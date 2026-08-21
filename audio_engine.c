// Standard C Libraries
#include <stdio.h>       // For printf, snprintf
#include <stdlib.h>      // For calloc, free
#include <stdatomic.h>   // For lock-free atomic variables
#include <math.h>        // For audio math/clipping

// Audio Processing Libraries
#include <sndfile.h>     // For reading FLAC/WAV files
#include <samplerate.h>  // For SRC_SINC_BEST_QUALITY rate conversion
#include <rubberband/rubberband-c.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <stdalign.h>
#include <glib.h>
#include "encoder.h"

// Project Local Headers
#include "audio_engine.h" // Provides <jack/jack.h> and function signatures
#include "encoder.h"
#include "video_ringbuffer.h" // Required for SPSC_Video_Queue
#include "ringbuffer.h"   // Provides AudioFrame and lock-free queue

// UI -> RT Communication (Aligned to 64 bytes to prevent false sharing with RT writes)
/** Destructive pre-buffer multiplier applied directly to incoming hardware samples before writing to memory. */
alignas(64) static _Atomic float input_gain = 1.0f;
/** Master output multiplier applied dynamically during real-time playback. */
static _Atomic float bt_gain = 1.0f;
/** Non-destructive post-buffer multipliers applied dynamically to individual tracks during playback. */
_Atomic float multitrack_track_gains[MAX_TRACKS] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
_Atomic float playback_speed = 1.0f;

_Atomic bool track_has_audio[MAX_TRACKS] = {false};
_Atomic bool track_has_undo[MAX_TRACKS] = {false};

_Atomic bool engine_is_recording = false;
_Atomic bool engine_is_armed = false;
_Atomic bool engine_is_paused = false;
_Atomic bool engine_is_playing = false;
_Atomic bool loop_active = false;
_Atomic size_t loop_start_frame = 0;
_Atomic size_t loop_end_frame = 0;

// RT -> UI Communication (Isolated cache lines to prevent RT thread cache evictions)
alignas(64) _Atomic float vu_peak_input_l = 0.0f;
_Atomic float vu_peak_input_r = 0.0f;
_Atomic float vu_peak_bt_l = 0.0f;
_Atomic float vu_peak_bt_r = 0.0f;

jack_client_t *client;
jack_port_t *input_port_1, *input_port_2;
jack_port_t *output_port_1, *output_port_2;
SPSC_Audio_Queue audio_queue;
SPSC_Video_Queue video_queue;
SPSC_Command_Queue cmd_queue;

float *pristine_bt_buf = NULL;
size_t pristine_frames = 0;

// --- REPLACED GLOBALS (Around line 40) ---
// Real-Time Engine State
RubberBandState rt_rb_state = NULL;
atomic_size_t pristine_read_pos = 0; // Converted to atomic to share with background thread

// Background DSP Stretcher State
SPSC_Stereo_Queue stretch_queue;
static pthread_t stretcher_thread;
static _Atomic bool stretcher_keep_running = false;
static _Atomic bool stretcher_flush_request = false;
static _Atomic bool stretcher_ratio_update = false;

// Multi-Track Looper Engine State
float *multitrack_tracks[MAX_TRACKS] = {NULL};
float *undo_tracks[MAX_TRACKS] = {NULL};
_Atomic bool track_is_soloed[MAX_TRACKS] = {false};
_Atomic int active_track_count = 0;
_Atomic int current_recording_track = 0;
_Atomic bool is_multitrack_mode = false;

atomic_size_t backing_track_frames = 0; // Dynamic tracking for UI playhead completion
alignas(64) atomic_size_t playback_pos = 0;
atomic_int active_sample_rate = 44100; // Will be immediately overwritten by JACK on init

_Atomic bool seek_flag = false;
_Atomic double seek_target = 0.0;

_Atomic bool speed_change_flag = false;
_Atomic float speed_target = 1.0f;

// Atomic Handshake flags for lock-free memory swapping
_Atomic bool request_track_free = false;
_Atomic bool safe_to_free_track = false;
_Atomic bool stretcher_safe_to_free = false;

/**
 * @brief Centralized synchronization barrier. Requests all DSP threads (RT and Stretcher) to detach from active memory buffers and spins until safe.
 * @return true if both threads successfully detached, false if the operation timed out.
 */
bool await_rt_thread_detach(void) {
    atomic_store_explicit(&request_track_free, true, memory_order_release);
    int timeout = 500;
    while ((!atomic_load_explicit(&safe_to_free_track, memory_order_acquire) ||
        !atomic_load_explicit(&stretcher_safe_to_free, memory_order_acquire)) && timeout > 0) {
        usleep(1000);
    timeout--;
        }

        if (timeout == 0) {
            printf("CRITICAL: DSP Barrier Timeout. Aborting safe memory swap to prevent deadlock.\n");
            // Failsafe: Reset flags so the RT engine doesn't remain muted forever
            atomic_store_explicit(&request_track_free, false, memory_order_release);
            atomic_store_explicit(&safe_to_free_track, false, memory_order_relaxed);
            atomic_store_explicit(&stretcher_safe_to_free, false, memory_order_relaxed);
            return false;
        }

        return true;
}

/**
 * @brief Releases the synchronization barrier, resuming active real-time processing.
 */
void resume_rt_thread(void) {
    atomic_store_explicit(&safe_to_free_track, false, memory_order_relaxed);
    atomic_store_explicit(&stretcher_safe_to_free, false, memory_order_relaxed);
    atomic_store_explicit(&request_track_free, false, memory_order_release);
}

// Forward declaration to resolve implicit usage in load_audio_ffmpeg
static bool is_ram_allocation_safe(sf_count_t source_frames, int source_rate, int target_rate);
/**
 * @brief Drains all available frames from the provided decoder context and extracts them as normalized interleaved floats.
 * Dynamically resizes the destination buffer if the extracted samples exceed current capacity.
 * @param codec_ctx The FFmpeg codec context.
 * @param frame The pre-allocated FFmpeg frame.
 * @param raw_buf Pointer to the destination float array.
 * @param capacity Pointer to the current maximum frame capacity of the destination array.
 * @param total_samples Pointer to the total number of samples currently written.
 * @param channels The number of audio channels in the stream.
 * @return 0 on success, -1 on memory allocation failure.
 */
static int drain_and_extract_frames(AVCodecContext *codec_ctx, AVFrame *frame, float **raw_buf, size_t *capacity, size_t *total_samples, int channels) {
    while (avcodec_receive_frame(codec_ctx, frame) == 0) {
        int nb_samples = frame->nb_samples;
        if (*total_samples + nb_samples * channels > *capacity * channels) {
            size_t new_cap = *capacity * 2;
            float *tmp = realloc(*raw_buf, new_cap * channels * sizeof(float));
            if (!tmp) return -1;
            *capacity = new_cap;
            *raw_buf = tmp;
        }

        for (int s = 0; s < nb_samples; s++) {
            for (int c = 0; c < channels; c++) {
                float sample_val = 0.0f;
                if (frame->format == AV_SAMPLE_FMT_FLTP) sample_val = ((float*)frame->data[c])[s];
                else if (frame->format == AV_SAMPLE_FMT_FLT) sample_val = ((float*)frame->data[0])[s * channels + c];
                else if (frame->format == AV_SAMPLE_FMT_S16P) sample_val = ((int16_t*)frame->data[c])[s] / 32768.0f;
                else if (frame->format == AV_SAMPLE_FMT_S16) sample_val = ((int16_t*)frame->data[0])[s * channels + c] / 32768.0f;
                else if (frame->format == AV_SAMPLE_FMT_S32P) sample_val = ((int32_t*)frame->data[c])[s] / 2147483648.0f;
                else if (frame->format == AV_SAMPLE_FMT_S32) sample_val = ((int32_t*)frame->data[0])[s * channels + c] / 2147483648.0f;

                (*raw_buf)[(*total_samples)++] = sample_val;
            }
        }
    }
    return 0;
}

/**
 * @brief Decodes audio tracks from an MKV or media container into floating-point stereo buffers using FFmpeg.
 * @param filepath Absolute path to the media file.
 * @param out_raw Pointer to receive the allocated interleaved float raw data array for Stream 0.
 * @param out_frames Pointer to store total decoded audio frame count.
 * @param out_channels Pointer to store source channel count.
 * @param out_rate Pointer to store source audio sample rate.
 * @param out_raw2 Pointer to receive secondary audio stream data if available.
 * @param out_frames2 Pointer to store total decoded audio frame count for Stream 1.
 * @param target_jack_rate The active JACK hardware sample rate for RAM validation.
 * @return 0 on success, -1 on format error, -2 if track exceeds safe RAM limits.
 */
