#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "audio_core.h"
#include "config.h"
#include "encoder.h"
#include "ringbuffer.h"

extern _Atomic int active_track_count;

static pthread_t encoder_thread;
static volatile int keep_running = 0;
static int thread_is_active = 0;

static AVFormatContext *fmt_ctx = NULL;
static SPSC_Video_Queue *video_queue_ptr = NULL;

#define MAX_ENC_STREAMS (QJ_MAX_TRACKS + 1)
static AVCodecContext *codec_ctx_tracks[MAX_ENC_STREAMS] = {NULL};
static AVStream *audio_streams[MAX_ENC_STREAMS] = {NULL};
static int active_encoder_tracks = 1;

static AVStream *video_stream = NULL;

static pthread_mutex_t enc_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t enc_cond = PTHREAD_COND_INITIALIZER;
static _Atomic bool enc_is_sleeping = false;

// Unifying the video context to match encoder.h
AVCodecContext *video_enc_ctx = NULL;
static AVBufferRef *hw_device_ctx = NULL; // VAAPI Hardware Context
_Atomic bool encoder_disk_error = false;

/**
 * @brief Background thread loop for encoding audio and video frames.
 * @param arg Unused thread argument.
 * @return NULL upon completion.
 */
static void *encoder_loop(void *arg);

// Helper function to allocate GPU frames for the encoder
/**
 * @brief Allocates and initializes the VAAPI hardware frames context.
 * @param ctx The AVCodecContext to bind the frames context to.
 * @param hw_device_ctx The hardware device reference.
 * @param width The frame width.
 * @param height The frame height.
 * @return 0 on success, -1 on failure.
 */
static int set_hwframe_ctx(AVCodecContext *ctx, AVBufferRef *hw_device_ctx, int width, int height) {
    AVBufferRef *hw_frames_ref = av_hwframe_ctx_alloc(hw_device_ctx);
    if (!hw_frames_ref)
        return -1;

    AVHWFramesContext *frames_ctx = (AVHWFramesContext *)(hw_frames_ref->data);
    frames_ctx->format = AV_PIX_FMT_VAAPI;
    frames_ctx->sw_format = AV_PIX_FMT_NV12; // VAAPI standard software format
    frames_ctx->width = width;
    frames_ctx->height = height;
    frames_ctx->initial_pool_size = 20;

    if (av_hwframe_ctx_init(hw_frames_ref) < 0) {
        av_buffer_unref(&hw_frames_ref);
        return -1;
    }

    ctx->hw_frames_ctx = av_buffer_ref(hw_frames_ref);
    av_buffer_unref(&hw_frames_ref);
    return 0;
}

/**
 * @brief Initializes hardware/software encoding via discovery ladder and starts the background
 * thread.
 * @param output_filename The target output MKV file path.
 * @param width The width of the encoded video.
 * @param height The height of the encoded video.
 * @param sample_rate The dynamic hardware audio sample rate from JACK.
 * @param queue Pointer to the lock-free video frame queue.
 * @return 0 on success, -1 on failure.
 */
