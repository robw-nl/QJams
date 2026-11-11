#include "video_engine.h"
#include "audio_core.h"
#include "config.h"
#include "encoder.h"
#include "scanner.h"
#include "ui_globals.h"
#include <errno.h>
#include <fcntl.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <linux/videodev2.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>

#define REQ_BUFFERS 4

struct v4l2_buffer_mapping {
    void *start;
    size_t length;
};

static int fd = -1;
static struct v4l2_buffer_mapping *buffers = NULL;
static unsigned int actual_v4l2_buffers = 0;
static pthread_t video_thread;

/**
 * @brief Flag indicating if the V4L2 hardware was successfully initialized.
 */
static _Atomic bool camera_detected = false;

/**
 * @brief Flag indicating if the video thread is active. Must be accessed atomically.
 */
static _Atomic bool video_thread_active = false;

/**
 * @brief Loop condition for the video thread. Must be accessed atomically.
 */
static _Atomic int keep_running = 0;
static SPSC_Video_Queue *target_queue = NULL;

extern _Atomic bool mkv_video_keep_running;
static AVCodecContext *preview_dec_ctx = NULL;
static struct SwsContext *preview_sws_ctx = NULL;
static struct SwsContext *encode_sws_ctx = NULL;

static int cached_preview_w = 0, cached_preview_h = 0, cached_preview_fmt = -1;
static int cached_preview_out_w = 0;
static int cached_encode_w = 0, cached_encode_h = 0, cached_encode_fmt = -1;

static uint8_t *nv12_pool[64] = {0};
static uint8_t *rgba_pool[32] = {0};
static uint8_t *yuyv_cpu_cache = NULL;

static uint32_t capture_v4l2_fmt = 0;
int capture_width = 1280;
int capture_height = 720;

// --- MKV PLAYBACK STATE ---
static pthread_t mkv_video_thread;
_Atomic bool mkv_video_keep_running = false;
static uint8_t *mkv_rgba_pool[32] = {NULL};
extern char stashed_raw_path[1024];

static pthread_mutex_t mkv_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t mkv_cond = PTHREAD_COND_INITIALIZER;
static char mkv_target_path[1024] = {0};
static bool mkv_path_changed = false;

static void *mkv_video_loop(void *arg);

/**
 * @brief Checks if a V4L2 camera was detected during initialization.
 * @return true if camera is present, false otherwise.
 */
bool is_camera_detected(void) {
    return atomic_load_explicit(&camera_detected, memory_order_acquire);
}

/**
 * @brief Checks if the V4L2 capture thread is actively running.
 * @return true if the video thread is active, false otherwise.
 */
bool is_video_engine_running(void) {
    return atomic_load_explicit(&video_thread_active, memory_order_acquire);
}

/**
 * @brief Checks if an MKV video file is actively occupying the preview window at the current
 * playhead.
 * @return true if an MKV target path is active and playback_pos is within [v_start, v_end), false
 * otherwise.
 */
bool is_mkv_preview_active(void) {
    pthread_mutex_lock(&mkv_mutex);
    bool has_target = (mkv_target_path[0] != '\0');
    pthread_mutex_unlock(&mkv_mutex);
    if (!has_target)
        return false;
    size_t v_start = atomic_load_explicit(&video_start_frame, memory_order_acquire);
    size_t v_end = atomic_load_explicit(&video_end_frame, memory_order_acquire);
    size_t pos = atomic_load_explicit(&get_transport_state()->playback_pos, memory_order_acquire);
    return (v_end > v_start && pos >= v_start && pos < v_end);
}