static int load_audio_ffmpeg(const char* filepath, float** out_raw, sf_count_t* out_frames, int* out_channels, int* out_rate, float** out_raw2, sf_count_t* out_frames2, int target_jack_rate) {
    AVFormatContext *fmt_ctx = NULL;
    if (avformat_open_input(&fmt_ctx, filepath, NULL, NULL) != 0) return -1;
    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    int stream_idx1 = -1, stream_idx2 = -1;
    for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            if (stream_idx1 < 0) stream_idx1 = (int)i;
            else if (stream_idx2 < 0) { stream_idx2 = (int)i; break; }
        }
    }

    if (stream_idx1 < 0) {
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    AVStream *st1 = fmt_ctx->streams[stream_idx1];
    const AVCodec *codec1 = avcodec_find_decoder(st1->codecpar->codec_id);
    AVCodecContext *codec_ctx1 = avcodec_alloc_context3(codec1);
    avcodec_parameters_to_context(codec_ctx1, st1->codecpar);

    if (codec_ctx1->ch_layout.nb_channels == 0 || codec_ctx1->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) {
        av_channel_layout_default(&codec_ctx1->ch_layout, 2);
    }
    if (codec_ctx1->sample_rate == 0) {
        codec_ctx1->sample_rate = 48000;
    }

    if (avcodec_open2(codec_ctx1, codec1, NULL) < 0) {
        avcodec_free_context(&codec_ctx1);
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    int channels = codec_ctx1->ch_layout.nb_channels > 0 ? codec_ctx1->ch_layout.nb_channels : 2;
    int sample_rate = codec_ctx1->sample_rate;

    // Fast Probe: Validate RAM requirements using container metadata before decoding begins
    double duration_sec = 0.0;
    if (st1->duration != AV_NOPTS_VALUE) {
        duration_sec = st1->duration * av_q2d(st1->time_base);
    } else if (fmt_ctx->duration != AV_NOPTS_VALUE) {
        duration_sec = (double)fmt_ctx->duration / AV_TIME_BASE;
    }

    if (duration_sec > 0.0) {
        sf_count_t estimated_frames = (sf_count_t)(duration_sec * sample_rate);
        if (!is_ram_allocation_safe(estimated_frames, sample_rate, target_jack_rate)) {
            avcodec_free_context(&codec_ctx1);
            avformat_close_input(&fmt_ctx);
            return -2;
        }
    }

    // Setup Codec 2 and define channel count globally
    AVCodecContext *codec_ctx2 = NULL;
    int ch2 = channels;
    if (stream_idx2 >= 0 && out_raw2 != NULL) {
        AVStream *st2 = fmt_ctx->streams[stream_idx2];
        const AVCodec *codec2 = avcodec_find_decoder(st2->codecpar->codec_id);
        codec_ctx2 = avcodec_alloc_context3(codec2);
        avcodec_parameters_to_context(codec_ctx2, st2->codecpar);

        if (codec_ctx2->ch_layout.nb_channels == 0 || codec_ctx2->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) {
            av_channel_layout_default(&codec_ctx2->ch_layout, channels);
        }
        if (codec_ctx2->sample_rate == 0) {
            codec_ctx2->sample_rate = sample_rate;
        }

        if (avcodec_open2(codec_ctx2, codec2, NULL) < 0) {
            avcodec_free_context(&codec_ctx2);
            codec_ctx2 = NULL;
        } else {
            ch2 = codec_ctx2->ch_layout.nb_channels > 0 ? codec_ctx2->ch_layout.nb_channels : channels;
        }
    }

    size_t capacity1 = 1024 * 1024;
    size_t capacity2 = 1024 * 1024;
    float *raw_buf1 = malloc(capacity1 * channels * sizeof(float));
    float *raw_buf2 = codec_ctx2 ? malloc(capacity2 * ch2 * sizeof(float)) : NULL;

    // Prevent memory faults if initial allocation fails
    if (!raw_buf1 || (codec_ctx2 && !raw_buf2)) goto decode_cleanup;

    size_t total_samples1 = 0;
    size_t total_samples2 = 0;

    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();

    while (av_read_frame(fmt_ctx, pkt) == 0) {
        if (pkt->stream_index == stream_idx1) {
            if (avcodec_send_packet(codec_ctx1, pkt) == 0) {
                if (drain_and_extract_frames(codec_ctx1, frame, &raw_buf1, &capacity1, &total_samples1, channels) < 0) {
                    goto loop_cleanup;
                }
            }
        } else if (codec_ctx2 && pkt->stream_index == stream_idx2) {
            if (avcodec_send_packet(codec_ctx2, pkt) == 0) {
                if (drain_and_extract_frames(codec_ctx2, frame, &raw_buf2, &capacity2, &total_samples2, ch2) < 0) {
                    goto loop_cleanup;
                }
            }
        }
        av_packet_unref(pkt);
    }

    /**
     * @brief FLUSH DECODERS: Ensures internally cached samples at EOF are successfully retrieved to prevent track truncation.
     */
    if (avcodec_send_packet(codec_ctx1, NULL) == 0) {
        if (drain_and_extract_frames(codec_ctx1, frame, &raw_buf1, &capacity1, &total_samples1, channels) < 0) goto loop_cleanup;
    }

    if (codec_ctx2 && avcodec_send_packet(codec_ctx2, NULL) == 0) {
        if (drain_and_extract_frames(codec_ctx2, frame, &raw_buf2, &capacity2, &total_samples2, ch2) < 0) goto loop_cleanup;
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&codec_ctx1);
    if (codec_ctx2) avcodec_free_context(&codec_ctx2);
    avformat_close_input(&fmt_ctx);

    *out_raw = raw_buf1;
    if (out_raw2) *out_raw2 = raw_buf2;
    else free(raw_buf2);

    *out_frames = (sf_count_t)(total_samples1 / channels);
    if (out_frames2) *out_frames2 = raw_buf2 ? (sf_count_t)(total_samples2 / ch2) : 0;
    *out_channels = channels;
    *out_rate = sample_rate;
    return 0;

    loop_cleanup:
    av_frame_free(&frame);
    av_packet_free(&pkt);
    decode_cleanup:
    if (raw_buf1) free(raw_buf1);
    if (raw_buf2) free(raw_buf2);
    avcodec_free_context(&codec_ctx1);
    if (codec_ctx2) avcodec_free_context(&codec_ctx2);
    avformat_close_input(&fmt_ctx);
    return -1;
}

/**
 * @brief State-free quadratic soft-clipper. Passes audio below -2dBFS (0.8) linearly,
 * and applies a smooth quadratic roll-off to transients up to +1.5dBFS (1.2), gracefully
 * preventing harsh digital clipping without requiring state buffers.
 * @param x The raw audio sample.
 * @return The saturated audio sample tightly bounded to [-1.0, 1.0].
 */
static inline float soft_clip(float x) {
    float abs_x = fabsf(x);
    if (abs_x <= 0.8f) return x;
    if (abs_x >= 1.2f) return (x > 0.0f) ? 1.0f : -1.0f;
    float p = abs_x - 0.8f;
    float out = abs_x - (1.25f * p * p);
    return (x > 0.0f) ? out : -out;
}

/**
 * @brief Helper to compute VU meter decay and atomically update peak variables lock-free.
 * @param peak_var Pointer to the atomic peak float.
 * @param current_max The absolute maximum amplitude calculated in the current DSP block.
 * @return void
 */
static inline void update_peak_decay(_Atomic float *peak_var, float current_max) {
    float prev = atomic_load_explicit(peak_var, memory_order_relaxed);
    if (current_max < prev) current_max = prev * 0.98f;
    if (current_max < 0.0001f) current_max = 0.0f;
    atomic_store_explicit(peak_var, current_max, memory_order_relaxed);
}

/**
 * @brief Background thread that handles heavy FFT time-stretching, decoupling it from the RT loop.
 * @param arg Unused thread argument.
 * @return NULL
 */
/**
 * @brief Background thread that handles heavy FFT time-stretching, decoupling it from the RT loop.
 * @param arg Unused thread argument.
 * @return NULL
 */
static void* stretcher_loop(void* arg) {
    (void)arg;
    float in_l[1024];
    float in_r[1024];
    float out_l[1024];
    float out_r[1024];

    while (atomic_load_explicit(&stretcher_keep_running, memory_order_acquire)) {
        // 1. Acknowledge the global memory barrier FIRST to prevent deadlocks
        if (atomic_load_explicit(&request_track_free, memory_order_acquire)) {
            atomic_store_explicit(&stretcher_safe_to_free, true, memory_order_release);
            usleep(5000);
            continue;
        }

        // 2. Safely check for valid memory
        if (!rt_rb_state || !pristine_bt_buf) {
            usleep(5000);
            continue;
        }

        if (atomic_exchange_explicit(&stretcher_flush_request, false, memory_order_acquire)) {
            rubberband_reset(rt_rb_state);
        }

        if (atomic_exchange_explicit(&stretcher_ratio_update, false, memory_order_acquire)) {
            float speed = atomic_load_explicit(&playback_speed, memory_order_relaxed);
            rubberband_set_time_ratio(rt_rb_state, 1.0 / speed);
        }

        bool active = atomic_load_explicit(&engine_is_playing, memory_order_acquire) ||
        atomic_load_explicit(&engine_is_recording, memory_order_acquire);
        bool paused = atomic_load_explicit(&engine_is_paused, memory_order_acquire);

        if (!active || paused) {
            usleep(5000);
            continue;
        }

        size_t write_idx = atomic_load_explicit(&stretch_queue.write_index, memory_order_relaxed);
        size_t read_idx = atomic_load_explicit(&stretch_queue.read_index, memory_order_acquire);

        // Maintain an ahead-of-time buffer of ~16384 frames (approx 340ms at 48kHz) to absorb CPU jitter safely
        if ((write_idx - read_idx) >= 16384) {
            usleep(2000);
            continue;
        }

        size_t current_read_pos = atomic_load_explicit(&pristine_read_pos, memory_order_acquire);

        if (rubberband_available(rt_rb_state) < 1024 && current_read_pos < pristine_frames) {
            size_t push_chunk = 1024;
            if (current_read_pos + push_chunk > pristine_frames) {
                push_chunk = pristine_frames - current_read_pos;
            }

            for (size_t k = 0; k < push_chunk; k++) {
                in_l[k] = pristine_bt_buf[(current_read_pos + k) * 2];
                in_r[k] = pristine_bt_buf[(current_read_pos + k) * 2 + 1];
            }

            const float *in_ptrs[2] = { in_l, in_r };
            int is_final = ((current_read_pos + push_chunk) >= pristine_frames) ? 1 : 0;
            rubberband_process(rt_rb_state, in_ptrs, push_chunk, is_final);

            atomic_store_explicit(&pristine_read_pos, current_read_pos + push_chunk, memory_order_release);
        }

        int available = rubberband_available(rt_rb_state);
        int frames_to_read = (available >= 1024) ? 1024 : available;

        if (frames_to_read > 0) {
            float *out_ptrs[2] = { out_l, out_r };
            rubberband_retrieve(rt_rb_state, out_ptrs, frames_to_read);
            for (int i = 0; i < frames_to_read; i++) {
                StereoFrame *sf = acquire_stereo_frame(&stretch_queue);
                if (sf) {
                    sf->l = out_l[i];
                    sf->r = out_r[i];
                    commit_stereo_frame(&stretch_queue);
                }
            }
        }
    }
    return NULL;
}

// Runs at SCHED_FIFO priority. Absolutely no blocking calls permitted here.
int process_audio(jack_nframes_t nframes, void *arg) {
    (void)arg;

    jack_nframes_t cycle_start_time = jack_last_frame_time(client);

    jack_default_audio_sample_t *out_1 = (jack_default_audio_sample_t *)jack_port_get_buffer(output_port_1, nframes);
    jack_default_audio_sample_t *out_2 = (jack_default_audio_sample_t *)jack_port_get_buffer(output_port_2, nframes);
    jack_default_audio_sample_t *in_1 = (jack_default_audio_sample_t *)jack_port_get_buffer(input_port_1, nframes);
    jack_default_audio_sample_t *in_2 = (jack_default_audio_sample_t *)jack_port_get_buffer(input_port_2, nframes);

    EngineCommand cmd;
    while (pop_command(&cmd_queue, &cmd)) {
        switch (cmd.type) {
            case CMD_NEXT_TRACK: {
                int current = atomic_load_explicit(&current_recording_track, memory_order_acquire);
                if (current < MAX_TRACKS - 1) {
                    int next_track = current + 1;
                    atomic_store_explicit(&current_recording_track, next_track, memory_order_release);
                    int active = atomic_load_explicit(&active_track_count, memory_order_acquire);
                    if (next_track >= active) {
                        atomic_store_explicit(&active_track_count, next_track + 1, memory_order_release);
                    }
                }
                break;
            }
            default:
                break;
        }
    }

    if (atomic_load_explicit(&request_track_free, memory_order_acquire)) {
        atomic_store_explicit(&safe_to_free_track, true, memory_order_release);
        for (jack_nframes_t i = 0; i < nframes; i++) {
            out_1[i] = 0.0f;
            out_2[i] = 0.0f;
        }
        return 0;
    }

    size_t current_pos = atomic_load_explicit(&playback_pos, memory_order_acquire);
    bool recording = atomic_load_explicit(&engine_is_recording, memory_order_acquire);
    bool playing = atomic_load_explicit(&engine_is_playing, memory_order_acquire);
    bool paused = atomic_load_explicit(&engine_is_paused, memory_order_acquire);
    bool active = recording || playing;
    float current_input_gain = atomic_load_explicit(&input_gain, memory_order_relaxed);
    float current_bt_gain = atomic_load_explicit(&bt_gain, memory_order_relaxed);

    float max_l = 0.0f, max_r = 0.0f;
    float max_bt_l = 0.0f, max_bt_r = 0.0f;

    bool do_seek = atomic_exchange_explicit(&seek_flag, false, memory_order_acquire);
    bool do_speed = atomic_exchange_explicit(&speed_change_flag, false, memory_order_acquire);

    if (do_speed) {
        float speed = atomic_load_explicit(&speed_target, memory_order_relaxed);
        float old_speed = atomic_load_explicit(&playback_speed, memory_order_relaxed);

        if (old_speed != speed) {
            atomic_store_explicit(&playback_speed, speed, memory_order_relaxed);
            atomic_store_explicit(&stretcher_ratio_update, true, memory_order_release);

            double fraction = (backing_track_frames > 0) ? ((double)current_pos / (double)backing_track_frames) : 0.0;
            double loop_s_frac = (backing_track_frames > 0) ? ((double)atomic_load_explicit(&loop_start_frame, memory_order_relaxed) / (double)backing_track_frames) : 0.0;
            double loop_e_frac = (backing_track_frames > 0) ? ((double)atomic_load_explicit(&loop_end_frame, memory_order_relaxed) / (double)backing_track_frames) : 0.0;

            backing_track_frames = (size_t)(pristine_frames / speed);
            current_pos = (size_t)(fraction * backing_track_frames);

            if (atomic_load_explicit(&loop_active, memory_order_acquire)) {
                atomic_store_explicit(&loop_start_frame, (size_t)(loop_s_frac * backing_track_frames), memory_order_relaxed);
                atomic_store_explicit(&loop_end_frame, (size_t)(loop_e_frac * backing_track_frames), memory_order_relaxed);
            }

            if (!(active && !paused) && !do_seek) {
                atomic_store_explicit(&playback_pos, current_pos, memory_order_release);
            }

            atomic_store_explicit(&stretch_queue.read_index, atomic_load_explicit(&stretch_queue.write_index, memory_order_relaxed), memory_order_release);
            atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);
        }
    }

    if (do_seek) {
        double frac = atomic_load_explicit(&seek_target, memory_order_relaxed);
        size_t new_pristine = (size_t)(frac * pristine_frames);
        current_pos = (size_t)(frac * backing_track_frames);

        atomic_store_explicit(&pristine_read_pos, new_pristine, memory_order_release);
        atomic_store_explicit(&playback_pos, current_pos, memory_order_release);

        atomic_store_explicit(&stretch_queue.read_index, atomic_load_explicit(&stretch_queue.write_index, memory_order_relaxed), memory_order_release);
        atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);
    }

    for (jack_nframes_t i = 0; i < nframes; i++) {
        float raw_bt_l = 0.0f;
        float raw_bt_r = 0.0f;

        if (active && !paused) {
            float current_speed = atomic_load_explicit(&playback_speed, memory_order_relaxed);
            float l0_gain = atomic_load_explicit(&is_multitrack_mode, memory_order_relaxed) ? atomic_load_explicit(&multitrack_track_gains[0], memory_order_relaxed) : 1.0f;

            if (current_speed == 1.0f && atomic_load_explicit(&is_multitrack_mode, memory_order_relaxed) && multitrack_tracks[0]) {
                if ((current_pos + i) < pristine_frames) {
                    raw_bt_l = multitrack_tracks[0][(current_pos + i) * 2] * l0_gain;
                    raw_bt_r = multitrack_tracks[0][(current_pos + i) * 2 + 1] * l0_gain;
                }
            } else {
                StereoFrame sf;
                if (pop_stereo_frame(&stretch_queue, &sf)) {
                    raw_bt_l = sf.l * l0_gain;
                    raw_bt_r = sf.r * l0_gain;
                }
            }
        }

        int layers = atomic_load_explicit(&active_track_count, memory_order_relaxed);

        bool any_solo_active = false;
        for (int l = 0; l < MAX_TRACKS; l++) {
            if (atomic_load_explicit(&track_is_soloed[l], memory_order_relaxed)) {
                any_solo_active = true;
                break;
            }
        }

        if (active && !paused && (current_pos + i) < backing_track_frames) {
            bool base_audible = true;
            if (any_solo_active && !atomic_load_explicit(&track_is_soloed[0], memory_order_relaxed)) base_audible = false;

            if (!base_audible) {
                raw_bt_l = 0.0f;
                raw_bt_r = 0.0f;
            }

            for (int l = 1; l < layers && l < MAX_TRACKS; l++) {
                if (!multitrack_tracks[l]) continue;

                bool overdub_audible = true;
                if (any_solo_active && !atomic_load_explicit(&track_is_soloed[l], memory_order_relaxed)) overdub_audible = false;

                if (overdub_audible && (current_pos + i) < pristine_frames) {
                    float l_gain = atomic_load_explicit(&multitrack_track_gains[l], memory_order_relaxed);
                    raw_bt_l += multitrack_tracks[l][(current_pos + i) * 2] * l_gain;
                    raw_bt_r += multitrack_tracks[l][(current_pos + i) * 2 + 1] * l_gain;
                }
            }
        }

        float scaled_bt_l = raw_bt_l * current_bt_gain;
        float scaled_bt_r = raw_bt_r * current_bt_gain;

        if (fabsf(scaled_bt_l) > max_bt_l) max_bt_l = fabsf(scaled_bt_l);
        if (fabsf(scaled_bt_r) > max_bt_r) max_bt_r = fabsf(scaled_bt_r);

        out_1[i] = soft_clip(scaled_bt_l);
        out_2[i] = soft_clip(scaled_bt_r);

        float raw_input_l = in_1[i];
        float raw_input_r = in_2[i];
        float scaled_input_l = raw_input_l * current_input_gain;
        float scaled_input_r = raw_input_r * current_input_gain;

        if (fabsf(scaled_input_l) > max_l) max_l = fabsf(scaled_input_l);
        if (fabsf(scaled_input_r) > max_r) max_r = fabsf(scaled_input_r);

        if (recording && !paused && (current_pos + i) < pristine_frames) {
            int rec_track = atomic_load_explicit(&current_recording_track, memory_order_relaxed);
            // ALWAYS route audio to the active multitrack layer to allow Video Mode overdubs
            if (rec_track >= 0 && rec_track < MAX_TRACKS && multitrack_tracks[rec_track]) {
                multitrack_tracks[rec_track][(current_pos + i) * 2] = scaled_input_l;
                multitrack_tracks[rec_track][(current_pos + i) * 2 + 1] = scaled_input_r;
            }

            AudioFrame *frame = acquire_audio_frame(&audio_queue);
            if (frame) {
                frame->mix_l = soft_clip(scaled_bt_l + scaled_input_l);
                frame->mix_r = soft_clip(scaled_bt_r + scaled_input_r);
                frame->pts = (int64_t)(cycle_start_time + i); // Lock payload perfectly to the hardware clock
                commit_audio_frame(&audio_queue);
                wake_encoder();
            }
        }
    }

    update_peak_decay(&vu_peak_input_l, max_l);
    update_peak_decay(&vu_peak_input_r, max_r);
    update_peak_decay(&vu_peak_bt_l, max_bt_l);
    update_peak_decay(&vu_peak_bt_r, max_bt_r);

    if (active && !paused) {
        size_t next_pos = current_pos + nframes;
        if (atomic_load_explicit(&loop_active, memory_order_acquire)) {
            size_t l_end = atomic_load_explicit(&loop_end_frame, memory_order_relaxed);
            if (next_pos >= l_end) {
                size_t l_start = atomic_load_explicit(&loop_start_frame, memory_order_relaxed);
                next_pos = l_start + (next_pos - l_end);

                size_t target_pristine = (size_t)(((double)next_pos / (double)backing_track_frames) * pristine_frames);
                atomic_store_explicit(&pristine_read_pos, target_pristine, memory_order_release);

                atomic_store_explicit(&stretch_queue.read_index, atomic_load_explicit(&stretch_queue.write_index, memory_order_relaxed), memory_order_release);
                atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);
            }
        }
        // FILE: audio_engine.c
        atomic_store_explicit(&playback_pos, next_pos, memory_order_release);
    }
    return 0;
}