int init_and_start_encoder(const char *output_filename, int width, int height, int sample_rate,
                           SPSC_Video_Queue *queue) {
    video_queue_ptr = queue;
    atomic_store_explicit(&video_queue_ptr->read_index,
                          atomic_load_explicit(&video_queue_ptr->write_index, memory_order_relaxed),
                          memory_order_release);
    if (avformat_alloc_output_context2(&fmt_ctx, NULL, NULL, output_filename) < 0 || !fmt_ctx) {
        log_and_notify(0, "[Encoder] Error: Could not allocate output format context.\n");
        return -1;
    }
    const AVCodec *v_codec = NULL;
    int hw_accel_type = 0; // 0 = CPU, 1 = VAAPI, 2 = NVENC

    if (av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI, NULL, NULL, 0) == 0) {
        v_codec = avcodec_find_encoder_by_name("h264_vaapi");
        if (v_codec) {
            hw_accel_type = 1;
            log_and_notify(0, "[Encoder] Selected VAAPI Hardware Acceleration.\n");
        } else {
            av_buffer_unref(&hw_device_ctx);
            hw_device_ctx = NULL;
        }
    }

    if (!v_codec) {
        v_codec = avcodec_find_encoder_by_name("h264_nvenc");
        if (v_codec) {
            hw_accel_type = 2;
            log_and_notify(0, "[Encoder] Selected NVENC Hardware Acceleration.\n");
        }
    }

    if (!v_codec) {
        v_codec = avcodec_find_encoder(AV_CODEC_ID_H264);
        if (v_codec) {
            log_and_notify(0, "[Encoder] Warning: Hardware acceleration failed. Falling back to "
                              "default CPU H.264.\n");
        } else {
            log_and_notify(0, "[Encoder] Error: No suitable h264 encoder found.\n");
            goto encoder_cleanup;
        }
    }

    video_enc_ctx = avcodec_alloc_context3(v_codec);
    if (!video_enc_ctx) {
        log_and_notify(0, "[Encoder] Error: Could not allocate video encoder context.\n");
        goto encoder_cleanup;
    }

    video_enc_ctx->width = width;
    video_enc_ctx->height = height;
    video_enc_ctx->time_base = (AVRational){1, 30};
    video_enc_ctx->framerate = (AVRational){30, 1};

    // Force ALL-Intra recording so -c:v copy NLE trims are 100% frame-accurate
    video_enc_ctx->gop_size = 1;
    video_enc_ctx->max_b_frames = 0;

    if (hw_accel_type == 1) { // VAAPI
        video_enc_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
        video_enc_ctx->pix_fmt = AV_PIX_FMT_VAAPI;

        // Explicitly define Constant Quantization Parameter (CQP) mode and quality level
        av_opt_set(video_enc_ctx->priv_data, "rc_mode", "CQP", 0);
        av_opt_set(video_enc_ctx->priv_data, "qp", "20", 0);

        video_enc_ctx->compression_level = 20;
        set_hwframe_ctx(video_enc_ctx, hw_device_ctx, width, height);
    } else if (hw_accel_type == 2) { // NVENC
        video_enc_ctx->pix_fmt = AV_PIX_FMT_NV12;
        av_opt_set(video_enc_ctx->priv_data, "preset", "p4", 0);
    } else { // CPU
        video_enc_ctx->pix_fmt = AV_PIX_FMT_NV12;
        av_opt_set(video_enc_ctx->priv_data, "preset", "veryfast", 0);
        av_opt_set(video_enc_ctx->priv_data, "crf", "23", 0);
    }

    if (fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER) {
        video_enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    if (avcodec_open2(video_enc_ctx, v_codec, NULL) < 0) {
        if (hw_accel_type > 0) {
            log_and_notify(
                0,
                "[Encoder] Warning: Hardware codec failed to open. Falling back to CPU libx264.\n");
            avcodec_free_context(&video_enc_ctx);
            if (hw_device_ctx) {
                av_buffer_unref(&hw_device_ctx);
                hw_device_ctx = NULL;
            }

            v_codec = avcodec_find_encoder(AV_CODEC_ID_H264);
            if (!v_codec) {
                log_and_notify(0,
                               "[Encoder] Error: CPU fallback failed (no H.264 encoder found).\n");
                goto encoder_cleanup;
            }

            video_enc_ctx = avcodec_alloc_context3(v_codec);
            if (!video_enc_ctx) {
                log_and_notify(0, "[Encoder] Error: Could not allocate fallback context.\n");
                goto encoder_cleanup;
            }

            video_enc_ctx->width = width;
            video_enc_ctx->height = height;
            video_enc_ctx->time_base = (AVRational){1, 30};
            video_enc_ctx->framerate = (AVRational){30, 1};

            // Force ALL-Intra recording so -c:v copy NLE trims are 100% frame-accurate
            video_enc_ctx->gop_size = 1;
            video_enc_ctx->max_b_frames = 0;

            video_enc_ctx->pix_fmt = AV_PIX_FMT_NV12;
            av_opt_set(video_enc_ctx->priv_data, "preset", "veryfast", 0);
            av_opt_set(video_enc_ctx->priv_data, "crf", "23", 0);

            if (fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER) {
                video_enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            }

            if (avcodec_open2(video_enc_ctx, v_codec, NULL) < 0) {
                log_and_notify(0, "[Encoder] Error: Could not open fallback video codec.\n");
                goto encoder_cleanup;
            }
            hw_accel_type = 0;
        } else {
            log_and_notify(0, "[Encoder] Error: Could not open video codec.\n");
            goto encoder_cleanup;
        }
    }

    video_stream = avformat_new_stream(fmt_ctx, v_codec);
    if (!video_stream) {
        log_and_notify(0, "[Encoder] Error: Could not allocate video stream.\n");
        goto encoder_cleanup;
    }
    video_stream->time_base = video_enc_ctx->time_base;
    avcodec_parameters_from_context(video_stream->codecpar, video_enc_ctx);
    video_stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    video_stream->codecpar->codec_id = AV_CODEC_ID_H264;
    const AVCodec *a_codec = avcodec_find_encoder(AV_CODEC_ID_PCM_S16LE);
    if (!a_codec) {
        log_and_notify(0, "[Encoder] Error: PCM_S16LE audio encoder not found.\n");
        goto encoder_cleanup;
    }
    active_encoder_tracks = QJ_MAX_TRACKS;
    int total_streams = active_encoder_tracks + 1;
    for (int i = 0; i < total_streams; i++) {
        codec_ctx_tracks[i] = avcodec_alloc_context3(a_codec);
        if (!codec_ctx_tracks[i]) {
            log_and_notify(
                0, "[Encoder] Error: Could not allocate audio codec context for track %d.\n", i);
            goto encoder_cleanup;
        }
        codec_ctx_tracks[i]->sample_rate = sample_rate;
        av_channel_layout_default(&codec_ctx_tracks[i]->ch_layout, 2);
        codec_ctx_tracks[i]->sample_fmt = AV_SAMPLE_FMT_S16;
        codec_ctx_tracks[i]->time_base = (AVRational){1, sample_rate};
        if (fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER) {
            codec_ctx_tracks[i]->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }
        if (avcodec_open2(codec_ctx_tracks[i], a_codec, NULL) < 0) {
            log_and_notify(0, "[Encoder] Error: Could not open audio codec for track %d.\n", i);
            goto encoder_cleanup;
        }
        audio_streams[i] = avformat_new_stream(fmt_ctx, a_codec);
        if (!audio_streams[i]) {
            log_and_notify(0, "[Encoder] Error: Could not allocate audio stream for track %d.\n",
                           i);
            goto encoder_cleanup;
        }
        avcodec_parameters_from_context(audio_streams[i]->codecpar, codec_ctx_tracks[i]);

        // FORCE Matroska compliance for audio streams
        audio_streams[i]->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
        audio_streams[i]->codecpar->codec_id = AV_CODEC_ID_PCM_S16LE;
    }

    // --- 4. OPEN FILE & START THREAD ---
    if (avio_open(&fmt_ctx->pb, output_filename, AVIO_FLAG_WRITE) < 0) {
        log_and_notify(0, "[Encoder] Error: Could not open output file.\n");
        goto encoder_cleanup;
    }

    if (avformat_write_header(fmt_ctx, NULL) < 0) {
        log_and_notify(0, "[Encoder] Error: Could not write header.\n");
        goto encoder_cleanup;
    }

    atomic_store_explicit(&encoder_disk_error, false, memory_order_release);

    static bool cond_monotonic_init = false;
    if (!cond_monotonic_init) {
        pthread_condattr_t attr;
        if (pthread_condattr_init(&attr) == 0) {
            pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
            pthread_cond_init(&enc_cond, &attr);
            pthread_condattr_destroy(&attr);
            cond_monotonic_init = true;
        }
    }

    keep_running = 1;
    thread_is_active = 1;
    pthread_create(&encoder_thread, NULL, encoder_loop, NULL);

    log_and_notify(0, "[Encoder] Pipeline Initialized and Running at %d Hz.\n", sample_rate);
    return 0;

encoder_cleanup:
    if (fmt_ctx && fmt_ctx->pb)
        avio_closep(&fmt_ctx->pb);
    if (fmt_ctx) {
        avformat_free_context(fmt_ctx);
        fmt_ctx = NULL;
    }
    for (int i = 0; i < MAX_ENC_STREAMS; i++) {
        if (codec_ctx_tracks[i]) {
            avcodec_free_context(&codec_ctx_tracks[i]);
            codec_ctx_tracks[i] = NULL;
        }
    }
    if (video_enc_ctx) {
        avcodec_free_context(&video_enc_ctx);
        video_enc_ctx = NULL;
    }
    if (hw_device_ctx) {
        av_buffer_unref(&hw_device_ctx);
        hw_device_ctx = NULL;
    }
    return -1;
}