static void *video_capture_loop(void *arg) {
    (void)arg;
    struct v4l2_buffer buf;
    fd_set fds;
    struct timeval tv;

    int64_t total_pause_offset_us = 0;
    int64_t pause_start_time_us = 0;
    bool was_paused = false;

    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    if (!pkt || !frame) {
        log_and_notify(0, "[Video] Error: capture packet/frame allocation failed.\n");
        return NULL;
    }

    while (atomic_load_explicit(&keep_running, memory_order_acquire)) {
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int r = select(fd + 1, &fds, NULL, NULL, &tv);
        if (r == -1)
            continue;
        if (r == 0)
            continue;

        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        if (ioctl(fd, VIDIOC_DQBUF, &buf) == -1)
            continue;

        if ((buf.flags & V4L2_BUF_FLAG_ERROR) || buf.bytesused < 4) {
            ioctl(fd, VIDIOC_QBUF, &buf);
            continue;
        }

        int64_t pts = (int64_t)buf.timestamp.tv_sec * 1000000 + buf.timestamp.tv_usec;

        pkt->data = buffers[buf.index].start;
        pkt->size = buf.bytesused;

        if (capture_v4l2_fmt == V4L2_PIX_FMT_MJPEG) {
            const uint8_t *raw = (const uint8_t *)pkt->data;
            /* Verify valid JPEG SOI (0xFF 0xD8) header before invoking libavcodec */
            if (pkt->size < 16 || raw[0] != 0xFF || raw[1] != 0xD8) {
                goto frame_cleanup;
            }
            if (avcodec_send_packet(preview_dec_ctx, pkt) != 0)
                goto frame_cleanup;
            if (avcodec_receive_frame(preview_dec_ctx, frame) != 0)
                goto frame_cleanup;
            switch (frame->format) {
                case AV_PIX_FMT_YUVJ420P:
                    frame->format = AV_PIX_FMT_YUV420P;
                    break;
                case AV_PIX_FMT_YUVJ422P:
                    frame->format = AV_PIX_FMT_YUV422P;
                    break;
                case AV_PIX_FMT_YUVJ444P:
                    frame->format = AV_PIX_FMT_YUV444P;
                    break;
                default:
                    break;
            }
        } else if (capture_v4l2_fmt == V4L2_PIX_FMT_YUYV) {
            size_t max_cache_size = (size_t)(capture_width * capture_height * 2);
            size_t safe_pkt_size = (pkt->size > 0) ? (size_t)pkt->size : 0;
            size_t copy_size = (safe_pkt_size > max_cache_size) ? max_cache_size : safe_pkt_size;
            if (yuyv_cpu_cache)
                memcpy(yuyv_cpu_cache, pkt->data, copy_size);
            av_image_fill_arrays(frame->data, frame->linesize,
                                 yuyv_cpu_cache ? yuyv_cpu_cache : pkt->data, AV_PIX_FMT_YUYV422,
                                 capture_width, capture_height, 1);
            frame->width = capture_width;
            frame->height = capture_height;
            frame->format = AV_PIX_FMT_YUYV422;
        } else {
            goto frame_cleanup;
        }

        // --- CONTINUOUS PREVIEW MAPPING ---

        bool block_webcam = false;
        pthread_mutex_lock(&mkv_mutex);
        bool has_mkv_target = (mkv_target_path[0] != '\0');
        pthread_mutex_unlock(&mkv_mutex);
        if (has_mkv_target && video_end_frame > video_start_frame) {
            size_t pos =
                atomic_load_explicit(&get_transport_state()->playback_pos, memory_order_acquire);
            if (pos >= video_start_frame && pos < video_end_frame) {
                block_webcam = true;
            }
        }

        // Pause live camera preview ONLY if an MKV video is actively monopolizing the UI bounds
        if (!block_webcam && frame->width > 0 && frame->height > 0) {
            size_t p_w_idx = atomic_load_explicit(&preview_queue.write_index, memory_order_relaxed);
            size_t p_r_idx = atomic_load_explicit(&preview_queue.read_index, memory_order_acquire);
            if (p_w_idx - p_r_idx < preview_queue.capacity) {

                int target_w = atomic_load_explicit(&is_video_expanded, memory_order_acquire)
                                   ? ui_state.config.video_width_expanded
                                   : ui_state.config.video_width_default;
                if (target_w < 16)
                    target_w = 480;
                if (target_w > 1920)
                    target_w = 1920;

                int target_h =
                    (int)((double)target_w * ((double)frame->height / (double)frame->width));
                if (target_h < 16)
                    target_h = 16;
                if (target_h > 1080)
                    target_h = 1080;

                if (preview_sws_ctx &&
                    (frame->width != cached_preview_w || frame->height != cached_preview_h ||
                     frame->format != cached_preview_fmt || target_w != cached_preview_out_w)) {
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
                    cached_preview_out_w = target_w;
                }

                uint8_t *rgba_buf = rgba_pool[p_w_idx & (preview_queue.capacity - 1)];
                uint8_t *dest_data[4] = {rgba_buf, NULL, NULL, NULL};
                int dest_linesize[4] = {target_w * 4, 0, 0, 0};

                sws_scale(preview_sws_ctx, (const uint8_t *const *)frame->data, frame->linesize, 0,
                          frame->height, dest_data, dest_linesize);

                push_preview_frame(&preview_queue, rgba_buf, target_w, target_h);
            }
        }

        // --- ROUTE DECODED YUV TO ENCODER IF RECORDING AND ARMED ---
        if (!atomic_load_explicit(&get_transport_state()->is_armed, memory_order_acquire)) {
            total_pause_offset_us = 0;
            was_paused = false;
            goto frame_cleanup;
        }

        if (!atomic_load_explicit(&video_is_armed, memory_order_acquire)) {
            goto frame_cleanup;
        }

        if (atomic_load_explicit(&get_transport_state()->is_paused, memory_order_acquire)) {
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
        if (e_w_idx - e_r_idx >= target_queue->capacity)
            goto frame_cleanup;

        int nv12_size = av_image_get_buffer_size(AV_PIX_FMT_NV12, capture_width, capture_height, 1);
        uint8_t *nv12_payload = nv12_pool[e_w_idx & (target_queue->capacity - 1)];

        if (encode_sws_ctx &&
            (frame->width != cached_encode_w || frame->height != cached_encode_h ||
             frame->format != cached_encode_fmt)) {
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
        av_image_fill_arrays(dst_data, dst_linesize, nv12_payload, AV_PIX_FMT_NV12, capture_width,
                             capture_height, 1);

        sws_scale(encode_sws_ctx, (const uint8_t *const *)frame->data, frame->linesize, 0,
                  frame->height, dst_data, dst_linesize);

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

int init_and_start_video_engine(const char *device_path, SPSC_Video_Queue *queue) {
    target_queue = queue;

    if (!atomic_exchange_explicit(&mkv_video_keep_running, true, memory_order_acquire)) {
        pthread_condattr_t attr;
        if (pthread_condattr_init(&attr) == 0) {
            pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
            pthread_cond_init(&mkv_cond, &attr);
            pthread_condattr_destroy(&attr);
        }
        pthread_create(&mkv_video_thread, NULL, mkv_video_loop, NULL);
    }

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
        if (yuyv_cpu_cache)
            free(yuyv_cpu_cache);
        yuyv_cpu_cache = malloc(capture_width * capture_height * 2);
        if (!yuyv_cpu_cache)
            return -1;
    }

    int alloc_failed = 0;
    for (int i = 0; i < 64; i++) {
        if (nv12_pool[i])
            free(nv12_pool[i]);
        nv12_pool[i] = malloc(capture_width * capture_height * 3 / 2);
        if (!nv12_pool[i]) {
            alloc_failed = 1;
            break;
        }
    }

    if (!alloc_failed) {
        for (int i = 0; i < 32; i++) {
            if (!rgba_pool[i]) {
                rgba_pool[i] = malloc(1920 * 1080 * 4);
            }
            if (!rgba_pool[i]) {
                alloc_failed = 1;
                break;
            }
        }
    }

    if (alloc_failed) {
        for (int i = 0; i < 64; i++) {
            free(nv12_pool[i]);
            nv12_pool[i] = NULL;
        }
        for (int i = 0; i < 32; i++) {
            free(rgba_pool[i]);
            rgba_pool[i] = NULL;
        }
        if (yuyv_cpu_cache) {
            free(yuyv_cpu_cache);
            yuyv_cpu_cache = NULL;
        }
        return -1;
    }

    int retries = 5;
    while (retries > 0) {
        fd = open(device_path, O_RDWR | O_NONBLOCK, 0);
        if (fd != -1)
            break;
        usleep(100000);
        retries--;
    }

    if (fd == -1) {
        atomic_store_explicit(&camera_detected, false, memory_order_release);
        log_and_notify(0, "[Video Engine] No camera detected. Initializing playback-only mode.\n");
        return 0;
    }

    atomic_store_explicit(&camera_detected, true, memory_order_release);

    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = capture_width;
    fmt.fmt.pix.height = capture_height;
    fmt.fmt.pix.pixelformat = capture_v4l2_fmt;
    fmt.fmt.pix.field = V4L2_FIELD_ANY;

    if (ioctl(fd, VIDIOC_S_FMT, &fmt) == -1) {
        log_and_notify(0, "[Video Engine] VIDIOC_S_FMT failed: %s\n", strerror(errno));
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
        log_and_notify(0, "[Video Engine] VIDIOC_REQBUFS failed: %s\n", strerror(errno));
        goto v4l2_init_fail;
    }

    actual_v4l2_buffers = req.count;
    buffers = calloc(actual_v4l2_buffers, sizeof(*buffers));
    for (unsigned int i = 0; i < actual_v4l2_buffers; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (ioctl(fd, VIDIOC_QUERYBUF, &buf) == -1) {
            log_and_notify(0, "[Video Engine] VIDIOC_QUERYBUF failed: %s\n", strerror(errno));
            goto v4l2_init_fail;
        }

        buffers[i].length = buf.length;
        buffers[i].start =
            mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);

        if (buffers[i].start == MAP_FAILED) {
            buffers[i].start = NULL;
            log_and_notify(0, "[Video Engine] mmap failed: %s\n", strerror(errno));
            goto v4l2_init_fail;
        }

        ioctl(fd, VIDIOC_QBUF, &buf);
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_STREAMON, &type) == -1) {
        log_and_notify(0, "[Video Engine] VIDIOC_STREAMON failed: %s\n", strerror(errno));
        goto v4l2_init_fail;
    }

    log_and_notify(0, "Video Engine Live: V4L2 Thread capturing %s at %dx%d 30FPS.\n",
                   capture_v4l2_fmt == V4L2_PIX_FMT_MJPEG ? "MJPEG" : "YUYV", capture_width,
                   capture_height);

    atomic_store_explicit(&keep_running, 1, memory_order_release);
    atomic_store_explicit(&video_thread_active, true, memory_order_release);
    pthread_create(&video_thread, NULL, video_capture_loop, NULL);
    return 0;

v4l2_init_fail:
    atomic_store_explicit(&camera_detected, false, memory_order_release);
    if (buffers) {
        for (unsigned int i = 0; i < actual_v4l2_buffers; i++) {
            if (buffers[i].start && buffers[i].start != MAP_FAILED) {
                munmap(buffers[i].start, buffers[i].length);
            }
        }
        free(buffers);
        buffers = NULL;
        actual_v4l2_buffers = 0;
    }
    if (fd != -1) {
        close(fd);
        fd = -1;
    }
    if (preview_dec_ctx) {
        avcodec_free_context(&preview_dec_ctx);
        preview_dec_ctx = NULL;
    }
    for (int i = 0; i < 64; i++) {
        free(nv12_pool[i]);
        nv12_pool[i] = NULL;
    }
    for (int i = 0; i < 32; i++) {
        free(rgba_pool[i]);
        rgba_pool[i] = NULL;
    }
    if (yuyv_cpu_cache) {
        free(yuyv_cpu_cache);
        yuyv_cpu_cache = NULL;
    }
    return -1;
}

/**
 * @brief Stops only the V4L2 capture thread and releases camera hardware resources,
 *        keeping the MKV video preview decoder thread alive for playback-only mode.
 */
void stop_v4l2_capture(void) {
    atomic_store_explicit(&keep_running, 0, memory_order_release);
    atomic_store_explicit(&camera_detected, false, memory_order_release);

    if (fd != -1) {
        if (atomic_load_explicit(&video_thread_active, memory_order_acquire)) {
            pthread_join(video_thread, NULL);
            atomic_store_explicit(&video_thread_active, false, memory_order_release);
        }

        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(fd, VIDIOC_STREAMOFF, &type);

        if (buffers) {
            for (unsigned int i = 0; i < actual_v4l2_buffers; i++) {
                if (buffers[i].start && buffers[i].start != MAP_FAILED) {
                    munmap(buffers[i].start, buffers[i].length);
                }
            }
            free(buffers);
            buffers = NULL;
            actual_v4l2_buffers = 0;
        }
        close(fd);
        fd = -1;
    }

    if (preview_dec_ctx) {
        avcodec_free_context(&preview_dec_ctx);
        preview_dec_ctx = NULL;
    }
    if (preview_sws_ctx) {
        sws_freeContext(preview_sws_ctx);
        preview_sws_ctx = NULL;
    }
    if (encode_sws_ctx) {
        sws_freeContext(encode_sws_ctx);
        encode_sws_ctx = NULL;
    }

    for (int i = 0; i < 64; i++) {
        free(nv12_pool[i]);
        nv12_pool[i] = NULL;
    }
    if (yuyv_cpu_cache) {
        free(yuyv_cpu_cache);
        yuyv_cpu_cache = NULL;
    }
}

/**
 * @brief Shuts down both the V4L2 capture thread and MKV preview decoder thread, releasing all
 * memory pools.
 */
void stop_video_engine(void) {
    stop_v4l2_capture();

    if (atomic_exchange_explicit(&mkv_video_keep_running, false, memory_order_release)) {
        pthread_mutex_lock(&mkv_mutex);
        pthread_cond_signal(&mkv_cond);
        pthread_mutex_unlock(&mkv_mutex);
        pthread_join(mkv_video_thread, NULL);
    }

    PreviewPayload p_payload;
    while (pop_preview_frame(&preview_queue, &p_payload)) {
    }

    if (atomic_load_explicit(&engine_is_shutting_down, memory_order_acquire)) {
        for (int i = 0; i < 32; i++) {
            free(rgba_pool[i]);
            rgba_pool[i] = NULL;
            if (mkv_rgba_pool[i]) {
                free(mkv_rgba_pool[i]);
                mkv_rgba_pool[i] = NULL;
            }
        }
    }

    log_and_notify(0, "Video Engine cleanly shutdown.\n");
}

static void *mkv_video_loop(void *arg) {
    (void)arg;
    if (!mkv_rgba_pool[0]) {
        for (int i = 0; i < 32; i++)
            mkv_rgba_pool[i] = malloc(1920 * 1080 * 4);
    }
    while (atomic_load_explicit(&mkv_video_keep_running, memory_order_acquire)) {
        char current_filepath[1024];

        pthread_mutex_lock(&mkv_mutex);
        while (!mkv_path_changed && mkv_target_path[0] == '\0' &&
               atomic_load_explicit(&mkv_video_keep_running, memory_order_acquire)) {
            pthread_cond_wait(&mkv_cond, &mkv_mutex);
        }
        if (!atomic_load_explicit(&mkv_video_keep_running, memory_order_acquire)) {
            pthread_mutex_unlock(&mkv_mutex);
            break;
        }
        strncpy(current_filepath, mkv_target_path, sizeof(current_filepath) - 1);
        current_filepath[sizeof(current_filepath) - 1] = '\0';
        mkv_path_changed = false;
        pthread_mutex_unlock(&mkv_mutex);

        if (current_filepath[0] == '\0') {
            continue;
        }

        AVFormatContext *fmt_ctx = NULL;
        AVDictionary *opts = NULL;
        av_dict_set(&opts, "probesize", "1048576", 0);
        av_dict_set(&opts, "analyzeduration", "1000000", 0);

        if (avformat_open_input(&fmt_ctx, current_filepath, NULL, &opts) != 0) {
            av_dict_free(&opts);
            continue;
        }
        av_dict_free(&opts);

        avformat_find_stream_info(fmt_ctx, NULL);

        int v_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
        if (v_idx < 0) {
            avformat_close_input(&fmt_ctx);
            continue;
        }

        AVCodecContext *v_ctx = avcodec_alloc_context3(NULL);
        avcodec_parameters_to_context(v_ctx, fmt_ctx->streams[v_idx]->codecpar);

        v_ctx->thread_count = 4;
        v_ctx->thread_type = FF_THREAD_SLICE;

        v_ctx->pkt_timebase = fmt_ctx->streams[v_idx]->time_base;

        const AVCodec *v_codec = avcodec_find_decoder(v_ctx->codec_id);
        if (!v_ctx || !v_codec) {
            if (v_ctx)
                avcodec_free_context(&v_ctx);
            avformat_close_input(&fmt_ctx);
            continue;
        }

        if (avcodec_open2(v_ctx, v_codec, NULL) < 0) {
            avcodec_free_context(&v_ctx);
            avformat_close_input(&fmt_ctx);
            continue;
        }

        AVPacket *pkt = av_packet_alloc();
        AVFrame *frame = av_frame_alloc();
        if (!pkt || !frame) {
            av_packet_free(&pkt);
            av_frame_free(&frame);
            avcodec_free_context(&v_ctx);
            avformat_close_input(&fmt_ctx);
            continue;
        }
        struct SwsContext *sws_ctx = NULL;
        int mkv_sws_target_w = 0;
        AVRational time_base = fmt_ctx->streams[v_idx]->time_base;
        double last_audio_time = -1.0;
        bool eof_flushed = false;

        bool is_live_take = false;
        if (strcmp(current_filepath, current_raw_path) == 0) {
            is_live_take = true;
        }

        bool abort_file = false;
        size_t last_rendered_pos = SIZE_MAX;
        size_t last_sought_pos = SIZE_MAX;
        bool last_rendered_expanded =
            atomic_load_explicit(&is_video_expanded, memory_order_acquire);
        bool pending_parked_frame = false;

        while (!abort_file && atomic_load_explicit(&mkv_video_keep_running, memory_order_acquire)) {
            pthread_mutex_lock(&mkv_mutex);
            if (mkv_path_changed) {
                abort_file = true;
                pthread_mutex_unlock(&mkv_mutex);
                break;
            }
            pthread_mutex_unlock(&mkv_mutex);

            int current_rate = atomic_load_explicit(&get_transport_state()->active_sample_rate,
                                                    memory_order_acquire);
            if (current_rate == 0) {
                usleep(10000);
                continue;
            }

            bool is_playing =
                atomic_load_explicit(&get_transport_state()->is_playing, memory_order_acquire);
            bool is_paused =
                atomic_load_explicit(&get_transport_state()->is_paused, memory_order_acquire);
            size_t pos =
                atomic_load_explicit(&get_transport_state()->playback_pos, memory_order_acquire);
            size_t v_start = atomic_load_explicit(&video_start_frame, memory_order_acquire);
            size_t v_end = atomic_load_explicit(&video_end_frame, memory_order_acquire);
            if (v_end <= v_start || pos < v_start || pos >= v_end) {
                usleep(10000);
                continue;
            }
            bool cur_expanded = atomic_load_explicit(&is_video_expanded, memory_order_acquire);
            bool size_changed = (cur_expanded != last_rendered_expanded);
            bool is_actively_moving = (is_playing && !is_paused);

            if (!is_actively_moving && pos == last_rendered_pos && !size_changed &&
                !pending_parked_frame) {
                usleep(10000);
                continue;
            }

            if (size_changed) {
                last_rendered_expanded = cur_expanded;
                last_sought_pos = SIZE_MAX;
            }

            double current_audio_time = (double)pos / current_rate;

            double offset_sec = (double)video_start_frame / current_rate;

            double local_audio_time = current_audio_time - offset_sec;

            if (local_audio_time < 0.0) {
                usleep(10000);
                continue;
            }

            static float last_seen_speed = 1.0f;
            float current_speed_check =
                atomic_load_explicit(&get_transport_state()->playback_speed, memory_order_relaxed);
            bool speed_changed = (current_speed_check != last_seen_speed);
            last_seen_speed = current_speed_check;

            bool need_parked_seek = (!is_actively_moving && pos != last_sought_pos);

            if (need_parked_seek || last_audio_time < 0.0 ||
                fabs(local_audio_time - last_audio_time) > 0.25 || speed_changed) {
                double target_mkv_time =
                    local_audio_time * (current_speed_check > 0.0f ? current_speed_check : 1.0f);
                av_seek_frame(fmt_ctx, v_idx, (int64_t)(target_mkv_time / av_q2d(time_base)),
                              AVSEEK_FLAG_BACKWARD);
                avcodec_flush_buffers(v_ctx);
                eof_flushed = false;
                last_sought_pos = pos;
                if (!is_actively_moving) {
                    pending_parked_frame = true;
                }
            }
            last_audio_time = local_audio_time;

            // video_engine.c

            int read_ret = av_read_frame(fmt_ctx, pkt);
            if (read_ret >= 0 || !eof_flushed) {
                bool should_decode = false;
                if (read_ret >= 0) {
                    if (pkt->stream_index == v_idx) {
                        bool skip_decode =
                            is_live_take &&
                            atomic_load_explicit(&get_transport_state()->is_recording,
                                                 memory_order_acquire) &&
                            atomic_load_explicit(&video_is_armed, memory_order_acquire);

                        if (!skip_decode) {
                            double raw_pkt_time = 0.0;
                            if (pkt->pts != AV_NOPTS_VALUE) {
                                raw_pkt_time = pkt->pts * av_q2d(time_base);
                            } else if (pkt->dts != AV_NOPTS_VALUE) {
                                raw_pkt_time = pkt->dts * av_q2d(time_base);
                            }

                            float spd = atomic_load_explicit(&get_transport_state()->playback_speed,
                                                             memory_order_relaxed);
                            double check_time = raw_pkt_time / (spd > 0.0f ? spd : 1.0f);

                            // Use decoder-level non-ref discard during catch-up to preserve P/IDR
                            // references
                            if (check_time < last_audio_time - 0.05) {
                                v_ctx->skip_frame = AVDISCARD_NONREF;
                            } else {
                                v_ctx->skip_frame = AVDISCARD_DEFAULT;
                            }

                            if (avcodec_send_packet(v_ctx, pkt) == 0) {
                                should_decode = true;
                            }
                        }
                    }
                } else {
                    eof_flushed = true;
                    v_ctx->skip_frame = AVDISCARD_DEFAULT;
                    if (avcodec_send_packet(v_ctx, NULL) == 0) {
                        should_decode = true;
                    }
                }

                if (should_decode) {
                    bool seek_interrupted = false;
                    while (!seek_interrupted && avcodec_receive_frame(v_ctx, frame) == 0) {
                        int64_t best_ts = (frame->best_effort_timestamp != AV_NOPTS_VALUE)
                                              ? frame->best_effort_timestamp
                                              : frame->pts;
                        double raw_frame_time =
                            (best_ts != AV_NOPTS_VALUE) ? (best_ts * av_q2d(time_base)) : 0.0;
                        double frame_time = 0.0;
                        double sync_last_audio = local_audio_time;
                        float init_spd = atomic_load_explicit(
                            &get_transport_state()->playback_speed, memory_order_relaxed);
                        frame_time = raw_frame_time / (init_spd > 0.0f ? init_spd : 1.0f);

                        // Immediately drop pre-seek catch-up frames before sleep/SWS scaling
                        if (is_actively_moving && frame_time < local_audio_time - 0.04) {
                            av_frame_unref(frame);
                            continue;
                        }
                        if (!is_actively_moving && pending_parked_frame &&
                            frame_time < local_audio_time - 0.04) {
                            av_frame_unref(frame);
                            continue;
                        }

                        while (
                            is_actively_moving && !abort_file &&
                            atomic_load_explicit(&mkv_video_keep_running, memory_order_acquire)) {
                            pthread_mutex_lock(&mkv_mutex);
                            if (mkv_path_changed) {
                                abort_file = true;
                                pthread_mutex_unlock(&mkv_mutex);
                                break;
                            }
                            pthread_mutex_unlock(&mkv_mutex);

                            if (!atomic_load_explicit(&get_transport_state()->is_playing,
                                                      memory_order_acquire) ||
                                atomic_load_explicit(&get_transport_state()->is_paused,
                                                     memory_order_acquire)) {
                                is_actively_moving = false;
                                break;
                            }

                            float current_speed = atomic_load_explicit(
                                &get_transport_state()->playback_speed, memory_order_relaxed);
                            frame_time =
                                raw_frame_time / (current_speed > 0.0f ? current_speed : 1.0f);
                            size_t cur_pos = atomic_load_explicit(
                                &get_transport_state()->playback_pos, memory_order_acquire);
                            double inner_audio_time = (double)cur_pos / current_rate;
                            double inner_local_time = inner_audio_time - offset_sec;

                            // Detect both backward AND forward timeline jumps immediately
                            if (fabs(inner_local_time - sync_last_audio) > 0.20) {
                                seek_interrupted = true;
                                break;
                            }
                            sync_last_audio = inner_local_time;
                            last_audio_time = inner_local_time;
                            double time_diff = frame_time - inner_local_time;
                            if (time_diff <= 0.02)
                                break;
                            long long delay_ns = (long long)((time_diff - 0.02) * 1000000000LL);
                            if (delay_ns < 1000000LL)
                                delay_ns = 1000000LL;
                            if (delay_ns > 10000000LL)
                                delay_ns = 10000000LL;
                            struct timespec now, target;
                            clock_gettime(CLOCK_MONOTONIC, &now);
                            target.tv_sec = now.tv_sec + (delay_ns / 1000000000LL);
                            target.tv_nsec = now.tv_nsec + (delay_ns % 1000000000LL);
                            if (target.tv_nsec >= 1000000000LL) {
                                target.tv_sec++;
                                target.tv_nsec -= 1000000000LL;
                            }

                            pthread_mutex_lock(&mkv_mutex);
                            pthread_cond_timedwait(&mkv_cond, &mkv_mutex, &target);
                            pthread_mutex_unlock(&mkv_mutex);
                        }

                        if (seek_interrupted) {
                            av_frame_unref(frame);
                            break;
                        }

                        if (!is_actively_moving) {
                            float current_speed = atomic_load_explicit(
                                &get_transport_state()->playback_speed, memory_order_relaxed);
                            frame_time =
                                raw_frame_time / (current_speed > 0.0f ? current_speed : 1.0f);
                        }

                        if (frame_time < last_audio_time - 0.1 || frame->width <= 0 ||
                            frame->height <= 0)
                            continue;

                        int target_w =
                            atomic_load_explicit(&is_video_expanded, memory_order_acquire)
                                ? ui_state.config.video_width_expanded
                                : ui_state.config.video_width_default;
                        if (target_w < 16)
                            target_w = 480;
                        if (target_w > 1280)
                            target_w = 1280;
                        int target_h = (int)(target_w * (9.0 / 16.0));
                        if (target_h < 16)
                            target_h = 16;
                        if (target_h > 720)
                            target_h = 720;

                        double src_aspect = (double)frame->width / (double)frame->height;
                        double target_aspect = (double)target_w / (double)target_h;
                        int scaled_w = target_w;
                        int scaled_h = target_h;
                        if (src_aspect > target_aspect)
                            scaled_h = (int)((double)target_w / src_aspect);
                        else
                            scaled_w = (int)((double)target_h * src_aspect);

                        if (scaled_w < 2)
                            scaled_w = 2;
                        if (scaled_w > target_w)
                            scaled_w = target_w;
                        if (scaled_h < 2)
                            scaled_h = 2;
                        if (scaled_h > target_h)
                            scaled_h = target_h;

                        if (sws_ctx && mkv_sws_target_w != target_w) {
                            sws_freeContext(sws_ctx);
                            sws_ctx = NULL;
                        }

                        if (!sws_ctx) {
                            sws_ctx = sws_getContext(frame->width, frame->height, frame->format,
                                                     scaled_w, scaled_h, AV_PIX_FMT_RGBA,
                                                     SWS_FAST_BILINEAR, NULL, NULL, NULL);
                            mkv_sws_target_w = target_w;
                        }

                        size_t p_w_idx =
                            atomic_load_explicit(&preview_queue.write_index, memory_order_relaxed);
                        size_t p_r_idx =
                            atomic_load_explicit(&preview_queue.read_index, memory_order_acquire);
                        if (sws_ctx && (p_w_idx - p_r_idx < preview_queue.capacity)) {
                            uint8_t *rgba_buf = mkv_rgba_pool[p_w_idx & 31];
                            if (rgba_buf) {
                                memset(rgba_buf, 0, (size_t)target_w * (size_t)target_h * 4);
                                int x_offset = (target_w - scaled_w) / 2;
                                int y_offset = (target_h - scaled_h) / 2;
                                if (x_offset < 0)
                                    x_offset = 0;
                                if (y_offset < 0)
                                    y_offset = 0;
                                uint8_t *dest_data[4] = {rgba_buf +
                                                             (y_offset * target_w + x_offset) * 4,
                                                         NULL, NULL, NULL};
                                int dest_linesize[4] = {target_w * 4, 0, 0, 0};
                                sws_scale(sws_ctx, (const uint8_t *const *)frame->data,
                                          frame->linesize, 0, frame->height, dest_data,
                                          dest_linesize);
                                pthread_mutex_lock(&mkv_mutex);
                                bool valid_to_push =
                                    (!mkv_path_changed && mkv_target_path[0] != '\0' &&
                                     atomic_load_explicit(&video_end_frame, memory_order_acquire) >
                                         atomic_load_explicit(&video_start_frame,
                                                              memory_order_acquire));
                                pthread_mutex_unlock(&mkv_mutex);
                                if (valid_to_push) {
                                    push_preview_frame(&preview_queue, rgba_buf, target_w,
                                                       target_h);
                                    last_rendered_pos = pos;
                                    pending_parked_frame = false;
                                } else {
                                    abort_file = true;
                                    break;
                                }
                                if (!is_actively_moving)
                                    break;
                            }
                        }
                    }
                }
                if (read_ret >= 0) {
                    av_packet_unref(pkt);
                }
            } else {
                size_t standby_pos = atomic_load_explicit(&get_transport_state()->playback_pos,
                                                          memory_order_acquire);
                bool standby_expanded =
                    atomic_load_explicit(&is_video_expanded, memory_order_acquire);

                while (!abort_file &&
                       atomic_load_explicit(&mkv_video_keep_running, memory_order_acquire)) {
                    pthread_mutex_lock(&mkv_mutex);
                    if (mkv_path_changed) {
                        abort_file = true;
                        pthread_mutex_unlock(&mkv_mutex);
                        break;
                    }
                    pthread_mutex_unlock(&mkv_mutex);

                    size_t current_pos = atomic_load_explicit(&get_transport_state()->playback_pos,
                                                              memory_order_acquire);
                    bool current_expanded =
                        atomic_load_explicit(&is_video_expanded, memory_order_acquire);
                    int rate = atomic_load_explicit(&get_transport_state()->active_sample_rate,
                                                    memory_order_acquire);
                    if (rate <= 0)
                        rate = 48000;

                    if (current_pos < standby_pos || current_pos > standby_pos + (rate / 2) ||
                        current_expanded != standby_expanded) {
                        float current_speed = atomic_load_explicit(
                            &get_transport_state()->playback_speed, memory_order_relaxed);
                        double new_local_time = ((double)current_pos / rate) - offset_sec;

                        if (current_expanded != standby_expanded && new_local_time > 0.1) {
                            new_local_time -= 0.1;
                        }

                        double target_mkv_time =
                            new_local_time * (current_speed > 0.0f ? current_speed : 1.0f);

                        av_seek_frame(fmt_ctx, v_idx,
                                      (int64_t)(target_mkv_time / av_q2d(time_base)),
                                      AVSEEK_FLAG_BACKWARD);
                        avcodec_flush_buffers(v_ctx);
                        eof_flushed = false;

                        last_audio_time = new_local_time;
                        break;
                    }
                    standby_pos = current_pos;
                    standby_expanded = current_expanded;

                    pthread_mutex_lock(&mkv_mutex);
                    struct timespec ts;
                    clock_gettime(CLOCK_MONOTONIC, &ts);
                    ts.tv_nsec += 10000000;
                    if (ts.tv_nsec >= 1000000000) {
                        ts.tv_sec++;
                        ts.tv_nsec -= 1000000000;
                    }
                    pthread_cond_timedwait(&mkv_cond, &mkv_mutex, &ts);
                    pthread_mutex_unlock(&mkv_mutex);
                }
            }
        }

        avcodec_free_context(&v_ctx);
        avformat_close_input(&fmt_ctx);

        if (sws_ctx)
            sws_freeContext(sws_ctx);
        av_packet_free(&pkt);
        av_frame_free(&frame);
    }

    return NULL;
}

void launch_mkv_video_preview(const char *filepath) {
    pthread_mutex_lock(&mkv_mutex);
    strncpy(mkv_target_path, filepath, sizeof(mkv_target_path) - 1);
    mkv_target_path[sizeof(mkv_target_path) - 1] = '\0';
    mkv_path_changed = true;
    pthread_cond_signal(&mkv_cond);
    pthread_mutex_unlock(&mkv_mutex);
}

static void clear_stash_thread(GTask *task, gpointer source_object, gpointer task_data,
                               GCancellable *cancellable) {
    (void)source_object;
    (void)cancellable;
    (void)task;
    char *path = (char *)task_data;
    remove(path);
}

void clear_video_stash_async(void) {
    if (strlen(stashed_raw_path) > 0) {
        GTask *task = g_task_new(NULL, NULL, NULL, NULL);
        g_task_set_task_data(task, g_strdup(stashed_raw_path), g_free);
        g_task_run_in_thread(task, clear_stash_thread);
        g_object_unref(task);
        stashed_raw_path[0] = '\0';
    }
}

void abort_active_video_preview(bool stash) {

    pthread_mutex_lock(&mkv_mutex);
    if (strcmp(mkv_target_path, current_raw_path) == 0) {
        mkv_target_path[0] = '\0';
        mkv_path_changed = true;
        pthread_cond_signal(&mkv_cond);
    }
    pthread_mutex_unlock(&mkv_mutex);

    PreviewPayload stale_payload;
    while (pop_preview_frame(&preview_queue, &stale_payload)) {
    }

    if (strlen(current_raw_path) > 0) {
        if (stash) {
            snprintf(stashed_raw_path, sizeof(stashed_raw_path), "%s.stash", current_raw_path);
            rename(current_raw_path, stashed_raw_path);
        } else {
            remove(current_raw_path);
        }
        current_raw_path[0] = '\0';
    }
}

void force_clear_mkv_video_preview(void) {
    pthread_mutex_lock(&mkv_mutex);
    mkv_target_path[0] = '\0';
    mkv_path_changed = true;
    pthread_cond_signal(&mkv_cond);
    pthread_mutex_unlock(&mkv_mutex);

    PreviewPayload stale_payload;
    while (pop_preview_frame(&preview_queue, &stale_payload)) {
    }
}

void restore_stashed_video_preview(void) {
    if (strlen(stashed_raw_path) > 0 && access(stashed_raw_path, F_OK) == 0) {
        char restored_path[1024];
        strncpy(restored_path, stashed_raw_path, sizeof(restored_path) - 1);
        char *ext = strstr(restored_path, ".stash");
        if (ext)
            *ext = '\0';

        rename(stashed_raw_path, restored_path);
        strncpy(current_raw_path, restored_path, sizeof(current_raw_path) - 1);
        stashed_raw_path[0] = '\0';

        launch_mkv_video_preview(current_raw_path);
    }
}