/**
 * @brief Polls the JACK server for a specific hardware device until it becomes visible or times out.
 * @param client Pointer to the active JACK client.
 * @param target_device The substring identifier of the hardware.
 * @param flags The JACK port flags to filter by.
 * @return A null-terminated array of port strings, or NULL on timeout. Must be freed with jack_free().
 */
static const char** wait_for_jack_ports(jack_client_t *client, const char *target_device, unsigned long flags) {
    const char **ports = NULL;
    int retries = 5;

    while (retries > 0) {
        ports = jack_get_ports(client, NULL, NULL, flags);
        if (ports) {
            for (int i = 0; ports[i] != NULL; i++) {
                if (strstr(ports[i], target_device) != NULL) return ports; // Hardware is visible
            }
            jack_free(ports);
            ports = NULL;
        }
        usleep(100000); // 100ms polling delay
        retries--;
    }
    return NULL;
}

/**
 * @brief Connects physical USB hardware inputs to the internal QJams capture ports.
 * @param target_audio_device The name identifier of the hardware to connect.
 * @return 0 on success, 2 if the ports could not be located or routed.
 */
int patch_audio_ports(const char* target_audio_device) {
    if (!client) return 2;

    printf("Attempting to patch capture device: %s\n", target_audio_device);

    jack_port_disconnect(client, input_port_1);
    jack_port_disconnect(client, input_port_2);

    const char **ports = wait_for_jack_ports(client, target_audio_device, JackPortIsOutput);
    int connected = 0;

    if (ports) {
        // Pass 1: Explicitly target USB 1 & 2 (AUX0 and AUX1 in PipeWire Pro Audio mode)
        for (int i = 0; ports[i] != NULL && connected < 2; i++) {
            if (strstr(ports[i], target_audio_device) != NULL) {
                jack_port_t *dst = NULL;
                if (strstr(ports[i], "AUX0") != NULL) dst = input_port_1;
                else if (strstr(ports[i], "AUX1") != NULL) dst = input_port_2;

                if (dst && jack_connect(client, ports[i], jack_port_name(dst)) == 0) {
                    printf("Connected Capture (USB 1/2): %s -> %s\n", ports[i], jack_port_name(dst));
                    connected++;
                }
            }
        }

        // Pass 2: The original strict filter (Targets standard non-AUX channels like FL/FR if present)
        if (connected < 2) {
            for (int i = 0; ports[i] != NULL && connected < 2; i++) {
                if (strstr(ports[i], target_audio_device) != NULL && strstr(ports[i], "AUX") == NULL) {
                    jack_port_t *dst = (connected == 0) ? input_port_1 : input_port_2;
                    if (jack_connect(client, ports[i], jack_port_name(dst)) == 0) {
                        printf("Connected Capture (Standard): %s -> %s\n", ports[i], jack_port_name(dst));
                        connected++;
                    }
                }
            }
        }

        // Pass 3: Ultimate Fallback (Accept any matching port)
        if (connected < 2) {
            for (int i = 0; ports[i] != NULL && connected < 2; i++) {
                if (strstr(ports[i], target_audio_device) != NULL) {
                    jack_port_t *dst = (connected == 0) ? input_port_1 : input_port_2;
                    // jack_connect fails safely if a connection already exists, preventing duplicates
                    if (jack_connect(client, ports[i], jack_port_name(dst)) == 0) {
                        printf("Connected Capture (Fallback): %s -> %s\n", ports[i], jack_port_name(dst));
                        connected++;
                    }
                }
            }
        }
        jack_free(ports);
    }

    if (connected < 2) {
        printf("Warning: Failure mapping complete stereo ports for audio device '%s'.\n", target_audio_device);
        return 2;
    }

    return 0;
}

/**
 * @brief Connects QJams output ports to the physical hardware playback ports.
 * @param target_playback_device The name identifier of the hardware to connect.
 * @return 0 on success, 2 if the ports could not be located or routed.
 */
int patch_playback_ports(const char* target_playback_device) {
    if (!client) return 2;

    printf("Attempting to patch playback device: %s\n", target_playback_device);

    // Disconnect old routes just in case the user changed hardware live
    jack_port_disconnect(client, output_port_1);
    jack_port_disconnect(client, output_port_2);

    const char **ports = wait_for_jack_ports(client, target_playback_device, JackPortIsPhysical | JackPortIsInput);
    int connected = 0;

    if (ports) {
        for (int i = 0; ports[i] != NULL && connected < 2; i++) {
            if (strstr(ports[i], target_playback_device) != NULL) {
                jack_port_t *src = (connected == 0) ? output_port_1 : output_port_2;
                if (jack_connect(client, jack_port_name(src), ports[i]) == 0) {
                    printf("Connected Playback: %s -> %s\n", jack_port_name(src), ports[i]);
                    connected++;
                }
            }
        }
        jack_free(ports);
    }

    if (connected == 0) {
        printf("Warning: Failure mapping ports for playback device '%s'.\n", target_playback_device);
        return 2;
    }

    return 0;
}