/**
 * @brief Safe wrapper for writing frames that intercepts physical disk capacity errors.
 */
static void safe_interleaved_write_frame(AVFormatContext *s, AVPacket *pkt) {
    if (av_interleaved_write_frame(s, pkt) < 0) {
        atomic_store_explicit(&encoder_disk_error, true, memory_order_release);
        usleep(100000);
    }
}

/**
 * @brief Drains available audio frames from the lock-free queue and encodes them to the MKV
 * container. Utilizes the hardware PTS injected by the JACK thread to prevent audio/video
 * desynchronization.
 * @param a_frame_mix The reusable AVFrame for the master mix stream.
 * @param pkt The reusable AVPacket used for writing.
 * @param first_a_pts Pointer to the baseline presentation timestamp.
 * @return 1 if frames were encoded, 0 if the queue lacked sufficient frames.
 */
/**
 * @brief Drains available audio frames from the lock-free queue and encodes them to the MKV
 * container.
 * @param a_frames Array of reusable AVFrames for the master mix and track streams.
 * @param pkt The reusable AVPacket used for writing.
 * @param current_audio_samples Pointer to the contiguous sample counter.
 * @param flush_partial If true, encodes any remaining partial batch (< frame_size) before shutdown.
 * @return 1 if frames were encoded, 0 if the queue lacked sufficient frames.
 */
