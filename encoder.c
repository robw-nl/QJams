#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>

#include "ringbuffer.h"
#include "encoder.h"

extern SPSC_Audio_Queue audio_queue;

static pthread_t encoder_thread;
static volatile int keep_running = 0;
static int thread_is_active = 0;

static AVFormatContext *fmt_ctx = NULL;
static AVCodecContext *codec_ctx_input = NULL;
static AVCodecContext *codec_ctx_bt = NULL;
static SPSC_Video_Queue* video_queue_ptr = NULL;

static AVStream *audio_stream_input = NULL;
static AVStream *audio_stream_bt = NULL;
static AVStream *video_stream = NULL;

static pthread_mutex_t enc_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t enc_cond = PTHREAD_COND_INITIALIZER;

// Unifying the video context to match encoder.h
AVCodecContext *video_enc_ctx = NULL;
static AVBufferRef *hw_device_ctx = NULL; // VAAPI Hardware Context

/**
 * @brief Background thread loop for encoding audio and video frames.
 * @param arg Unused thread argument.
 * @return NULL upon completion.
 */
static void* encoder_loop(void* arg);

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
    if (!hw_frames_ref) return -1;

    AVHWFramesContext *frames_ctx = (AVHWFramesContext *)(hw_frames_ref->data);
    frames_ctx->format    = AV_PIX_FMT_VAAPI;
    frames_ctx->sw_format = AV_PIX_FMT_NV12; // VAAPI standard software format
    frames_ctx->width     = width;
    frames_ctx->height    = height;
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
 * @brief Initializes hardware/software encoding via discovery ladder and starts the background thread.
 * @param output_filename The target output MKV file path.
 * @param width The width of the encoded video.
 * @param height The height of the encoded video.
 * @param sample_rate The dynamic hardware audio sample rate from JACK.
 * @param queue Pointer to the lock-free video frame queue.
 * @return 0 on success, -1 on failure.
 */