/**
 * @brief Callback triggered when the JACK server dynamically changes its sample rate.
 * @param nframes The new sample rate.
 * @param arg Opaque pointer to user data (unused).
 * @return 0 on success.
 */
int jack_sample_rate_cb(jack_nframes_t nframes, void *arg) {
    (void)arg;
    atomic_store_explicit(&active_sample_rate, (int)nframes, memory_order_release);
    printf("JACK sample rate dynamically updated to %d Hz\n", nframes);
    return 0;
}

/**
 * @brief Callback triggered when the JACK server forcefully shuts down or disconnects the client.
 * @param arg Opaque pointer to user data (unused).
 * @return void
 */
extern gboolean on_jack_shutdown_ui(gpointer data);

void jack_shutdown(void *arg) {
    (void)arg;
    printf("JACK shutdown triggered. Attempting graceful UI halt...\n");
    g_idle_add(on_jack_shutdown_ui, NULL);
}

extern gboolean on_jack_port_registration_ui(gpointer data);

/**
 * @brief Callback triggered when JACK detects a hardware port connection or disconnection.
 * @param port_id The ID of the port.
 * @param register_port Non-zero if registering, zero if unregistering.
 * @param arg Opaque user data.
 */
void jack_port_registration_cb(jack_port_id_t port_id, int register_port, void *arg) {
    (void)port_id; (void)register_port; (void)arg;
    // Run safely in the GTK main loop, bouncing out of JACK's notification thread
    g_idle_add(on_jack_port_registration_ui, NULL);
}

/**
 * @brief Dynamically calculates if the system has enough available physical memory
 * to safely load and pre-allocate recording buffers for the requested track.
 * @param source_frames Total frames in the source file.
 * @param source_rate Sample rate of the source file.
 * @param target_rate The active JACK hardware sample rate.
 * @return true if safe to load, false if it exceeds safe RAM limits.
 */
static bool is_ram_allocation_safe(sf_count_t source_frames, int source_rate, int target_rate) {
    if (source_rate <= 0 || target_rate <= 0) return false;

    // Calculate the exact frame count after resampling
    double ratio = (double)target_rate / (double)source_rate;
    size_t pristine_frames_est = (size_t)(source_frames * ratio);

    // Calculate total required memory footprint: Pristine + 6 Full-Length Looper Layers + 6 Undo Buffers (Stereo Floats)
    size_t pristine_bytes = pristine_frames_est * 2 * sizeof(float);
    size_t layer_bytes = pristine_frames_est * 2 * sizeof(float);
    size_t total_required_bytes = pristine_bytes + (layer_bytes * MAX_TRACKS * 2);

    // Query Linux for currently available physical RAM using /proc/meminfo
    size_t available_ram = 0;
    FILE *meminfo = fopen("/proc/meminfo", "r");
    if (meminfo) {
        char line[256];
        while (fgets(line, sizeof(line), meminfo)) {
            if (strncmp(line, "MemAvailable:", 13) == 0) {
                long long avail_kb;
                if (sscanf(line, "MemAvailable: %lld kB", &avail_kb) == 1) {
                    available_ram = (size_t)avail_kb * 1024;
                }
                break;
            }
        }
        fclose(meminfo);
    }

    // Fallback to sysconf if MemAvailable is missing
    if (available_ram == 0) {
        long pages = sysconf(_SC_AVPHYS_PAGES);
        long page_size = sysconf(_SC_PAGE_SIZE);
        if (pages < 0 || page_size < 0) return true;
        available_ram = (size_t)pages * (size_t)page_size;
    }
    // Add back the memory currently held by the 12 active and 12 undo arrays
    // as it will be freed and recycled by the new track allocation
    size_t currently_held_bytes = 0;
    for (int i = 0; i < MAX_TRACKS; i++) {
        if (multitrack_tracks[i]) currently_held_bytes += pristine_frames * 2 * sizeof(float);
        if (undo_tracks[i]) currently_held_bytes += pristine_frames * 2 * sizeof(float);
    }
    available_ram += currently_held_bytes;

    // 1. Calculate an absolute maximum RAM cap (e.g., 40 GB) to prevent FFmpeg decode hangs
    size_t absolute_cap_bytes = 40ULL * 1024 * 1024 * 1024; // 40 GB

    // 2. Allow up to 85% of currently available physical RAM (raised from 50%)
    size_t dynamic_cap_bytes = (available_ram * 85) / 100;

    size_t final_cap = (dynamic_cap_bytes < absolute_cap_bytes) ? dynamic_cap_bytes : absolute_cap_bytes;

    // Reject if the track demands more than the allowed threshold
    if (total_required_bytes > final_cap) {
        printf("Error: Track requires %zu MB, but max allowed is %zu MB.\n",
               total_required_bytes / (1024 * 1024), final_cap / (1024 * 1024));
        return false;
    }

    return true;
}

/**
 * @brief Resamples a generic interleaved float audio stream using a configured SwrContext.
 * Processes data in chunks to bound memory usage and completely flushes the resampler at EOF.
 * @param swr_ctx The configured FFmpeg software resampler context.
 * @param in_data Pointer to the source raw audio data.
 * @param in_frames Total number of source frames.
 * @param in_channels Number of interleaved source channels.
 * @param out_data Pointer to the pre-allocated destination buffer (always stereo).
 * @param target_out_frames Total expected capacity of the destination buffer in frames.
 * @param ratio The resampling ratio (out_rate / in_rate).
 * @return void
 */
static void resample_stream(SwrContext *swr_ctx, const float *in_data, sf_count_t in_frames, int in_channels, float *out_data, size_t target_out_frames, double ratio) {
    if (!swr_ctx || !in_data || !out_data) return;

    int chunk_size = 1000000;
    int max_out_chunk = (int)((double)chunk_size * ratio) + 8192;

    sf_count_t in_pos = 0;
    size_t out_pos = 0;

    while (in_pos < in_frames) {
        int in_count = (int)((in_frames - in_pos > (sf_count_t)chunk_size) ? chunk_size : (in_frames - in_pos));

        size_t space_left = target_out_frames - out_pos;
        if (space_left == 0) break;

        int out_count = (space_left > (size_t)max_out_chunk) ? max_out_chunk : (int)space_left;

        const uint8_t *in_ptr[1] = { (const uint8_t *)(in_data + (in_pos * in_channels)) };
        uint8_t *out_ptr[1] = { (uint8_t *)(out_data + (out_pos * 2)) };

        int ret = swr_convert(swr_ctx, out_ptr, out_count, in_ptr, in_count);
        if (ret < 0) break;

        in_pos += in_count;
        out_pos += (size_t)ret;
    }

    // Flush remaining cached samples from the resampler
    while (out_pos < target_out_frames) {
        size_t space_left = target_out_frames - out_pos;
        int out_count = (space_left > (size_t)max_out_chunk) ? max_out_chunk : (int)space_left;
        uint8_t *out_ptr[1] = { (uint8_t *)(out_data + (out_pos * 2)) };

        int ret = swr_convert(swr_ctx, out_ptr, out_count, NULL, 0);
        if (ret <= 0) break;
        out_pos += (size_t)ret;
    }
}

/**
 * @brief Decodes an audio file and safely imports it into a specific multitrack layer.
 * Enforces Strict Bounding: truncates long files and zero-pads short files to match pristine_frames.
 * Uses the centralized atomic handshake to safely swap the memory pointer.
 * @param filepath The absolute path to the audio file.
 * @param track_idx The target layer index (0 to MAX_TRACKS - 1).
 * @return 0 on success, -1 on format error, -2 on memory failure, -3 on corrupt media.
 */
int import_to_layer(const char* filepath, int track_idx) {
    if (!client || track_idx < 0 || track_idx >= MAX_TRACKS || pristine_frames == 0) return -1;

    SF_INFO sfinfo = {0};
    float* raw_data = NULL;
    jack_nframes_t jack_rate = jack_get_sample_rate(client);

    SNDFILE *file = sf_open(filepath, SFM_READ, &sfinfo);
    if (file) {
        raw_data = calloc(sfinfo.frames * sfinfo.channels, sizeof(float));
        sf_readf_float(file, raw_data, sfinfo.frames);
        sf_close(file);
    } else {
        int rate = 0, channels = 0;
        sf_count_t frames = 0;
        if (load_audio_ffmpeg(filepath, &raw_data, &frames, &channels, &rate, NULL, NULL, jack_rate) != 0) {
            return -1;
        }
        sfinfo.frames = frames;
        sfinfo.channels = channels;
        sfinfo.samplerate = rate;
    }

    if (sfinfo.frames == 0 || sfinfo.samplerate == 0) {
        if (raw_data) free(raw_data);
        return -3;
    }

    double ratio = (double)jack_rate / (double)sfinfo.samplerate;
    size_t resampled_frames = (size_t)(sfinfo.frames * ratio);

    float* new_layer_buf = calloc(pristine_frames * 2, sizeof(float));
    float* src_out = calloc(resampled_frames * 2, sizeof(float));

    if (!new_layer_buf || !src_out) {
        if (new_layer_buf) free(new_layer_buf);
        if (src_out) free(src_out);
        if (raw_data) free(raw_data);
        return -2;
    }

    SwrContext *swr_ctx = swr_alloc();
    AVChannelLayout in_ch_layout, out_ch_layout;
    av_channel_layout_default(&in_ch_layout, sfinfo.channels);
    av_channel_layout_default(&out_ch_layout, 2);

    av_opt_set_chlayout(swr_ctx, "in_chlayout",    &in_ch_layout, 0);
    av_opt_set_int(swr_ctx,      "in_sample_rate", sfinfo.samplerate, 0);
    av_opt_set_sample_fmt(swr_ctx,"in_sample_fmt",  AV_SAMPLE_FMT_FLT, 0);

    av_opt_set_chlayout(swr_ctx, "out_chlayout",   &out_ch_layout, 0);
    av_opt_set_int(swr_ctx,      "out_sample_rate", jack_rate, 0);
    av_opt_set_sample_fmt(swr_ctx,"out_sample_fmt", AV_SAMPLE_FMT_FLT, 0);

    swr_init(swr_ctx);
    resample_stream(swr_ctx, raw_data, sfinfo.frames, sfinfo.channels, src_out, resampled_frames, ratio);
    swr_free(&swr_ctx);
    free(raw_data);

    size_t current_pos = atomic_load_explicit(&playback_pos, memory_order_acquire);
    int truncated = 0;
    size_t frames_to_copy = resampled_frames;

    if (current_pos + resampled_frames > pristine_frames) {
        frames_to_copy = (pristine_frames > current_pos) ? (pristine_frames - current_pos) : 0;
        truncated = 1;
    }

    for (size_t i = 0; i < frames_to_copy * 2; i++) {
        new_layer_buf[(current_pos * 2) + i] = src_out[i];
    }
    free(src_out);

    if (!await_rt_thread_detach()) {
        free(new_layer_buf);
        return -1;
    }

    float *old_undo = undo_tracks[track_idx];
    undo_tracks[track_idx] = multitrack_tracks[track_idx];
    multitrack_tracks[track_idx] = new_layer_buf;
    if (old_undo) free(old_undo);

    if (track_idx == 0 && pristine_bt_buf) {
        memcpy(pristine_bt_buf, new_layer_buf, pristine_frames * 2 * sizeof(float));
        atomic_store_explicit(&stretch_queue.read_index, atomic_load_explicit(&stretch_queue.write_index, memory_order_relaxed), memory_order_release);
        atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);
    }

    int active = atomic_load_explicit(&active_track_count, memory_order_acquire);
    if (track_idx >= active) {
        atomic_store_explicit(&active_track_count, track_idx + 1, memory_order_release);
    }

    atomic_store_explicit(&track_has_audio[track_idx], true, memory_order_release);

    resume_rt_thread();

    return truncated ? 1 : 0;
}