static int process_audio_batch(AVFrame **a_frames, AVPacket *pkt, int64_t *current_audio_samples,
                               bool flush_partial) {
    size_t a_write = atomic_load_explicit(&audio_queue.write_index, memory_order_acquire);
    size_t a_read = atomic_load_explicit(&audio_queue.read_index, memory_order_relaxed);
    size_t avail = a_write - a_read;

    int max_samples = codec_ctx_tracks[0]->frame_size == 0 ? 1024 : codec_ctx_tracks[0]->frame_size;
    int nb_samples = max_samples;

    if (avail < (size_t)max_samples) {
        if (!flush_partial || avail == 0)
            return 0;
        nb_samples = (int)avail;
    }

    int total_streams = active_encoder_tracks + 1;
    for (int t = 0; t < total_streams; t++) {
        av_frame_make_writable(a_frames[t]);
        a_frames[t]->nb_samples = nb_samples;
    }

    for (int i = 0; i < nb_samples; i++) {
        AudioFrame f = {0};
        pop_audio_frame(&audio_queue, &f);

        // Stream 0: Master Mix
        int16_t *out_mix = (int16_t *)a_frames[0]->data[0];
        out_mix[i * 2] = (int16_t)(f.mix_l * 32767.0f);
        out_mix[i * 2 + 1] = (int16_t)(f.mix_r * 32767.0f);

        // Streams 1 to N: Isolated Tracks
        for (int t = 0; t < active_encoder_tracks; t++) {
            int16_t *out = (int16_t *)a_frames[t + 1]->data[0];
            out[i * 2] = (int16_t)(soft_clip(f.tracks_l[t]) * 32767.0f);
            out[i * 2 + 1] = (int16_t)(soft_clip(f.tracks_r[t]) * 32767.0f);
        }
    }

    for (int t = 0; t < total_streams; t++) {
        // ASSIGN STRICT CONTIGUOUS TIMESTAMPS
        a_frames[t]->pts = *current_audio_samples;

        if (avcodec_send_frame(codec_ctx_tracks[t], a_frames[t]) == 0) {
            while (avcodec_receive_packet(codec_ctx_tracks[t], pkt) == 0) {
                av_packet_rescale_ts(pkt, codec_ctx_tracks[t]->time_base,
                                     audio_streams[t]->time_base);
                pkt->stream_index = audio_streams[t]->index;
                safe_interleaved_write_frame(fmt_ctx, pkt);
                av_packet_unref(pkt);
            }
        }
    }

    // Increment the sample counter by the exact number of processed samples
    *current_audio_samples += nb_samples;
    return 1;
}

/**
 * @brief Pulls a single video frame from the queue, maps memory to the hardware context, and
 * encodes it.
 * @param v_frame_enc The reusable hardware encoding frame.
 * @param sw_frame The reusable software frame mapping payload memory.
 * @param pkt The reusable AVPacket.
 * @param first_v_pts Pointer to the baseline presentation timestamp.
 * @return 1 if a frame was processed, 0 if the queue was empty.
 */
