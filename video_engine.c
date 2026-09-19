#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <linux/videodev2.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include "encoder.h"
#include "config.h"
#include "video_engine.h"
#include "audio_engine.h"
#include "scanner.h"
#include "ui_globals.h"

#define REQ_BUFFERS 4

struct v4l2_buffer_mapping {
    void *start;
    size_t length;
};

static int fd = -1;
static struct v4l2_buffer_mapping* buffers = NULL;
static pthread_t video_thread;
static volatile int keep_running = 0;
static SPSC_Video_Queue* target_queue = NULL;

extern SPSC_Preview_Queue preview_queue;
extern _Atomic bool mkv_video_keep_running;
static AVCodecContext *preview_dec_ctx = NULL;
static struct SwsContext *preview_sws_ctx = NULL;
static struct SwsContext *encode_sws_ctx = NULL;

static int cached_preview_w = 0, cached_preview_h = 0, cached_preview_fmt = -1;
static int cached_encode_w = 0, cached_encode_h = 0, cached_encode_fmt = -1;

static uint8_t* nv12_pool[64] = {0};
static uint8_t* rgba_pool[32] = {0};
static uint8_t* yuyv_cpu_cache = NULL;

static uint32_t capture_v4l2_fmt = 0;
int capture_width = 1280;
int capture_height = 720;