/**
 * @brief Loads an audio file, resamples it to match the JACK sample rate, and initializes the RubberBand stretcher.
 * Utilizes the centralized lock-free atomic handshake to detach the RT thread safely before swapping memory maps.
 * @param filepath The absolute path to the audio file.
 * @param client_ptr Pointer to the active JACK client.
 * @return 0 on success, -1 on format error, -2 if RAM check fails, -3 on corrupt decode.
 */
int load_backing_track(const char* filepath, jack_client_t* client_ptr) {
    if (!client_ptr) return -1;
    SF_INFO sfinfo = {0};
    float* raw_data = NULL;
    float* raw_data2 = NULL;
    sf_count_t frames2 = 0;

    jack_nframes_t jack_rate = jack_get_sample_rate(client_ptr);

    SNDFILE *file = sf_open(filepath, SFM_READ, &sfinfo);
    if (file) {
        if (!is_ram_allocation_safe(sfinfo.frames, sfinfo.samplerate, jack_rate)) {
            sf_close(file);
            return -2;
        }

        raw_data = calloc(sfinfo.frames * sfinfo.channels, sizeof(float));
        sf_readf_float(file, raw_data, sfinfo.frames);
        sf_close(file);
    } else {
        int rate = 0, channels = 0;
        sf_count_t frames = 0;
        int ret = load_audio_ffmpeg(filepath, &raw_data, &frames, &channels, &rate, &raw_data2, &frames2, jack_rate);
        if (ret != 0) {
            return ret;
        }
        sfinfo.frames = frames;
        sfinfo.channels = channels;
        sfinfo.samplerate = rate;
    }

    if (sfinfo.frames == 0 || sfinfo.samplerate == 0) {
        if (raw_data) free(raw_data);
        if (raw_data2) free(raw_data2);
        return -3;
    }

    atomic_store_explicit(&active_sample_rate, (int)jack_rate, memory_order_release);
    double ratio = (double)jack_rate / (double)sfinfo.samplerate;
    size_t new_pristine_frames = (size_t)(sfinfo.frames * ratio);

    float* new_pristine_buf = calloc(new_pristine_frames * 2, sizeof(float));
    float* src_out = calloc(new_pristine_frames * 2, sizeof(float));
    float* src_out2 = raw_data2 ? calloc(new_pristine_frames * 2, sizeof(float)) : NULL;

    if (!new_pristine_buf || !src_out || (raw_data2 && !src_out2)) {
        if (new_pristine_buf) free(new_pristine_buf);
        if (src_out) free(src_out);
        if (src_out2) free(src_out2);
        if (raw_data) free(raw_data);
        if (raw_data2) free(raw_data2);
        return -2;
    }

    SwrContext *swr_ctx = swr_alloc();
    AVChannelLayout in_ch_layout, out_ch_layout;
    av_channel_layout_default(&in_ch_layout, sfinfo.channels);
    av_channel_layout_default(&out_ch_layout, 2);

    av_opt_set_chlayout(swr_ctx, "in_chlayout",    &in_ch_layout, 0);
    av_opt_set_int(swr_ctx,      "in_sample_rate", sfinfo.samplerate, 0);
    av_opt_set_sample_fmt(swr_ctx,"in_sample_fmt",  AV_SAMPLE_FMT_FLT, 0);

    av_opt_set_chlayout(swr_ctx, "out_chlayout",   &out_ch_layout, 0);
    av_opt_set_int(swr_ctx,      "out_sample_rate", jack_rate, 0);
    av_opt_set_sample_fmt(swr_ctx,"out_sample_fmt", AV_SAMPLE_FMT_FLT, 0);

    swr_init(swr_ctx);
    resample_stream(swr_ctx, raw_data, sfinfo.frames, sfinfo.channels, src_out, new_pristine_frames, ratio);

    if (raw_data2 && src_out2) {
        swr_init(swr_ctx);
        resample_stream(swr_ctx, raw_data2, frames2, sfinfo.channels, src_out2, new_pristine_frames, ratio);
    }
    swr_free(&swr_ctx);

    for (size_t i = 0; i < new_pristine_frames * 2; i++) {
        new_pristine_buf[i] = src_out[i];
    }

    int rb_options = RubberBandOptionProcessRealTime | RubberBandOptionEngineFiner | RubberBandOptionPitchHighQuality | RubberBandOptionPhaseIndependent | RubberBandOptionWindowLong | RubberBandOptionPitchHighConsistency;
    RubberBandState new_rb_state = rubberband_new(jack_rate, 2, rb_options, 1.0, 1.0);

    float* new_multitrack_tracks[MAX_TRACKS] = {NULL};
    float* new_undo_tracks[MAX_TRACKS] = {NULL};
    for (int i = 0; i < MAX_TRACKS; i++) {
        new_multitrack_tracks[i] = calloc(new_pristine_frames * 2, sizeof(float));
        new_undo_tracks[i] = calloc(new_pristine_frames * 2, sizeof(float));
        if (!new_multitrack_tracks[i] || !new_undo_tracks[i]) {
            for (int j = 0; j <= i; j++) {
                if (new_multitrack_tracks[j]) free(new_multitrack_tracks[j]);
                if (new_undo_tracks[j]) free(new_undo_tracks[j]);
            }
            free(new_pristine_buf);
            if (src_out) free(src_out);
            if (src_out2) free(src_out2);
            if (raw_data) free(raw_data);
            if (raw_data2) free(raw_data2);
            rubberband_delete(new_rb_state);
            return -2;
        }
    }

    for (size_t i = 0; i < new_pristine_frames * 2; i++) {
        new_multitrack_tracks[0][i] = src_out[i];
    }

    free(src_out);
    free(raw_data);

    // Route dual-audio containers natively into Track 2
    if (src_out2) {
        for (size_t i = 0; i < new_pristine_frames * 2; i++) {
            new_multitrack_tracks[1][i] = src_out2[i];
        }
        free(src_out2);
    }
    if (raw_data2) free(raw_data2);

    if (!await_rt_thread_detach()) {
        for (int i = 0; i < MAX_TRACKS; i++) {
            if (new_multitrack_tracks[i]) free(new_multitrack_tracks[i]);
            if (new_undo_tracks[i]) free(new_undo_tracks[i]);
        }
        free(new_pristine_buf);
        rubberband_delete(new_rb_state);
        return -1;
    }

    if (pristine_bt_buf) free(pristine_bt_buf);
    for (int i = 0; i < MAX_TRACKS; i++) {
        if (multitrack_tracks[i]) free(multitrack_tracks[i]);
        if (undo_tracks[i]) free(undo_tracks[i]);
    }

    if (rt_rb_state) rubberband_delete(rt_rb_state);

    pristine_bt_buf = new_pristine_buf;
    for (int i = 0; i < MAX_TRACKS; i++) {
        multitrack_tracks[i] = new_multitrack_tracks[i];
        undo_tracks[i] = new_undo_tracks[i];
    }

    rt_rb_state = new_rb_state;
    pristine_read_pos = 0;
    atomic_store_explicit(&playback_pos, 0, memory_order_release);
    pristine_frames = new_pristine_frames;
    backing_track_frames = new_pristine_frames;

    atomic_store_explicit(&stretch_queue.read_index, atomic_load_explicit(&stretch_queue.write_index, memory_order_relaxed), memory_order_release);
    atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);

    atomic_store_explicit(&active_track_count, 2, memory_order_release);
    atomic_store_explicit(&current_recording_track, 1, memory_order_release);

    atomic_store_explicit(&track_has_audio[0], true, memory_order_release);
    if (src_out2) atomic_store_explicit(&track_has_audio[1], true, memory_order_release);

    resume_rt_thread();

    float current_speed = atomic_load_explicit(&playback_speed, memory_order_relaxed);
    set_playback_speed(current_speed);

    printf("Loaded and resampled backing track to %d Hz for JIT streaming\n", jack_rate);
    return 0;
}

/**
 * @brief Initializes a multi-track loop canvas of an exact frame count.
 * Pauses the RT thread via atomic handshake before wiping and allocating arrays.
 * @param exact_frames The exact length of the canvas in frames.
 * @return 0 on success, -2 on memory failure, -1 on timeout.
 */
int init_exact_loop_canvas(size_t exact_frames) {
    if (!client) return -1;
    jack_nframes_t jack_rate = jack_get_sample_rate(client);
    atomic_store_explicit(&active_sample_rate, (int)jack_rate, memory_order_release);

    if (!is_ram_allocation_safe(exact_frames, jack_rate, jack_rate)) return -2;

    float* new_multitrack_tracks[MAX_TRACKS] = {NULL};
    float* new_undo_tracks[MAX_TRACKS] = {NULL};

    for (int i = 0; i < MAX_TRACKS; i++) {
        new_multitrack_tracks[i] = calloc(exact_frames * 2, sizeof(float));
        new_undo_tracks[i] = calloc(exact_frames * 2, sizeof(float));
        if (!new_multitrack_tracks[i] || !new_undo_tracks[i]) {
            for (int j = 0; j <= i; j++) {
                if (new_multitrack_tracks[j]) free(new_multitrack_tracks[j]);
                if (new_undo_tracks[j]) free(new_undo_tracks[j]);
            }
            return -2;
        }
    }

    RubberBandState new_rb_state = rt_rb_state;
    if (!new_rb_state) {
        int rb_options = RubberBandOptionProcessRealTime | RubberBandOptionEngineFiner | RubberBandOptionPitchHighQuality | RubberBandOptionPhaseIndependent | RubberBandOptionWindowLong | RubberBandOptionPitchHighConsistency;
        new_rb_state = rubberband_new(jack_rate, 2, rb_options, 1.0, 1.0);
    }

    if (!await_rt_thread_detach()) {
        for (int i = 0; i < MAX_TRACKS; i++) {
            if (new_multitrack_tracks[i]) free(new_multitrack_tracks[i]);
            if (new_undo_tracks[i]) free(new_undo_tracks[i]);
        }
        return -1;
    }

    if (pristine_bt_buf) free(pristine_bt_buf);
    pristine_bt_buf = calloc(exact_frames * 2, sizeof(float));

    for (int i = 0; i < MAX_TRACKS; i++) {
        if (multitrack_tracks[i]) free(multitrack_tracks[i]);
        if (undo_tracks[i]) free(undo_tracks[i]);
        multitrack_tracks[i] = new_multitrack_tracks[i];
        undo_tracks[i] = new_undo_tracks[i];
    }

    rt_rb_state = new_rb_state;
    if (rt_rb_state) rubberband_reset(rt_rb_state);

    pristine_read_pos = 0;
    atomic_store_explicit(&playback_pos, 0, memory_order_release);
    pristine_frames = exact_frames;
    backing_track_frames = exact_frames;
    atomic_store_explicit(&active_track_count, 1, memory_order_release);
    atomic_store_explicit(&current_recording_track, 0, memory_order_release);

    for (int i = 0; i < MAX_TRACKS; i++) {
        atomic_store_explicit(&track_has_audio[i], false, memory_order_release);
        atomic_store_explicit(&track_has_undo[i], false, memory_order_release);
    }

    atomic_store_explicit(&stretch_queue.read_index, atomic_load_explicit(&stretch_queue.write_index, memory_order_relaxed), memory_order_release);
    atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);

    resume_rt_thread();

    return 0;
}

/**
 * @brief Initializes a blank multi-track loop canvas.
 * @param duration_seconds The length of the blank canvas in seconds.
 * @return 0 on success, -2 on memory failure.
 */
