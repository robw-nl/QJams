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

#include "video_engine.h"
#include "audio_engine.h"
#include "scanner.h"
#include "ui_globals.h"

extern _Atomic bool is_mkv_mode;

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
static AVCodecContext *preview_dec_ctx = NULL;
static struct SwsContext *preview_sws_ctx = NULL;
static struct SwsContext *encode_sws_ctx = NULL;

static uint8_t* nv12_pool[64] = {0};
static uint8_t* rgba_pool[32] = {0};
static uint8_t* yuyv_cpu_cache = NULL;

// Active Hardware Format State
static uint32_t capture_v4l2_fmt = 0;
int capture_width = 1280;
int capture_height = 720;

/**
 * @brief Background thread loop for memory-mapped V4L2 capture and scaling.
 * Supports dynamic stream handling for both MJPEG (decoded) and YUYV (raw mapped).
 * @param arg Unused thread argument.
 * @return NULL upon completion.
 */
static void* video_capture_loop(void* arg) {
    (void)arg;
    struct v4l2_buffer buf;
    fd_set fds;
    struct timeval tv;

    int64_t total_pause_offset_us = 0;
    int64_t pause_start_time_us = 0;
    bool was_paused = false;

    // OPTIMIZATION: Lift FFmpeg allocations outside the 30FPS loop
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();

    while (keep_running) {
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        // select() allows us to cleanly exit the thread instead of hard-blocking on DQBUF
        int r = select(fd + 1, &fds, NULL, NULL, &tv);
        if (r == -1) continue;
        if (r == 0) continue; // Timeout

        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        if (ioctl(fd, VIDIOC_DQBUF, &buf) == -1) continue;

        // 1. Extract exact hardware timestamp from the V4L2 buffer
        int64_t pts = (int64_t)buf.timestamp.tv_sec * 1000000 + buf.timestamp.tv_usec;

        // 2. Zero-Copy: Feed V4L2 memory directly to the payload
        pkt->data = buffers[buf.index].start;
        pkt->size = buf.bytesused;

        // GUARD CLAUSES: Flatten the logic based on dynamically discovered format
        if (capture_v4l2_fmt == V4L2_PIX_FMT_MJPEG) {
            if (avcodec_send_packet(preview_dec_ctx, pkt) != 0) goto frame_cleanup;
            if (avcodec_receive_frame(preview_dec_ctx, frame) != 0) goto frame_cleanup;
            // Force hardware format override to silence SWS scaler deprecation warnings globally
            switch (frame->format) {
                case AV_PIX_FMT_YUVJ420P: frame->format = AV_PIX_FMT_YUV420P; break;
                case AV_PIX_FMT_YUVJ422P: frame->format = AV_PIX_FMT_YUV422P; break;
                case AV_PIX_FMT_YUVJ444P: frame->format = AV_PIX_FMT_YUV444P; break;
                default: break;
            }
        } else if (capture_v4l2_fmt == V4L2_PIX_FMT_YUYV) {
            // Fast block copy from V4L2 mmap to CPU RAM to avoid PIO thrashing in sws_scale
            if (yuyv_cpu_cache) memcpy(yuyv_cpu_cache, pkt->data, pkt->size);
            av_image_fill_arrays(frame->data, frame->linesize, yuyv_cpu_cache ? yuyv_cpu_cache : pkt->data, AV_PIX_FMT_YUYV422, capture_width, capture_height, 1);
            frame->width = capture_width;
            frame->height = capture_height;
            frame->format = AV_PIX_FMT_YUYV422;
        } else {
            goto frame_cleanup; // Unknown format bailout
        }

        if (!atomic_load_explicit(&is_mkv_mode, memory_order_relaxed)) {
            size_t p_w_idx = atomic_load_explicit(&preview_queue.write_index, memory_order_relaxed);
            size_t p_r_idx = atomic_load_explicit(&preview_queue.read_index, memory_order_acquire);
            if (p_w_idx - p_r_idx < preview_queue.capacity) {

                // Dynamically calculate proper height preserving aspect ratio for a locked 640px width
                int target_height = (int)(640.0 * ((double)frame->height / (double)frame->width));

                if (!preview_sws_ctx) {
                    preview_sws_ctx = sws_getContext(frame->width, frame->height, frame->format,
                                                     640, target_height, AV_PIX_FMT_RGBA,
                                                     SWS_FAST_BILINEAR, NULL, NULL, NULL);
                }

                uint8_t *rgba_buf = rgba_pool[p_w_idx & (preview_queue.capacity - 1)];
                uint8_t *dest_data[4] = { rgba_buf, NULL, NULL, NULL };
                int dest_linesize[4] = { 640 * 4, 0, 0, 0 };
                sws_scale(preview_sws_ctx, (const uint8_t * const *)frame->data, frame->linesize,
                          0, frame->height, dest_data, dest_linesize);
                push_preview_frame(&preview_queue, rgba_buf, 640, target_height);
            }
        }

        // --- ROUTE DECODED YUV TO ENCODER IF RECORDING ---
        if (!atomic_load_explicit(&engine_is_armed, memory_order_acquire)) {
            total_pause_offset_us = 0;
            was_paused = false;
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

        // Bypass sending frames to the encoder if Multi-Track Mode is active
        if (atomic_load_explicit(&is_multitrack_mode, memory_order_relaxed)) goto frame_cleanup;

        size_t e_w_idx = atomic_load_explicit(&target_queue->write_index, memory_order_relaxed);
        size_t e_r_idx = atomic_load_explicit(&target_queue->read_index, memory_order_acquire);
        if (e_w_idx - e_r_idx >= target_queue->capacity) goto frame_cleanup;

        // 3. Guarantee output is NV12 for direct VAAPI hardware mapping
        int nv12_size = av_image_get_buffer_size(AV_PIX_FMT_NV12, capture_width, capture_height, 1);
        uint8_t *nv12_payload = nv12_pool[e_w_idx & (target_queue->capacity - 1)];

        // Initialize the dedicated encoder scaler safely
        if (!encode_sws_ctx) {
            encode_sws_ctx = sws_getContext(frame->width, frame->height, frame->format,
                                            capture_width, capture_height, AV_PIX_FMT_NV12,
                                            SWS_FAST_BILINEAR, NULL, NULL, NULL);
        }

        uint8_t *dst_data[4];
        int dst_linesize[4];
        av_image_fill_arrays(dst_data, dst_linesize, nv12_payload, AV_PIX_FMT_NV12, capture_width, capture_height, 1);

        // Safely map and convert the camera's colorspace directly into the ringbuffer payload
        sws_scale(encode_sws_ctx, (const uint8_t * const *)frame->data, frame->linesize,
                  0, frame->height, dst_data, dst_linesize);

        push_video_frame(target_queue, nv12_payload, (size_t)nv12_size, adjusted_pts);
        wake_encoder();
        bool expected = false;
        atomic_compare_exchange_strong(&engine_is_recording, &expected, true);

        frame_cleanup:
        av_frame_unref(frame); // Unref clears pointers, safely releasing manual YUYV memory maps
        av_packet_unref(pkt);

        // 4. Immediately return the buffer to the kernel driver
        ioctl(fd, VIDIOC_QBUF, &buf);
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);
    return NULL;
}

/**
 * @brief Initializes the V4L2 device by querying the hardware for the highest supported format, then starts the video capture thread.
 * @param device_path The hardware path of the video device (e.g., /dev/video0).
 * @param queue Pointer to the lock-free queue for routing frames to the encoder.
 * @return 0 on success, -1 on failure.
 */
int init_and_start_video_engine(const char* device_path, SPSC_Video_Queue* queue) {
    target_queue = queue;

    // Deep probe the camera for its highest supported format (MJPEG or YUYV)
    if (!get_best_video_format(device_path, &capture_v4l2_fmt, &capture_width, &capture_height)) {
        printf("Warning: Probing failed, falling back to 1280x720 MJPEG.\n");
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
    }

    // Ensure allocations are strictly re-entrant for hardware hot-patch retries
    for (int i = 0; i < 64; i++) {
        if (nv12_pool[i]) free(nv12_pool[i]);
        nv12_pool[i] = malloc(capture_width * capture_height * 3 / 2);
    }
    for (int i = 0; i < 32; i++) {
        // We defer exact RGBA sizing to Phase 2, but allocate enough for a 1080p frame max to be safe
        if (rgba_pool[i]) free(rgba_pool[i]);
        rgba_pool[i] = malloc(1920 * 1080 * 4);
    }
    // Spin-wait to allow the kernel V4L2 driver to release the hardware lock
    // after the terminal diagnostic scanner probes and closes it.
    int retries = 5;
    while (retries > 0) {
        fd = open(device_path, O_RDWR | O_NONBLOCK, 0);
        if (fd != -1) break;
        usleep(100000);
        retries--;
    }

    if (fd == -1) {
        perror("Cannot open V4L2 device");
        return -1;
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

    printf("Video Engine Live: V4L2 Thread capturing %s at %dx%d 30FPS.\n",
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
    return -1;
}

/**
 * @brief Shuts down the video capture thread and releases V4L2 hardware resources.
 * @return void
 */
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

        // Drain the abandoned ringbuffer memory WITHOUT freeing the payload pointers,
        // as they are strictly owned and freed by the rgba_pool below.
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

        printf("Video Engine cleanly shutdown.\n");
    }
}