static void* video_capture_loop(void* arg) {
    (void)arg;
    struct v4l2_buffer buf;
    fd_set fds;
    struct timeval tv;

    int64_t total_pause_offset_us = 0;
    int64_t pause_start_time_us = 0;
    bool was_paused = false;

    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();

    while (keep_running) {
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int r = select(fd + 1, &fds, NULL, NULL, &tv);
        if (r == -1) continue;
        if (r == 0) continue;

        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        if (ioctl(fd, VIDIOC_DQBUF, &buf) == -1) continue;

        int64_t pts = (int64_t)buf.timestamp.tv_sec * 1000000 + buf.timestamp.tv_usec;

        pkt->data = buffers[buf.index].start;
        pkt->size = buf.bytesused;

        if (capture_v4l2_fmt == V4L2_PIX_FMT_MJPEG) {
            if (avcodec_send_packet(preview_dec_ctx, pkt) != 0) goto frame_cleanup;
            if (avcodec_receive_frame(preview_dec_ctx, frame) != 0) goto frame_cleanup;
            switch (frame->format) {
                case AV_PIX_FMT_YUVJ420P: frame->format = AV_PIX_FMT_YUV420P; break;
                case AV_PIX_FMT_YUVJ422P: frame->format = AV_PIX_FMT_YUV422P; break;
                case AV_PIX_FMT_YUVJ444P: frame->format = AV_PIX_FMT_YUV444P; break;
                default: break;
            }
        } else if (capture_v4l2_fmt == V4L2_PIX_FMT_YUYV) {
            if (yuyv_cpu_cache) memcpy(yuyv_cpu_cache, pkt->data, pkt->size);
            av_image_fill_arrays(frame->data, frame->linesize, yuyv_cpu_cache ? yuyv_cpu_cache : pkt->data, AV_PIX_FMT_YUYV422, capture_width, capture_height, 1);
            frame->width = capture_width;
            frame->height = capture_height;
            frame->format = AV_PIX_FMT_YUYV422;
        } else {
            goto frame_cleanup;
        }

        // --- CONTINUOUS PREVIEW MAPPING ---
        bool is_playing = atomic_load_explicit(&engine_is_playing, memory_order_acquire);
        bool is_recording = atomic_load_explicit(&engine_is_recording, memory_order_acquire);

        // Listen to the mathematical boundary flag, not the thread-alive flag
        bool mkv_is_rendering = atomic_load_explicit(&is_mkv_mode, memory_order_acquire);

        // Pause live camera preview ONLY if an MKV video is actively monopolizing the UI bounds
        if (!(is_playing && !is_recording && mkv_is_rendering)) {
            size_t p_w_idx = atomic_load_explicit(&preview_queue.write_index, memory_order_relaxed);
            size_t p_r_idx = atomic_load_explicit(&preview_queue.read_index, memory_order_acquire);
            if (p_w_idx - p_r_idx < preview_queue.capacity) {

                // Hard-lock the UI preview dimensions to prevent GTK layout expansion
                int target_w = 320;
                int target_h = (frame->width > 0) ? (int)((double)target_w * ((double)frame->height / (double)frame->width)) : 180;

                if (preview_sws_ctx && (frame->width != cached_preview_w || frame->height != cached_preview_h || frame->format != cached_preview_fmt)) {
                    sws_freeContext(preview_sws_ctx);
                    preview_sws_ctx = NULL;
                }

                if (!preview_sws_ctx) {
                    preview_sws_ctx = sws_getContext(frame->width, frame->height, frame->format,
                                                     target_w, target_h, AV_PIX_FMT_RGBA,
                                                     SWS_FAST_BILINEAR, NULL, NULL, NULL);
                    cached_preview_w = frame->width;
                    cached_preview_h = frame->height;
                    cached_preview_fmt = frame->format;
                }

                uint8_t *rgba_buf = rgba_pool[p_w_idx & (preview_queue.capacity - 1)];
                uint8_t *dest_data[4] = { rgba_buf, NULL, NULL, NULL };
                int dest_linesize[4] = { target_w * 4, 0, 0, 0 };

                sws_scale(preview_sws_ctx, (const uint8_t * const *)frame->data, frame->linesize,
                          0, frame->height, dest_data, dest_linesize);

                push_preview_frame(&preview_queue, rgba_buf, target_w, target_h);
            }
        }

        // --- ROUTE DECODED YUV TO ENCODER IF RECORDING AND ARMED ---
        if (!atomic_load_explicit(&engine_is_armed, memory_order_acquire)) {
            total_pause_offset_us = 0;
            was_paused = false;
            goto frame_cleanup;
        }

        if (!atomic_load_explicit(&video_is_armed, memory_order_acquire)) {
            goto frame_cleanup;
        }

        if (atomic_load_explicit(&engine_is_paused, memory_order_acquire)) {
            goto frame_cleanup;
        }

        if (atomic_load_explicit(&engine_is_paused, memory_order_acquire)) {
            if (!was_paused) {
                pause_start_time_us = pts;
                was_paused = true;
            }
            goto frame_cleanup;
        }

        if (was_paused) {
            total_pause_offset_us += (pts - pause_start_time_us);
            was_paused = false;
        }

        int64_t adjusted_pts = pts - total_pause_offset_us;

        size_t e_w_idx = atomic_load_explicit(&target_queue->write_index, memory_order_relaxed);
        size_t e_r_idx = atomic_load_explicit(&target_queue->read_index, memory_order_acquire);
        if (e_w_idx - e_r_idx >= target_queue->capacity) goto frame_cleanup;

        int nv12_size = av_image_get_buffer_size(AV_PIX_FMT_NV12, capture_width, capture_height, 1);
        uint8_t *nv12_payload = nv12_pool[e_w_idx & (target_queue->capacity - 1)];

        if (encode_sws_ctx && (frame->width != cached_encode_w || frame->height != cached_encode_h || frame->format != cached_encode_fmt)) {
            sws_freeContext(encode_sws_ctx);
            encode_sws_ctx = NULL;
        }

        if (!encode_sws_ctx) {
            encode_sws_ctx = sws_getContext(frame->width, frame->height, frame->format,
                                            capture_width, capture_height, AV_PIX_FMT_NV12,
                                            SWS_FAST_BILINEAR, NULL, NULL, NULL);
            cached_encode_w = frame->width;
            cached_encode_h = frame->height;
            cached_encode_fmt = frame->format;
        }

        uint8_t *dst_data[4];
        int dst_linesize[4];
        av_image_fill_arrays(dst_data, dst_linesize, nv12_payload, AV_PIX_FMT_NV12, capture_width, capture_height, 1);

        sws_scale(encode_sws_ctx, (const uint8_t * const *)frame->data, frame->linesize,
                  0, frame->height, dst_data, dst_linesize);

        push_video_frame(target_queue, nv12_payload, (size_t)nv12_size, adjusted_pts);
        wake_encoder();

        // (The atomic_compare_exchange_strong that was here has been moved up)

        frame_cleanup:
        av_frame_unref(frame);
        av_packet_unref(pkt);

        ioctl(fd, VIDIOC_QBUF, &buf);
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);
    return NULL;
}