int init_empty_loop_canvas(int duration_seconds) {
    if (!client) return -1;
    jack_nframes_t jack_rate = jack_get_sample_rate(client);
    return init_exact_loop_canvas(jack_rate * duration_seconds);
}

/**
 * @brief Non-destructively resizes the active recording canvas.
 * @param duration_seconds The new length of the canvas in seconds.
 * @return 0 on success, -1 on timeout, -2 on memory failure.
 */
int resize_loop_canvas_seconds(int duration_seconds) {
    if (!client) return -1;
    jack_nframes_t jack_rate = atomic_load_explicit(&active_sample_rate, memory_order_acquire);
    if (jack_rate <= 0) jack_rate = 48000;

    size_t new_exact_frames = (size_t)jack_rate * duration_seconds;
    if (new_exact_frames == pristine_frames) return 0;

    if (!is_ram_allocation_safe(new_exact_frames, jack_rate, jack_rate)) return -2;

    float* new_multitrack_tracks[MAX_TRACKS] = {NULL};
    float* new_undo_tracks[MAX_TRACKS] = {NULL};

    for (int i = 0; i < MAX_TRACKS; i++) {
        new_multitrack_tracks[i] = calloc(new_exact_frames * 2, sizeof(float));
        new_undo_tracks[i] = calloc(new_exact_frames * 2, sizeof(float));
        if (!new_multitrack_tracks[i] || !new_undo_tracks[i]) {
            for (int j = 0; j <= i; j++) {
                if (new_multitrack_tracks[j]) free(new_multitrack_tracks[j]);
                if (new_undo_tracks[j]) free(new_undo_tracks[j]);
            }
            return -2;
        }
    }

    float* new_pristine_bt_buf = calloc(new_exact_frames * 2, sizeof(float));

    if (!new_pristine_bt_buf) {
        for (int i = 0; i < MAX_TRACKS; i++) {
            free(new_multitrack_tracks[i]);
            free(new_undo_tracks[i]);
        }
        return -2;
    }

    if (!await_rt_thread_detach()) {
        free(new_pristine_bt_buf);
        for (int i = 0; i < MAX_TRACKS; i++) {
            free(new_multitrack_tracks[i]);
            free(new_undo_tracks[i]);
        }
        return -1;
    }

    size_t frames_to_copy = (pristine_frames < new_exact_frames) ? pristine_frames : new_exact_frames;

    if (pristine_bt_buf) {
        memcpy(new_pristine_bt_buf, pristine_bt_buf, frames_to_copy * 2 * sizeof(float));
        free(pristine_bt_buf);
    }
    pristine_bt_buf = new_pristine_bt_buf;

    for (int i = 0; i < MAX_TRACKS; i++) {
        if (multitrack_tracks[i]) {
            memcpy(new_multitrack_tracks[i], multitrack_tracks[i], frames_to_copy * 2 * sizeof(float));
            free(multitrack_tracks[i]);
        }
        if (undo_tracks[i]) {
            memcpy(new_undo_tracks[i], undo_tracks[i], frames_to_copy * 2 * sizeof(float));
            free(undo_tracks[i]);
        }
        multitrack_tracks[i] = new_multitrack_tracks[i];
        undo_tracks[i] = new_undo_tracks[i];
    }

    pristine_frames = new_exact_frames;
    atomic_store_explicit(&backing_track_frames, new_exact_frames, memory_order_release);

    size_t current_pos = atomic_load_explicit(&playback_pos, memory_order_acquire);
    if (current_pos > new_exact_frames) {
        atomic_store_explicit(&playback_pos, new_exact_frames, memory_order_release);
    }

    atomic_store_explicit(&stretch_queue.read_index, atomic_load_explicit(&stretch_queue.write_index, memory_order_relaxed), memory_order_release);
    atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);

    resume_rt_thread();
    return 0;
}

/**
 * @brief Updates the linear gain multiplier for the Quad Cortex input.
 * @param multiplier The computed linear gain value.
 * @return void
 */
void set_input_gain(float multiplier) {
    atomic_store_explicit(&input_gain, multiplier, memory_order_relaxed);
}

/**
 * @brief Updates the linear gain multiplier for the backing track.
 * @param multiplier The computed linear gain value.
 * @return void
 */
void set_bt_gain(float multiplier) {
    atomic_store_explicit(&bt_gain, multiplier, memory_order_relaxed);
}

void set_multitrack_layer_gain(int track_idx, float multiplier) {
    if (track_idx >= 0 && track_idx < MAX_TRACKS) {
        atomic_store_explicit(&multitrack_track_gains[track_idx], multiplier, memory_order_relaxed);
    }
}

/**
 * @brief Moves the playback position to a specific fraction of the track length via atomic flags.
 * @param fraction The target position as a decimal (0.0 to 1.0).
 * @return void
 */
void seek_backing_track(double fraction) {
    atomic_store_explicit(&seek_target, fraction, memory_order_relaxed);
    atomic_store_explicit(&seek_flag, true, memory_order_release);
}

/**
 * @brief Adjusts the time-stretch ratio of the backing track via atomic flags.
 * @param speed The linear speed multiplier.
 * @return void
 */
void set_playback_speed(float speed) {
    atomic_store_explicit(&speed_target, speed, memory_order_relaxed);
    atomic_store_explicit(&speed_change_flag, true, memory_order_release);
}

/**
 * @brief Sets the A/B frame boundaries for continuous looped playback and calculates target frames.
 * @param start_frac The starting position as a decimal (0.0 to 1.0).
 * @param end_frac The ending position as a decimal (0.0 to 1.0).
 * @return void
 */
void set_loop_points(double start_frac, double end_frac) {
    if (start_frac > end_frac) {
        double tmp = start_frac;
        start_frac = end_frac;
        end_frac = tmp;
    }

    size_t total_frames = atomic_load_explicit(&backing_track_frames, memory_order_acquire);
    size_t start = (size_t)(start_frac * total_frames);
    size_t end = (size_t)(end_frac * total_frames);

    if (end - start >= 1) { // Minimum loop window safeguard lowered to 1 frame for single-sample surgical cuts
        atomic_store_explicit(&loop_start_frame, start, memory_order_relaxed);
        atomic_store_explicit(&loop_end_frame, end, memory_order_relaxed);
        atomic_store_explicit(&loop_active, true, memory_order_release);
    }
}

/**
 * @brief Disables the active loop boundaries via atomic state modification.
 * @return void
 */
void clear_loop_points(void) {
    atomic_store_explicit(&loop_active, false, memory_order_release);
    atomic_store_explicit(&loop_start_frame, 0, memory_order_relaxed);
    atomic_store_explicit(&loop_end_frame, 0, memory_order_relaxed);
}

void multitrack_next_track(void) {
    EngineCommand cmd = { .type = CMD_NEXT_TRACK, .target_track = 0 };
    push_command(&cmd_queue, cmd);
}

/**
 * @brief Undoes the last recorded layer and decrements the active layer count.
 * Safely detaches the RT thread via atomic handshake before wiping memory arrays.
 * @return void
 */
void multitrack_undo_track(void) {
    int current = atomic_load_explicit(&current_recording_track, memory_order_acquire);
    if (current == 0 && active_track_count <= 1) return;

    if (!await_rt_thread_detach()) return;

    if (current > 1) {
        if (multitrack_tracks[current] && pristine_frames > 0) {
            memset(multitrack_tracks[current], 0, pristine_frames * 2 * sizeof(float));
        }
        atomic_store_explicit(&current_recording_track, current - 1, memory_order_release);
        atomic_store_explicit(&active_track_count, current, memory_order_release);
    } else if (current == 1) {
        if (multitrack_tracks[current] && pristine_frames > 0) {
            memset(multitrack_tracks[current], 0, pristine_frames * 2 * sizeof(float));
        }
    }

    resume_rt_thread();
}

void reset_backing_track_position(void) {
    atomic_store_explicit(&playback_pos, backing_track_frames, memory_order_release);
    usleep(10000);

    atomic_store_explicit(&pristine_read_pos, 0, memory_order_release);

    // Lock-free queue flush: Instantly advance the consumer index to match the producer index
    atomic_store_explicit(&stretch_queue.read_index, atomic_load_explicit(&stretch_queue.write_index, memory_order_relaxed), memory_order_release);
    atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);

    atomic_store_explicit(&playback_pos, 0, memory_order_release);
}

/**
 * @brief Registers the JACK client, sets callbacks, and initializes lock-free SPSC queues.
 * @param queue_capacity The power-of-two size limit for the audio ringbuffer.
 * @param init_input_gain The initial gain multiplier for the Quad Cortex.
 * @param existing_client Pointer to a pre-opened JACK client.
 * @param target_audio_device The name identifier for the physical capture hardware.
 * @param target_playback_device The name identifier for the physical playback hardware.
 * @return 0 on success, 1 on JACK failure, 2 on port patching failure.
 */