int init_and_start_encoder(const char* output_filename, int width, int height, int sample_rate, SPSC_Video_Queue* queue) {
    video_queue_ptr = queue;

    // --- 1. FORMAT CONTEXT ---
    avformat_alloc_output_context2(&fmt_ctx, NULL, NULL, output_filename);

    // --- 2. VIDEO STREAM (DISCOVERY LADDER) ---
    const AVCodec *v_codec = NULL;
    int hw_accel_type = 0; // 0 = CPU, 1 = VAAPI, 2 = NVENC

    if (av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI, NULL, NULL, 0) == 0) {
        v_codec = avcodec_find_encoder_by_name("h264_vaapi");
        if (v_codec) {
            hw_accel_type = 1;
            printf("[Encoder] Selected VAAPI Hardware Acceleration.\n");
        } else {
            av_buffer_unref(&hw_device_ctx);
            hw_device_ctx = NULL;
        }
    }

    if (!v_codec) {
        v_codec = avcodec_find_encoder_by_name("h264_nvenc");
        if (v_codec) {
            hw_accel_type = 2;
            printf("[Encoder] Selected NVENC Hardware Acceleration.\n");
        }
    }

    if (!v_codec) {
        v_codec = avcodec_find_encoder_by_name("libx264");
        if (v_codec) {
            printf("[Encoder] Warning: Hardware acceleration failed. Falling back to CPU libx264.\n");
        } else {
            printf("[Encoder] Error: No suitable h264 encoder found.\n");
            goto encoder_cleanup;
        }
    }

    video_enc_ctx = avcodec_alloc_context3(v_codec);
    if (!video_enc_ctx) {
        printf("[Encoder] Error: Could not allocate video encoder context.\n");
        goto encoder_cleanup;
    }

    video_enc_ctx->width = width;
    video_enc_ctx->height = height;
    video_enc_ctx->time_base = (AVRational){1, 30};
    video_enc_ctx->framerate = (AVRational){30, 1};

    if (hw_accel_type == 1) { // VAAPI
        video_enc_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
        video_enc_ctx->pix_fmt = AV_PIX_FMT_VAAPI;
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
    avcodec_open2(video_enc_ctx, v_codec, NULL);

    video_stream = avformat_new_stream(fmt_ctx, v_codec);
    video_stream->time_base = video_enc_ctx->time_base;
    avcodec_parameters_from_context(video_stream->codecpar, video_enc_ctx);
    video_stream->codecpar->format = AV_PIX_FMT_NV12; // Muxer expects NV12

    // --- 3. AUDIO STREAMS (PCM 16-BIT INTERLEAVED) ---
    // PCM is extremely CPU efficient for raw capture and natively guarantees S16 interleaved support
    const AVCodec *a_codec = avcodec_find_encoder(AV_CODEC_ID_PCM_S16LE);

    // STREAM 0: Backing Track (RAW 1)
    codec_ctx_bt = avcodec_alloc_context3(a_codec);
    codec_ctx_bt->sample_rate = sample_rate;
    av_channel_layout_default(&codec_ctx_bt->ch_layout, 2);
    codec_ctx_bt->sample_fmt = AV_SAMPLE_FMT_S16;
    codec_ctx_bt->time_base = (AVRational){1, sample_rate};
    avcodec_open2(codec_ctx_bt, a_codec, NULL);
    audio_stream_bt = avformat_new_stream(fmt_ctx, a_codec);
    avcodec_parameters_from_context(audio_stream_bt->codecpar, codec_ctx_bt);

    // STREAM 1: Quad Cortex Input (RAW 2)
    codec_ctx_input = avcodec_alloc_context3(a_codec);
    codec_ctx_input->sample_rate = sample_rate;
    av_channel_layout_default(&codec_ctx_input->ch_layout, 2);
    codec_ctx_input->sample_fmt = AV_SAMPLE_FMT_S16;
    codec_ctx_input->time_base = (AVRational){1, sample_rate};
    avcodec_open2(codec_ctx_input, a_codec, NULL);
    audio_stream_input = avformat_new_stream(fmt_ctx, a_codec);
    avcodec_parameters_from_context(audio_stream_input->codecpar, codec_ctx_input);

    // --- 4. OPEN FILE & START THREAD ---
    if (avio_open(&fmt_ctx->pb, output_filename, AVIO_FLAG_WRITE) < 0) {
        printf("[Encoder] Error: Could not open output file.\n");
        goto encoder_cleanup;
    }

    if (avformat_write_header(fmt_ctx, NULL) < 0) {
        printf("[Encoder] Error: Could not write header.\n");
        goto encoder_cleanup;
    }

    keep_running = 1;
    thread_is_active = 1;
    pthread_create(&encoder_thread, NULL, encoder_loop, NULL);

    printf("[Encoder] Pipeline Initialized and Running at %d Hz.\n", sample_rate);
    return 0;

    encoder_cleanup:
    if (fmt_ctx && fmt_ctx->pb) avio_closep(&fmt_ctx->pb);
    if (fmt_ctx) { avformat_free_context(fmt_ctx); fmt_ctx = NULL; }
    if (codec_ctx_input) { avcodec_free_context(&codec_ctx_input); codec_ctx_input = NULL; }
    if (codec_ctx_bt) { avcodec_free_context(&codec_ctx_bt); codec_ctx_bt = NULL; }
    if (video_enc_ctx) { avcodec_free_context(&video_enc_ctx); video_enc_ctx = NULL; }
    if (hw_device_ctx) { av_buffer_unref(&hw_device_ctx); hw_device_ctx = NULL; }
    return -1;
}

/**
 * @brief Drains available audio frames from the lock-free queue and encodes them to the MKV container.
 * @param a_frame_input The reusable AVFrame for the input stream.
 * @param a_frame_bt The reusable AVFrame for the backing track stream.
 * @param pkt The reusable AVPacket used for writing.
 * @param a_pts Pointer to the running presentation timestamp.
 * @return 1 if frames were encoded, 0 if the queue lacked sufficient frames.
 */