int init_and_start_video_engine(const char* device_path, SPSC_Video_Queue* queue) {
    target_queue = queue;

    if (!get_best_video_format(device_path, &capture_v4l2_fmt, &capture_width, &capture_height)) {
        log_and_notify(0, "Warning: Probing failed, falling back to 1280x720 MJPEG.\n");
        capture_v4l2_fmt = V4L2_PIX_FMT_MJPEG;
        capture_width = 1280;
        capture_height = 720;
    }

    if (capture_v4l2_fmt == V4L2_PIX_FMT_MJPEG) {
        if (!preview_dec_ctx) {
            const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
            preview_dec_ctx = avcodec_alloc_context3(codec);
            avcodec_open2(preview_dec_ctx, codec, NULL);
        }
    } else if (capture_v4l2_fmt == V4L2_PIX_FMT_YUYV) {
        if (yuyv_cpu_cache) free(yuyv_cpu_cache);
        yuyv_cpu_cache = malloc(capture_width * capture_height * 2);
        if (!yuyv_cpu_cache) return -1;
    }

    int alloc_failed = 0;
    for (int i = 0; i < 64; i++) {
        if (nv12_pool[i]) free(nv12_pool[i]);
        nv12_pool[i] = malloc(capture_width * capture_height * 3 / 2);
        if (!nv12_pool[i]) {
            alloc_failed = 1;
            break;
        }
    }

    if (!alloc_failed) {
        for (int i = 0; i < 32; i++) {
            if (rgba_pool[i]) free(rgba_pool[i]);
            rgba_pool[i] = malloc(1920 * 1080 * 4);
            if (!rgba_pool[i]) {
                alloc_failed = 1;
                break;
            }
        }
    }

    if (alloc_failed) {
        for (int i = 0; i < 64; i++) { free(nv12_pool[i]); nv12_pool[i] = NULL; }
        for (int i = 0; i < 32; i++) { free(rgba_pool[i]); rgba_pool[i] = NULL; }
        if (yuyv_cpu_cache) { free(yuyv_cpu_cache); yuyv_cpu_cache = NULL; }
        return -1;
    }

    int retries = 5;
    while (retries > 0) {
        fd = open(device_path, O_RDWR | O_NONBLOCK, 0);
        if (fd != -1) break;
        usleep(100000);
        retries--;
    }

    if (fd == -1) {
        perror("Cannot open V4L2 device");
        goto v4l2_init_fail;
    }

    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = capture_width;
    fmt.fmt.pix.height = capture_height;
    fmt.fmt.pix.pixelformat = capture_v4l2_fmt;
    fmt.fmt.pix.field = V4L2_FIELD_ANY;

    if (ioctl(fd, VIDIOC_S_FMT, &fmt) == -1) {
        perror("VIDIOC_S_FMT failed");
        goto v4l2_init_fail;
    }

    struct v4l2_streamparm parm;
    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = 30;
    ioctl(fd, VIDIOC_S_PARM, &parm);

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = REQ_BUFFERS;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (ioctl(fd, VIDIOC_REQBUFS, &req) == -1) {
        goto v4l2_init_fail;
    }

    buffers = calloc(req.count, sizeof(*buffers));
    for (unsigned int i = 0; i < req.count; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (ioctl(fd, VIDIOC_QUERYBUF, &buf) == -1) {
            goto v4l2_init_fail;
        }

        buffers[i].length = buf.length;
        buffers[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);

        if (buffers[i].start == MAP_FAILED) {
            perror("mmap failed");
            for (unsigned int j = 0; j < i; j++) munmap(buffers[j].start, buffers[j].length);
            goto v4l2_init_fail;
        }

        ioctl(fd, VIDIOC_QBUF, &buf);
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_STREAMON, &type) == -1) {
        goto v4l2_init_fail;
    }

    log_and_notify(0, "Video Engine Live: V4L2 Thread capturing %s at %dx%d 30FPS.\n",
                   capture_v4l2_fmt == V4L2_PIX_FMT_MJPEG ? "MJPEG" : "YUYV", capture_width, capture_height);

    keep_running = 1;
    pthread_create(&video_thread, NULL, video_capture_loop, NULL);
    return 0;

    v4l2_init_fail:
    if (buffers) {
        free(buffers);
        buffers = NULL;
    }
    if (fd != -1) {
        close(fd);
        fd = -1;
    }
    for (int i = 0; i < 64; i++) { free(nv12_pool[i]); nv12_pool[i] = NULL; }
    for (int i = 0; i < 32; i++) { free(rgba_pool[i]); rgba_pool[i] = NULL; }
    if (yuyv_cpu_cache) { free(yuyv_cpu_cache); yuyv_cpu_cache = NULL; }
    return -1;
}

void stop_video_engine() {
    keep_running = 0;
    if (fd != -1) {
        pthread_join(video_thread, NULL);

        if (preview_dec_ctx) {
            avcodec_free_context(&preview_dec_ctx);
            preview_dec_ctx = NULL;
        }
        if (preview_sws_ctx) { sws_freeContext(preview_sws_ctx); preview_sws_ctx = NULL; }
        if (encode_sws_ctx) { sws_freeContext(encode_sws_ctx); encode_sws_ctx = NULL; }

        PreviewPayload p_payload;
        while (pop_preview_frame(&preview_queue, &p_payload)) { }

        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(fd, VIDIOC_STREAMOFF, &type);

        if (buffers) {
            for (int i = 0; i < REQ_BUFFERS; i++) {
                munmap(buffers[i].start, buffers[i].length);
            }
            free(buffers);
            buffers = NULL;
        }
        close(fd);
        fd = -1;

        for (int i = 0; i < 64; i++) { free(nv12_pool[i]); nv12_pool[i] = NULL; }
        for (int i = 0; i < 32; i++) { free(rgba_pool[i]); rgba_pool[i] = NULL; }
        if (yuyv_cpu_cache) { free(yuyv_cpu_cache); yuyv_cpu_cache = NULL; }

        log_and_notify(0, "Video Engine cleanly shutdown.\n");
    }
}