int init_audio_engine(size_t queue_capacity, float init_input_gain, jack_client_t* existing_client, const char* target_audio_device, const char* target_playback_device) {
    client = existing_client;
    input_gain = init_input_gain;

    // NEW: Immediately query the active hardware sample rate and register the dynamic callback
    jack_nframes_t current_rate = jack_get_sample_rate(client);
    atomic_store_explicit(&active_sample_rate, (int)current_rate, memory_order_release);
    jack_set_sample_rate_callback(client, jack_sample_rate_cb, 0);

    init_spsc_queue(&audio_queue, queue_capacity);
    init_stereo_queue(&stretch_queue, 65536); // Power of 2 (64k frames = ~1.3 seconds buffer)
    init_command_queue(&cmd_queue, 32); // Allocate memory for GTK -> RT sync commands

    // Spin up the background stretcher thread
    atomic_store_explicit(&stretcher_keep_running, true, memory_order_release);
    pthread_create(&stretcher_thread, NULL, stretcher_loop, NULL);

    // Bind the RT loop to the JACK client BEFORE activation!
    jack_set_process_callback(client, process_audio, 0);
    jack_on_shutdown(client, jack_shutdown, 0);
    jack_set_port_registration_callback(client, jack_port_registration_cb, 0);

    // Register internal QJams input and output ports
    input_port_1 = jack_port_register(client, "capture_1", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    input_port_2 = jack_port_register(client, "capture_2", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    output_port_1 = jack_port_register(client, "playback_1", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    output_port_2 = jack_port_register(client, "playback_2", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);

    if (jack_activate(client)) {
        printf("Cannot activate JACK client\n");
        return 1;
    }

    printf("Audio Engine Live: CachyOS RT thread active.\n");

    patch_playback_ports(target_playback_device);
    return patch_audio_ports(target_audio_device);
}

/**
 * @brief Closes the JACK client and frees all allocated DSP buffers, queues, and states.
 * @return void
 */
void shutdown_audio_engine() {
    if (atomic_exchange_explicit(&stretcher_keep_running, false, memory_order_release)) {
        pthread_join(stretcher_thread, NULL);
    }

    if (client) {
        jack_client_close(client);
        client = NULL;
    }
    if (rt_rb_state) rubberband_delete(rt_rb_state);
    if (audio_queue.buffer) free(audio_queue.buffer);
    if (video_queue.buffer) free(video_queue.buffer);
    if (stretch_queue.buffer) free(stretch_queue.buffer);
    if (cmd_queue.buffer) free(cmd_queue.buffer);
    if (pristine_bt_buf) free(pristine_bt_buf);
    for (int i = 0; i < MAX_TRACKS; i++) {
        if (multitrack_tracks[i]) { free(multitrack_tracks[i]); multitrack_tracks[i] = NULL; }
        if (undo_tracks[i]) { free(undo_tracks[i]); undo_tracks[i] = NULL; }
    }

    atomic_store_explicit(&vu_peak_input_l, 0.0f, memory_order_relaxed);
    atomic_store_explicit(&vu_peak_input_r, 0.0f, memory_order_relaxed);

    printf("Audio engine cleanly shut down.\n");
}

void swap_multitrack_tracks(int idx_a, int idx_b) {
    if (idx_a >= 0 && idx_b >= 0 && idx_a < MAX_TRACKS && idx_b < MAX_TRACKS) {
        if (!await_rt_thread_detach()) return;

        // Swap live layers
        float *tmp_multi = multitrack_tracks[idx_a];
        multitrack_tracks[idx_a] = multitrack_tracks[idx_b];
        multitrack_tracks[idx_b] = tmp_multi;

        // Swap undo layers simultaneously inside the same RT barrier
        float *tmp_undo = undo_tracks[idx_a];
        undo_tracks[idx_a] = undo_tracks[idx_b];
        undo_tracks[idx_b] = tmp_undo;

        // Synchronize the time-stretcher buffer if Track 1 was moved
        if ((idx_a == 0 || idx_b == 0) && pristine_bt_buf && pristine_frames > 0) {
            memcpy(pristine_bt_buf, multitrack_tracks[0], pristine_frames * 2 * sizeof(float));
            atomic_store_explicit(&stretch_queue.read_index, atomic_load_explicit(&stretch_queue.write_index, memory_order_relaxed), memory_order_release);
            atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);
        }

        resume_rt_thread();
    }
}

/**
 * @brief Atomically swaps a multitrack layer with its dedicated A/B undo buffer.
 * Safely detaches the RT thread before swapping memory block pointers.
 * Aborts if the RT thread fails to detach within the timeout window.
 * @param track_idx The index of the layer to swap.
 * @return void
 */
void swap_undo_track(int track_idx) {
    if (track_idx < 0 || track_idx >= MAX_TRACKS || !multitrack_tracks[track_idx] || !undo_tracks[track_idx]) return;

    if (!await_rt_thread_detach()) return;

    float *tmp = multitrack_tracks[track_idx];
    multitrack_tracks[track_idx] = undo_tracks[track_idx];
    undo_tracks[track_idx] = tmp;

    resume_rt_thread();
}

/**
 * @brief Clears a layer to absolute silence (backing up to undo) or restores it if already empty.
 * Uses the centralized atomic handshake to safely detach the RT thread before memory manipulation.
 * @param track_idx The target layer index.
 * @return 0 if cleared, 1 if restored, -1 on error.
 */
int clear_or_restore_track(int track_idx) {
    if (track_idx < 0 || track_idx >= MAX_TRACKS || !multitrack_tracks[track_idx] || !undo_tracks[track_idx]) return -1;

    size_t total_samples = pristine_frames * 2;
    // Massive O(1) Performance Upgrade: Bypass the heavy array loop scan
    bool has_audio = atomic_load_explicit(&track_has_audio[track_idx], memory_order_acquire);
    int action_status = -1;

    if (!await_rt_thread_detach()) return -1;

    int active = atomic_load_explicit(&active_track_count, memory_order_acquire);
    if (track_idx >= active && track_idx < MAX_TRACKS) {
        atomic_store_explicit(&active_track_count, track_idx + 1, memory_order_release);
    }

    if (has_audio) {
        bool undo_is_silent = true;
        for (size_t i = 0; i < total_samples; i++) {
            if (undo_tracks[track_idx][i] != 0.0f) {
                undo_is_silent = false;
                break;
            }
        }

        if (undo_is_silent) {
            float *tmp = multitrack_tracks[track_idx];
            multitrack_tracks[track_idx] = undo_tracks[track_idx];
            undo_tracks[track_idx] = tmp;
        } else {
            memcpy(undo_tracks[track_idx], multitrack_tracks[track_idx], total_samples * sizeof(float));
            memset(multitrack_tracks[track_idx], 0, total_samples * sizeof(float));
        }

        atomic_store_explicit(&track_has_audio[track_idx], false, memory_order_release);
        action_status = 0;
    } else {
        float *tmp = multitrack_tracks[track_idx];
        multitrack_tracks[track_idx] = undo_tracks[track_idx];
        undo_tracks[track_idx] = tmp;

        atomic_store_explicit(&track_has_audio[track_idx], true, memory_order_release);
        action_status = 1;
    }

    if (track_idx == 0 && pristine_bt_buf) {
        memcpy(pristine_bt_buf, multitrack_tracks[0], total_samples * sizeof(float));
        atomic_store_explicit(&stretch_queue.read_index, atomic_load_explicit(&stretch_queue.write_index, memory_order_relaxed), memory_order_release);
        atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);
    }

    resume_rt_thread();

    return action_status;
}

// ==============================================================================
// INTERNAL MATH CORES (No RT Locks, No Bounds Checking)
// ==============================================================================

static inline void internal_backup_track(int track_idx) {
    memcpy(undo_tracks[track_idx], multitrack_tracks[track_idx], pristine_frames * 2 * sizeof(float));
    atomic_store_explicit(&track_has_undo[track_idx], true, memory_order_release);
}

static inline void internal_cut_math(int track_idx, size_t start, size_t end) {
    size_t cut_len = end - start;
    size_t remaining = pristine_frames - end;
    size_t fade_len = (cut_len < 256) ? cut_len : 256;

    if (start >= fade_len) {
        for (size_t i = 0; i < fade_len; i++) {
            float mult = (float)(fade_len - i) / (float)fade_len;
            multitrack_tracks[track_idx][(start - fade_len + i) * 2] *= mult;
            multitrack_tracks[track_idx][(start - fade_len + i) * 2 + 1] *= mult;
        }
    }

    if (remaining > 0) {
        size_t apply_fade = (remaining < fade_len) ? remaining : fade_len;
        for (size_t i = 0; i < apply_fade; i++) {
            float mult = (float)i / (float)apply_fade;
            multitrack_tracks[track_idx][(end + i) * 2] *= mult;
            multitrack_tracks[track_idx][(end + i) * 2 + 1] *= mult;
        }
        memmove(&multitrack_tracks[track_idx][start * 2], &multitrack_tracks[track_idx][end * 2], remaining * 2 * sizeof(float));
    }

    memset(&multitrack_tracks[track_idx][(pristine_frames - cut_len) * 2], 0, cut_len * 2 * sizeof(float));
}

static inline void internal_smart_fade_math(int track_idx, size_t start, size_t end) {
    size_t sel_len = end - start;
    bool touches_start = (start == 0 || (start > 0 && multitrack_tracks[track_idx][(start - 1) * 2] == 0.0f));
    bool touches_end = (end >= pristine_frames || (end < pristine_frames && multitrack_tracks[track_idx][end * 2] == 0.0f));

    if (touches_start && !touches_end) {
        for (size_t i = 0; i < sel_len; i++) {
            float mult = (float)i / (float)(sel_len - 1);
            multitrack_tracks[track_idx][(start + i) * 2] *= mult;
            multitrack_tracks[track_idx][(start + i) * 2 + 1] *= mult;
        }
    } else if (touches_end && !touches_start) {
        for (size_t i = 0; i < sel_len; i++) {
            float mult = 1.0f - ((float)i / (float)(sel_len - 1));
            multitrack_tracks[track_idx][(start + i) * 2] *= mult;
            multitrack_tracks[track_idx][(start + i) * 2 + 1] *= mult;
        }
    } else {
        size_t half_len = sel_len / 2;
        for (size_t i = 0; i < half_len; i++) {
            float mult = 1.0f - ((float)i / (float)half_len);
            multitrack_tracks[track_idx][(start + i) * 2] *= mult;
            multitrack_tracks[track_idx][(start + i) * 2 + 1] *= mult;
        }
        multitrack_tracks[track_idx][(start + half_len) * 2] = 0.0f;
        multitrack_tracks[track_idx][(start + half_len) * 2 + 1] = 0.0f;
        for (size_t i = 1; i < (sel_len - half_len); i++) {
            float mult = (float)i / (float)(sel_len - half_len - 1);
            multitrack_tracks[track_idx][(start + half_len + i) * 2] *= mult;
            multitrack_tracks[track_idx][(start + half_len + i) * 2 + 1] *= mult;
        }
    }
}

static inline void internal_blend_fade_math(int track_idx, size_t start, size_t end) {
    size_t sel_len = end - start;
    size_t half_len = sel_len / 2;

    for (size_t i = 0; i < half_len; i++) {
        float mult = 1.0f - ((float)i / (float)half_len);
        multitrack_tracks[track_idx][(start + i) * 2] *= mult;
        multitrack_tracks[track_idx][(start + i) * 2 + 1] *= mult;
    }
    for (size_t i = 0; i < half_len; i++) {
        float mult = ((float)i / (float)half_len);
        multitrack_tracks[track_idx][(start + i) * 2] += multitrack_tracks[track_idx][(start + half_len + i) * 2] * mult;
        multitrack_tracks[track_idx][(start + i) * 2 + 1] += multitrack_tracks[track_idx][(start + half_len + i) * 2 + 1] * mult;
    }

    size_t remaining = pristine_frames - end;
    if (remaining > 0) {
        memmove(&multitrack_tracks[track_idx][(start + half_len) * 2],
                &multitrack_tracks[track_idx][end * 2],
                remaining * 2 * sizeof(float));
    }
    memset(&multitrack_tracks[track_idx][(pristine_frames - half_len) * 2], 0, half_len * 2 * sizeof(float));
}

static inline void internal_undo_math(int track_idx) {
    memcpy(multitrack_tracks[track_idx], undo_tracks[track_idx], pristine_frames * 2 * sizeof(float));
}

// ==============================================================================
// SINGLE TRACK EDITS
// ==============================================================================

/**
 * @brief Extracts the selected loop region from a layer and shifts the remaining audio left (Ripple Delete).
 * Includes a 5ms V-fade at the splice boundary to prevent zero-crossing audio pops.
 * Uses the centralized atomic handshake to safely detach the RT thread before memory manipulation.
 * @param track_idx The index of the layer to cut.
 * @return void
 */
void cut_multitrack_track_selection(int track_idx) {
    if (track_idx < 0 || track_idx >= MAX_TRACKS || !multitrack_tracks[track_idx]) return;
    if (!atomic_load_explicit(&loop_active, memory_order_acquire)) return;

    size_t start = atomic_load_explicit(&loop_start_frame, memory_order_acquire);
    size_t end = atomic_load_explicit(&loop_end_frame, memory_order_acquire);
    if (start >= end || end > pristine_frames) return;

    if (!await_rt_thread_detach()) return;

    internal_backup_track(track_idx);
    internal_cut_math(track_idx, start, end);

    if (track_idx == 0 && pristine_bt_buf) {
        memcpy(pristine_bt_buf, multitrack_tracks[0], pristine_frames * 2 * sizeof(float));
        atomic_store_explicit(&stretch_queue.read_index, atomic_load_explicit(&stretch_queue.write_index, memory_order_relaxed), memory_order_release);
        atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);
    }

    atomic_store_explicit(&playback_pos, start, memory_order_release);
    resume_rt_thread();

    clear_loop_points();
}

/**
 * @brief Instantly zeroes out the memory for a specific looper overdub layer.* @brief Instantly zeroes out the memory for a specific looper overdub layer.
 * @param track_idx The index of the layer to clear.
 * @return void
 */
void clear_multitrack_track(int track_idx) {
    if (track_idx < 0 || track_idx >= MAX_TRACKS || !multitrack_tracks[track_idx]) return;

    if (!await_rt_thread_detach()) return; // Failsafe abort if JACK hangs

    memset(multitrack_tracks[track_idx], 0, pristine_frames * 2 * sizeof(float));
    atomic_store_explicit(&track_has_audio[track_idx], false, memory_order_release);
    atomic_store_explicit(&track_has_undo[track_idx], false, memory_order_release);

    resume_rt_thread();
}