static int process_audio_batch(AVFrame *a_frame_input, AVFrame *a_frame_bt, AVPacket *pkt, int64_t *a_pts) {
    size_t a_write = atomic_load_explicit(&audio_queue.write_index, memory_order_acquire);
    size_t a_read = atomic_load_explicit(&audio_queue.read_index, memory_order_relaxed);

    if ((a_write - a_read) < (size_t)a_frame_input->nb_samples) return 0;

    av_frame_make_writable(a_frame_input);
    av_frame_make_writable(a_frame_bt);

    int16_t *out_input = (int16_t*)a_frame_input->data[0];
    int16_t *out_bt = (int16_t*)a_frame_bt->data[0];

    for (int i = 0; i < a_frame_input->nb_samples; i++) {
        AudioFrame f = {0};
        pop_audio_frame(&audio_queue, &f);

        out_input[i * 2]     = (int16_t)(f.input_l * 32767.0f);
        out_input[i * 2 + 1] = (int16_t)(f.input_r * 32767.0f);
        out_bt[i * 2]        = (int16_t)(f.bt_l * 32767.0f);
        out_bt[i * 2 + 1]    = (int16_t)(f.bt_r * 32767.0f);
    }

    a_frame_input->pts = *a_pts;
    a_frame_bt->pts = *a_pts;
    *a_pts += a_frame_input->nb_samples;

    if (avcodec_send_frame(codec_ctx_input, a_frame_input) == 0) {
        while (avcodec_receive_packet(codec_ctx_input, pkt) == 0) {
            av_packet_rescale_ts(pkt, codec_ctx_input->time_base, audio_stream_input->time_base);
            pkt->stream_index = audio_stream_input->index;
            av_interleaved_write_frame(fmt_ctx, pkt);
            av_packet_unref(pkt);
        }
    }

    if (avcodec_send_frame(codec_ctx_bt, a_frame_bt) == 0) {
        while (avcodec_receive_packet(codec_ctx_bt, pkt) == 0) {
            av_packet_rescale_ts(pkt, codec_ctx_bt->time_base, audio_stream_bt->time_base);
            pkt->stream_index = audio_stream_bt->index;
            av_interleaved_write_frame(fmt_ctx, pkt);
            av_packet_unref(pkt);
        }
    }
    return 1;
}