static int process_video_frame(AVFrame *v_frame_enc, AVFrame *sw_frame, AVPacket *pkt,
                               int64_t *first_v_pts) {
    VideoPayload v_payload;
    if (!pop_video_frame(video_queue_ptr, &v_payload))
        return 0;

    if (*first_v_pts == -1)
        *first_v_pts = v_payload.pts;
    int64_t normalized_v_pts = v_payload.pts - *first_v_pts;

    av_image_fill_arrays(sw_frame->data, sw_frame->linesize, v_payload.data, AV_PIX_FMT_NV12,
                         sw_frame->width, sw_frame->height, 1);
    sw_frame->pts =
        av_rescale_q(normalized_v_pts, (AVRational){1, 1000000}, video_enc_ctx->time_base);

    if (video_enc_ctx->hw_frames_ctx) { // VAAPI
        if (av_hwframe_get_buffer(video_enc_ctx->hw_frames_ctx, v_frame_enc, 0) == 0) {
            v_frame_enc->pts = sw_frame->pts;
            if (av_hwframe_transfer_data(v_frame_enc, sw_frame, 0) == 0) {
                if (avcodec_send_frame(video_enc_ctx, v_frame_enc) == 0) {
                    while (avcodec_receive_packet(video_enc_ctx, pkt) == 0) {
                        av_packet_rescale_ts(pkt, video_enc_ctx->time_base,
                                             video_stream->time_base);
                        pkt->stream_index = video_stream->index;
                        safe_interleaved_write_frame(fmt_ctx, pkt);
                        av_packet_unref(pkt);
                    }
                }
            }
        }
        av_frame_unref(v_frame_enc);
    } else { // NVENC / CPU
        if (avcodec_send_frame(video_enc_ctx, sw_frame) == 0) {
            while (avcodec_receive_packet(video_enc_ctx, pkt) == 0) {
                av_packet_rescale_ts(pkt, video_enc_ctx->time_base, video_stream->time_base);
                pkt->stream_index = video_stream->index;
                safe_interleaved_write_frame(fmt_ctx, pkt);
                av_packet_unref(pkt);
            }
        }
    }

    return 1;
}

/**
 * @brief Flushes a specific encoder context to write any remaining buffered frames to disk.
 * @param ctx The target FFmpeg codec context.
 * @param stream The target output stream.
 * @param pkt Reusable packet for execution.
 */
static void flush_encoder_context(AVCodecContext *ctx, AVStream *stream, AVPacket *pkt) {
    if (!ctx || !stream)
        return;
    avcodec_send_frame(ctx, NULL);
    while (avcodec_receive_packet(ctx, pkt) == 0) {
        av_packet_rescale_ts(pkt, ctx->time_base, stream->time_base);
        pkt->stream_index = stream->index;
        safe_interleaved_write_frame(fmt_ctx, pkt);
        av_packet_unref(pkt);
    }
}

/**
 * @brief Background thread loop for encoding audio and video frames.
 * @param arg Unused thread argument.
 * @return NULL upon completion.
 */