/**
 * @brief Applies a smart destructive fade (In, Out, or V-Duck) to the active loop selection.
 * Enforces mathematically perfect 0.0f and 1.0f boundaries to prevent visual/auditory overlap.
 * @param track_idx The index of the layer to fade.
 * @return void
 */
void apply_smart_fade_to_track(int track_idx) {
    if (track_idx < 0 || track_idx >= MAX_TRACKS || !multitrack_tracks[track_idx]) return;
    if (!atomic_load_explicit(&loop_active, memory_order_acquire)) return;

    size_t start = atomic_load_explicit(&loop_start_frame, memory_order_acquire);
    size_t end = atomic_load_explicit(&loop_end_frame, memory_order_acquire);
    if (start >= end || end > pristine_frames) return;

    if (!await_rt_thread_detach()) return;

    internal_backup_track(track_idx);
    internal_smart_fade_math(track_idx, start, end);

    if (track_idx == 0 && pristine_bt_buf) {
        memcpy(pristine_bt_buf, multitrack_tracks[0], pristine_frames * 2 * sizeof(float));
        atomic_store_explicit(&stretch_queue.read_index, atomic_load_explicit(&stretch_queue.write_index, memory_order_relaxed), memory_order_release);
        atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);
    }

    resume_rt_thread();
    clear_loop_points();
}

/**
 * @brief Applies a Ripple Crossfade (Blend Fade) across the selection.
 * Fades out the left half, fades in the right half, mixes them together, and ripple-shifts
 * the remaining timeline leftwards by half the selection length to create a seamless overlap.
 * @param track_idx The index of the layer to fade.
 * @return void
 */
void apply_blend_fade_to_track(int track_idx) {
    if (track_idx < 0 || track_idx >= MAX_TRACKS || !multitrack_tracks[track_idx]) return;
    if (!atomic_load_explicit(&loop_active, memory_order_acquire)) return;

    size_t start = atomic_load_explicit(&loop_start_frame, memory_order_acquire);
    size_t end = atomic_load_explicit(&loop_end_frame, memory_order_acquire);
    if (start >= end || end > pristine_frames) return;

    if (!await_rt_thread_detach()) return;

    internal_backup_track(track_idx);
    internal_blend_fade_math(track_idx, start, end);

    if (track_idx == 0 && pristine_bt_buf) {
        memcpy(pristine_bt_buf, multitrack_tracks[0], pristine_frames * 2 * sizeof(float));
        atomic_store_explicit(&stretch_queue.read_index, atomic_load_explicit(&stretch_queue.write_index, memory_order_relaxed), memory_order_release);
        atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);
    }

    resume_rt_thread();
    clear_loop_points();
}

/**
 * @brief Restores the layer's audio buffer from the undo snapshot.
 * Bypasses the DSP barrier to safely overwrite the active memory.
 * @param track_idx The index of the layer to restore.
 * @return void
 */
void undo_track_edit(int track_idx) {
    if (track_idx < 0 || track_idx >= MAX_TRACKS || !multitrack_tracks[track_idx] || !undo_tracks[track_idx]) return;

    if (!await_rt_thread_detach()) return;

    internal_undo_math(track_idx);
    atomic_store_explicit(&track_has_undo[track_idx], false, memory_order_release);

    if (track_idx == 0 && pristine_bt_buf) {
        memcpy(pristine_bt_buf, multitrack_tracks[0], pristine_frames * 2 * sizeof(float));
        atomic_store_explicit(&stretch_queue.read_index, atomic_load_explicit(&stretch_queue.write_index, memory_order_relaxed), memory_order_release);
        atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);
    }

    resume_rt_thread();
}

/**
 * @brief Manually triggers an undo backup for a track. Used for debouncing rapid scroll wheel events.
 * @param track_idx The index of the track.
 */
void backup_track_for_edit(int track_idx) {
    if (track_idx < 0 || track_idx >= MAX_TRACKS || !multitrack_tracks[track_idx]) return;
    if (!await_rt_thread_detach()) return;
    internal_backup_track(track_idx);
    resume_rt_thread();
}

/**
 * @brief Destructively amplifies or attenuates the selected region of a track by a specific dB amount.
 * @param track_idx The index of the track to edit.
 * @param db_delta The relative volume change in decibels (e.g., +0.5 or -0.5).
 */
void amplify_track_selection(int track_idx, float db_delta) {
    if (!atomic_load_explicit(&loop_active, memory_order_acquire)) return;
    size_t start = atomic_load_explicit(&loop_start_frame, memory_order_acquire);
    size_t end = atomic_load_explicit(&loop_end_frame, memory_order_acquire);
    if (start >= end || end > pristine_frames) return;
    if (track_idx < 0 || track_idx >= MAX_TRACKS || !multitrack_tracks[track_idx]) return;

    float multiplier = powf(10.0f, db_delta / 20.0f);

    if (!await_rt_thread_detach()) return;

    // Calculate a 30ms sloped transition based on the active hardware sample rate
    int current_rate = atomic_load_explicit(&active_sample_rate, memory_order_acquire);
    size_t fade_len = (size_t)((current_rate > 0 ? current_rate : 48000) * 0.030);

    // Safety cap: don't let the fade overlap if the selection is extremely narrow
    if (end - start < fade_len * 2) fade_len = (end - start) / 2;

    for (size_t i = 0; i < fade_len; i++) {
        float progress = (float)i / (float)fade_len;
        // Apply an S-Curve (Smoothstep) for natural volume ramping instead of a hard linear angle
        float curve = progress * progress * (3.0f - 2.0f * progress);
        float current_mult = 1.0f + (multiplier - 1.0f) * curve;

        multitrack_tracks[track_idx][(start + i) * 2] *= current_mult;
        multitrack_tracks[track_idx][(start + i) * 2 + 1] *= current_mult;
    }

    for (size_t i = fade_len; i < (end - start) - fade_len; i++) {
        multitrack_tracks[track_idx][(start + i) * 2] *= multiplier;
        multitrack_tracks[track_idx][(start + i) * 2 + 1] *= multiplier;
    }

    for (size_t i = 0; i < fade_len; i++) {
        float progress = (float)i / (float)fade_len;
        float curve = progress * progress * (3.0f - 2.0f * progress);
        float current_mult = multiplier + (1.0f - multiplier) * curve;

        multitrack_tracks[track_idx][(end - fade_len + i) * 2] *= current_mult;
        multitrack_tracks[track_idx][(end - fade_len + i) * 2 + 1] *= current_mult;
    }

    if (track_idx == 0 && pristine_bt_buf) {
        memcpy(pristine_bt_buf, multitrack_tracks[0], pristine_frames * 2 * sizeof(float));
        atomic_store_explicit(&stretch_queue.read_index, atomic_load_explicit(&stretch_queue.write_index, memory_order_relaxed), memory_order_release);
        atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);
    }

    resume_rt_thread();
}

// ==============================================================================
// MASTER TRACK EDITS
// ==============================================================================

/**
 * @brief Master cut: extracts the loop region across all active multitrack layers
 * and shifts the remaining audio left, using a single RT DSP lock.
 * @return void
 */
void master_cut_selection(void) {
    if (!atomic_load_explicit(&loop_active, memory_order_acquire)) return;
    size_t start = atomic_load_explicit(&loop_start_frame, memory_order_acquire);
    size_t end = atomic_load_explicit(&loop_end_frame, memory_order_acquire);
    if (start >= end || end > pristine_frames) return;

    if (!await_rt_thread_detach()) return;

    int layers = atomic_load_explicit(&active_track_count, memory_order_acquire);
    for (int l = 0; l < layers && l < MAX_TRACKS; l++) {
        if (multitrack_tracks[l]) {
            internal_backup_track(l);
            internal_cut_math(l, start, end);
        }
    }

    if (pristine_bt_buf && multitrack_tracks[0]) {
        memcpy(pristine_bt_buf, multitrack_tracks[0], pristine_frames * 2 * sizeof(float));
        atomic_store_explicit(&stretch_queue.read_index, atomic_load_explicit(&stretch_queue.write_index, memory_order_relaxed), memory_order_release);
        atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);
    }

    atomic_store_explicit(&playback_pos, start, memory_order_release);
    resume_rt_thread();
    clear_loop_points();
}

/**
 * @brief Master smart fade: applies V-fade (in, out, or duck) across all active layers
 * synchronously under a single RT DSP lock.
 * @return void
 */
void master_smart_fade(void) {
    if (!atomic_load_explicit(&loop_active, memory_order_acquire)) return;
    size_t start = atomic_load_explicit(&loop_start_frame, memory_order_acquire);
    size_t end = atomic_load_explicit(&loop_end_frame, memory_order_acquire);
    if (start >= end || end > pristine_frames) return;

    if (!await_rt_thread_detach()) return;

    int layers = atomic_load_explicit(&active_track_count, memory_order_acquire);
    for (int l = 0; l < layers && l < MAX_TRACKS; l++) {
        if (multitrack_tracks[l]) {
            internal_backup_track(l);
            internal_smart_fade_math(l, start, end);
        }
    }

    if (pristine_bt_buf && multitrack_tracks[0]) {
        memcpy(pristine_bt_buf, multitrack_tracks[0], pristine_frames * 2 * sizeof(float));
        atomic_store_explicit(&stretch_queue.read_index, atomic_load_explicit(&stretch_queue.write_index, memory_order_relaxed), memory_order_release);
        atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);
    }

    resume_rt_thread();
    clear_loop_points();
}

/**
 * @brief Master blend fade: applies a ripple crossfade across all active layers
 * synchronously under a single RT DSP lock.
 * @return void
 */
void master_blend_fade(void) {
    if (!atomic_load_explicit(&loop_active, memory_order_acquire)) return;
    size_t start = atomic_load_explicit(&loop_start_frame, memory_order_acquire);
    size_t end = atomic_load_explicit(&loop_end_frame, memory_order_acquire);
    if (start >= end || end > pristine_frames) return;

    if (!await_rt_thread_detach()) return;

    int layers = atomic_load_explicit(&active_track_count, memory_order_acquire);
    for (int l = 0; l < layers && l < MAX_TRACKS; l++) {
        if (multitrack_tracks[l]) {
            internal_backup_track(l);
            internal_blend_fade_math(l, start, end);
        }
    }

    if (pristine_bt_buf && multitrack_tracks[0]) {
        memcpy(pristine_bt_buf, multitrack_tracks[0], pristine_frames * 2 * sizeof(float));
        atomic_store_explicit(&stretch_queue.read_index, atomic_load_explicit(&stretch_queue.write_index, memory_order_relaxed), memory_order_release);
        atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);
    }

    resume_rt_thread();
    clear_loop_points();
}

/**
 * @brief Master undo: restores all active layers from their undo snapshots
 * simultaneously under a single RT DSP lock.
 * @return void
 */
void master_undo_edits(void) {
    if (!await_rt_thread_detach()) return;

    int layers = atomic_load_explicit(&active_track_count, memory_order_acquire);
    for (int l = 0; l < layers && l < MAX_TRACKS; l++) {
        if (multitrack_tracks[l] && undo_tracks[l]) {
            if (atomic_load_explicit(&track_has_undo[l], memory_order_acquire)) {
                internal_undo_math(l);
                atomic_store_explicit(&track_has_undo[l], false, memory_order_release);
            }
        }
    }

    if (pristine_bt_buf && multitrack_tracks[0]) {
        memcpy(pristine_bt_buf, multitrack_tracks[0], pristine_frames * 2 * sizeof(float));
        atomic_store_explicit(&stretch_queue.read_index, atomic_load_explicit(&stretch_queue.write_index, memory_order_relaxed), memory_order_release);
        atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);
    }

    resume_rt_thread();
}