/**
* @brief Pulls a single video frame from the queue, maps memory to the hardware context, and encodes it.
* @param v_frame_enc The reusable hardware encoding frame.
* @param sw_frame The reusable software frame mapping payload memory.
* @param pkt The reusable AVPacket.
* @param first_v_pts Pointer to the baseline presentation timestamp.
* @return 1 if a frame was processed, 0 if the queue was empty.
*/
static int process_video_frame(AVFrame *v_frame_enc, AVFrame *sw_frame, AVPacket *pkt, int64_t *first_v_pts) {
    VideoPayload v_payload;
    if (!pop_video_frame(video_queue_ptr, &v_payload)) return 0;

    if (*first_v_pts == -1) *first_v_pts = v_payload.pts;
    int64_t normalized_v_pts = v_payload.pts - *first_v_pts;

    av_image_fill_arrays(sw_frame->data, sw_frame->linesize, v_payload.data,
                         AV_PIX_FMT_NV12, sw_frame->width, sw_frame->height, 1);
    sw_frame->pts = av_rescale_q(normalized_v_pts, (AVRational){1, 1000000}, video_enc_ctx->time_base);

    if (video_enc_ctx->hw_frames_ctx) { // VAAPI
        if (av_hwframe_get_buffer(video_enc_ctx->hw_frames_ctx, v_frame_enc, 0) == 0) {
            v_frame_enc->pts = sw_frame->pts;
            if (av_hwframe_transfer_data(v_frame_enc, sw_frame, 0) == 0) {
                if (avcodec_send_frame(video_enc_ctx, v_frame_enc) == 0) {
                    while (avcodec_receive_packet(video_enc_ctx, pkt) == 0) {
                        av_packet_rescale_ts(pkt, video_enc_ctx->time_base, video_stream->time_base);
                        pkt->stream_index = video_stream->index;
                        av_interleaved_write_frame(fmt_ctx, pkt);
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
                av_interleaved_write_frame(fmt_ctx, pkt);
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
    if (!ctx || !stream) return;
    avcodec_send_frame(ctx, NULL);
    while (avcodec_receive_packet(ctx, pkt) == 0) {
        av_packet_rescale_ts(pkt, ctx->time_base, stream->time_base);
        pkt->stream_index = stream->index;
        av_interleaved_write_frame(fmt_ctx, pkt);
        av_packet_unref(pkt);
    }
}

/**
 * @brief Background thread loop for encoding audio and video frames.
 * @param arg Unused thread argument.
 * @return NULL upon completion.
 */
static void* encoder_loop(void* arg) {
    (void)arg;

    AVFrame *a_frame_input = av_frame_alloc();
    a_frame_input->nb_samples = codec_ctx_input->frame_size == 0 ? 1024 : codec_ctx_input->frame_size;
    a_frame_input->format = codec_ctx_input->sample_fmt;
    av_channel_layout_copy(&a_frame_input->ch_layout, &codec_ctx_input->ch_layout);
    av_frame_get_buffer(a_frame_input, 0);

    AVFrame *a_frame_bt = av_frame_alloc();
    a_frame_bt->nb_samples = codec_ctx_bt->frame_size == 0 ? 1024 : codec_ctx_bt->frame_size;
    a_frame_bt->format = codec_ctx_bt->sample_fmt;
    av_channel_layout_copy(&a_frame_bt->ch_layout, &codec_ctx_bt->ch_layout);
    av_frame_get_buffer(a_frame_bt, 0);

    AVFrame *v_frame_enc = av_frame_alloc();
    AVFrame *sw_frame = av_frame_alloc();
    sw_frame->format = AV_PIX_FMT_NV12;
    sw_frame->width = video_enc_ctx->width;
    sw_frame->height = video_enc_ctx->height;

    AVPacket *shared_pkt = av_packet_alloc(); // Reused uniformly for all packet submissions

    int64_t a_pts = 0;
    int64_t first_v_pts = -1;

    while (keep_running) {
        int activity = 0;

        while (process_audio_batch(a_frame_input, a_frame_bt, shared_pkt, &a_pts)) {
            activity = 1;
        }

        if (process_video_frame(v_frame_enc, sw_frame, shared_pkt, &first_v_pts)) {
            activity = 1;
        }

        if (!activity) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 5000000; // 5ms timeout fallback
            if (ts.tv_nsec >= 1000000000) { ts.tv_nsec -= 1000000000; ts.tv_sec++; }
            pthread_mutex_lock(&enc_mutex);
            pthread_cond_timedwait(&enc_cond, &enc_mutex, &ts);
            pthread_mutex_unlock(&enc_mutex);
        }
    }

    // Flush remaining internal codec states cleanly
    flush_encoder_context(codec_ctx_input, audio_stream_input, shared_pkt);
    flush_encoder_context(codec_ctx_bt, audio_stream_bt, shared_pkt);

    if (first_v_pts != -1) {
        flush_encoder_context(video_enc_ctx, video_stream, shared_pkt);
    }

    av_write_trailer(fmt_ctx);

    av_frame_free(&a_frame_input);
    av_frame_free(&a_frame_bt);
    av_frame_free(&v_frame_enc);
    av_packet_free(&shared_pkt);

    return NULL;
}

/**
 * @brief Wakes the encoder thread from sleep without waiting for a timeout.
 */
void wake_encoder(void) {
    pthread_cond_signal(&enc_cond);
}

/**
 * @brief Signals the encoder thread to stop, flushes remaining frames, and frees contexts.
 * @return void
 */
void stop_encoder() {
    keep_running = 0;
    if (thread_is_active) {
        pthread_join(encoder_thread, NULL);
        thread_is_active = 0;
    }

    // Drain abandoned ringbuffer memory
    if (video_queue_ptr) {
        VideoPayload stale_payload;
        while (pop_video_frame(video_queue_ptr, &stale_payload)) {}
    }

    if (fmt_ctx) {
        avio_closep(&fmt_ctx->pb);
        avformat_free_context(fmt_ctx);
        fmt_ctx = NULL;
    }

    if (codec_ctx_input) { avcodec_free_context(&codec_ctx_input); codec_ctx_input = NULL; }
    if (codec_ctx_bt) { avcodec_free_context(&codec_ctx_bt); codec_ctx_bt = NULL; }
    if (video_enc_ctx) { avcodec_free_context(&video_enc_ctx); video_enc_ctx = NULL; }
    if (hw_device_ctx) { av_buffer_unref(&hw_device_ctx); hw_device_ctx = NULL; }
}