static void *encoder_loop(void *arg) {
    (void)arg;

    int total_streams = active_encoder_tracks + 1;
    AVFrame *a_frames[MAX_ENC_STREAMS] = {NULL};
    for (int t = 0; t < total_streams; t++) {
        a_frames[t] = av_frame_alloc();
        if (!a_frames[t]) {
            log_and_notify(0, "[Encoder] Error: av_frame_alloc failed (audio track %d).\n", t);
            for (int x = 0; x < t; x++)
                av_frame_free(&a_frames[x]);
            return NULL;
        }
        a_frames[t]->nb_samples =
            codec_ctx_tracks[t]->frame_size == 0 ? 1024 : codec_ctx_tracks[t]->frame_size;
        a_frames[t]->format = codec_ctx_tracks[t]->sample_fmt;
        av_channel_layout_copy(&a_frames[t]->ch_layout, &codec_ctx_tracks[t]->ch_layout);
        if (av_frame_get_buffer(a_frames[t], 0) < 0) {
            log_and_notify(0, "[Encoder] Error: av_frame_get_buffer failed (audio track %d).\n", t);
            for (int x = 0; x <= t; x++)
                av_frame_free(&a_frames[x]);
            return NULL;
        }
    }
    AVFrame *v_frame_enc = av_frame_alloc();
    AVFrame *sw_frame = av_frame_alloc();
    AVPacket *shared_pkt = av_packet_alloc();

    if (!v_frame_enc || !sw_frame || !shared_pkt) {
        log_and_notify(0, "[Encoder] Error: frame/packet allocation failed (video).\n");
        for (int t = 0; t < total_streams; t++)
            av_frame_free(&a_frames[t]);
        av_frame_free(&v_frame_enc);
        av_frame_free(&sw_frame);
        av_packet_free(&shared_pkt);
        return NULL;
    }
    sw_frame->format = AV_PIX_FMT_NV12;
    sw_frame->width = video_enc_ctx->width;
    sw_frame->height = video_enc_ctx->height;

    int64_t current_audio_samples = 0; // REPLACES first_a_pts
    int64_t first_v_pts = -1;

    while (keep_running) {
        int activity = 0;

        while (process_audio_batch(a_frames, shared_pkt, &current_audio_samples, false)) {
            activity = 1;
        }

        while (process_video_frame(v_frame_enc, sw_frame, shared_pkt, &first_v_pts)) {
            activity = 1;
        }

        if (!activity) {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            ts.tv_nsec += 5000000;
            if (ts.tv_nsec >= 1000000000) {
                ts.tv_nsec -= 1000000000;
                ts.tv_sec++;
            }
            pthread_mutex_lock(&enc_mutex);
            atomic_store_explicit(&enc_is_sleeping, true, memory_order_release);
            pthread_cond_timedwait(&enc_cond, &enc_mutex, &ts);
            atomic_store_explicit(&enc_is_sleeping, false, memory_order_relaxed);
            pthread_mutex_unlock(&enc_mutex);
        }
    }

    // Drain any remaining queued audio (including final partial batch) and video frames before
    // closing
    while (process_audio_batch(a_frames, shared_pkt, &current_audio_samples, true)) {
    }
    while (process_video_frame(v_frame_enc, sw_frame, shared_pkt, &first_v_pts)) {
    }

    for (int t = 0; t < total_streams; t++) {
        flush_encoder_context(codec_ctx_tracks[t], audio_streams[t], shared_pkt);
    }

    if (first_v_pts != -1) {
        flush_encoder_context(video_enc_ctx, video_stream, shared_pkt);
    }

    av_write_trailer(fmt_ctx);

    for (int t = 0; t < total_streams; t++) {
        av_frame_free(&a_frames[t]);
    }
    av_frame_free(&v_frame_enc);
    av_frame_free(&sw_frame);
    av_packet_free(&shared_pkt);

    return NULL;
}

/**
 * @brief Wakes the encoder thread from sleep without waiting for a timeout.
 */
void wake_encoder(void) {
    if (atomic_load_explicit(&enc_is_sleeping, memory_order_acquire)) {
        if (pthread_mutex_trylock(&enc_mutex) == 0) {
            pthread_cond_signal(&enc_cond);
            pthread_mutex_unlock(&enc_mutex);
        }
    }
}

/**
 * @brief Signals the encoder thread to stop, flushes remaining frames, and frees contexts.
 * @return void
 */
void stop_encoder(void) {
    keep_running = 0;
    wake_encoder();
    if (thread_is_active) {
        pthread_join(encoder_thread, NULL);
        thread_is_active = 0;
    }

    // Drain abandoned ringbuffer memory
    if (video_queue_ptr) {
        VideoPayload stale_payload;
        while (pop_video_frame(video_queue_ptr, &stale_payload)) {
        }
    }

    if (fmt_ctx) {
        avio_closep(&fmt_ctx->pb);
        avformat_free_context(fmt_ctx);
        fmt_ctx = NULL;
    }

    for (int i = 0; i < MAX_ENC_STREAMS; i++) {
        if (codec_ctx_tracks[i]) {
            avcodec_free_context(&codec_ctx_tracks[i]);
            codec_ctx_tracks[i] = NULL;
        }
    }
    if (video_enc_ctx) {
        avcodec_free_context(&video_enc_ctx);
        video_enc_ctx = NULL;
    }
    if (hw_device_ctx) {
        av_buffer_unref(&hw_device_ctx);
        hw_device_ctx = NULL;
    }
}
