// Standard C Libraries
#include <stdio.h>       // For printf, snprintf
#include <stdlib.h>      // For calloc, free
#include <stdatomic.h>   // For lock-free atomic variables
#include <math.h>        // For audio math/clipping
#include <ctype.h>       // For isdigit
#include <fcntl.h>       // For fcntl, O_NONBLOCK

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
#include <gio/gio.h>
#include "encoder.h"

// Project Local Headers
#include "config.h"
#include "audio_engine.h" // Provides <jack/jack.h> and function signatures
#include "encoder.h"
#include "video_ringbuffer.h" // Required for SPSC_Video_Queue
#include "ringbuffer.h"   // Provides AudioFrame and lock-free queue

#include <sys/mman.h>

/**
 * Helper functions to allocate zero-filled audio buffers.
 * Rely on calloc's native page-faulting rather than hitting strict OS mlock limits.
 */

static float* alloc_audio_buffer(size_t frames) {
    size_t bytes = frames * 2 * sizeof(float);
    return calloc(1, bytes);
}

static void free_audio_buffer(float *ptr, size_t frames) {
    (void)frames; // Retained for signature compatibility
    if (ptr) {
        free(ptr);
    }
}

typedef struct {
    float *ptr;
    size_t frames;
} DeferredFreeData;

static gboolean deferred_locked_free_cb(gpointer data) {
    DeferredFreeData *df = (DeferredFreeData *)data;
    free_audio_buffer(df->ptr, df->frames);
    free(df);
    return G_SOURCE_REMOVE;
}

static inline void safe_deferred_locked_free(float *ptr, size_t frames) {
    if (!ptr) return;
    DeferredFreeData *df = malloc(sizeof(DeferredFreeData));
    if (df) {
        df->ptr = ptr;
        df->frames = frames;
        g_idle_add(deferred_locked_free_cb, df);
    }
}

static inline void safe_deferred_free(void *ptr);

static void free_detached_ledger(UndoLedger *ledger, size_t frames) {
    if (!ledger || !ledger->states) return;
    for (int i = 0; i < ledger->max_capacity; i++) {
        if (ledger->states[i].buffer) safe_deferred_locked_free(ledger->states[i].buffer, frames);
    }
    safe_deferred_free(ledger->states);
}

static void free_detached_ledger_sync(UndoLedger *ledger, size_t frames) {
    if (!ledger || !ledger->states) return;
    for (int i = 0; i < ledger->max_capacity; i++) {
        if (ledger->states[i].buffer) free_audio_buffer(ledger->states[i].buffer, frames);
    }
    free(ledger->states);
}
// End helper finctions to call mlock

// UI -> RT Communication (Aligned to 64 bytes to prevent false sharing with RT writes)
/** Destructive pre-buffer multiplier applied directly to incoming hardware samples before writing to memory. */
alignas(64) static _Atomic float input_gain = 1.0f;

/** Master output multiplier applied dynamically during real-time playback. */
static _Atomic float bt_gain = 1.0f;
_Atomic float playback_speed = 1.0f;

_Atomic bool engine_is_recording = false;
_Atomic bool engine_is_armed = false;
_Atomic bool engine_is_paused = false;
_Atomic bool engine_is_playing = false;
_Atomic bool loop_active = false;
_Atomic size_t loop_start_frame = 0;
_Atomic size_t loop_end_frame = 0;

// SAFETY [Critical]: Heartbeat to detect PipeWire graph suspension
_Atomic uint32_t dsp_heartbeat = 0;

// SAFETY [Critical]: Gatekeeper to prevent DSP loop from accessing detached ports
_Atomic bool audio_available = true;

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

// RT Metronome State
static int click_frames_remaining = 0;
static double click_phase = 0.0;

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

// Multi-Track Multitrack Engine State
_Atomic int active_track_count = 0;
_Atomic int current_recording_track = 0;
_Atomic int timebase_track = 0;

UndoLedger global_undo_ledger = { .states = NULL, .max_capacity = 0, .head = 0, .tail = 0, .count = 0 };

QJTrackBuffer master_tracks[MAX_TRACKS] = {
    { .gain = 1.0f }, { .gain = 1.0f }, { .gain = 1.0f }, { .gain = 1.0f },
    { .gain = 1.0f }, { .gain = 1.0f }, { .gain = 1.0f }, { .gain = 1.0f },
    { .gain = 1.0f }, { .gain = 1.0f }, { .gain = 1.0f }, { .gain = 1.0f }
};

float *clipboard_buffers[MAX_TRACKS] = {NULL};
size_t clipboard_frames = 0;
_Atomic uint32_t selected_tracks_mask = 1;

atomic_size_t backing_track_frames = 0; // Dynamic tracking for UI playhead completion
alignas(64) atomic_size_t playback_pos = 0;
atomic_int active_sample_rate = 44100; // Will be immediately overwritten by JACK on init

_Atomic bool seek_flag = false;
_Atomic double seek_target = 0.0;
static int output_fade_remaining = 0;
_Atomic bool loop_wrap_pending = false;

_Atomic bool speed_change_flag = false;
_Atomic float speed_target = 1.0f;

static uint32_t current_transaction_id = 0;
uint32_t video_creation_trans_id = 0;
uint32_t video_deletion_trans_id = 0;

_Atomic uint32_t saved_transaction_id = 0;

static uint32_t get_current_transaction(void) {
    if (global_undo_ledger.count == 0 || global_undo_ledger.head == 0) return 0;
    int idx = (global_undo_ledger.tail + global_undo_ledger.head - 1) % global_undo_ledger.max_capacity;
    return global_undo_ledger.states[idx].transaction_id;
}

void evaluate_session_dirty_state(void) {
    if (get_current_transaction() == atomic_load_explicit(&saved_transaction_id, memory_order_acquire)) {
        atomic_store_explicit(&global_undo_ledger.is_dirty, false, memory_order_release);
    } else {
        atomic_store_explicit(&global_undo_ledger.is_dirty, true, memory_order_release);
    }
}

bool is_session_dirty(void) {
    return atomic_load_explicit(&global_undo_ledger.is_dirty, memory_order_acquire);
}

void mark_session_saved(void) {
    atomic_store_explicit(&saved_transaction_id, get_current_transaction(), memory_order_release);
    atomic_store_explicit(&global_undo_ledger.is_dirty, false, memory_order_release);
}

void mark_session_dirty(void) {
    atomic_store_explicit(&global_undo_ledger.is_dirty, true, memory_order_release);
}

void mark_current_transaction_as_video(void) {
    video_creation_trans_id = current_transaction_id;
    video_deletion_trans_id = 0;
}

void mark_current_transaction_as_video_deletion(void) {
    video_deletion_trans_id = current_transaction_id;
}

static void push_undo_state(int track_idx, uint32_t trans_id);
static void free_detached_ledger(UndoLedger *ledger, size_t frames);

// Atomic Handshake flags for lock-free memory swapping
_Atomic bool request_track_free = false;
_Atomic bool safe_to_free_track = false;
_Atomic bool stretcher_safe_to_free = false;
_Atomic bool ui_scanner_is_busy = false;

// --- GLOBAL RT ANOMALY COUNTERS ---
_Atomic uint32_t rt_dropped_audio_frames = 0;
_Atomic uint32_t rt_cmd_queue_full = 0;

/**
 * @brief GTK idle callback to safely free memory on the UI thread.
 * @param data Pointer to the memory block.
 * @return G_SOURCE_REMOVE
 */
static gboolean deferred_free_cb(gpointer data) {
    log_and_notify(0, "[MEM-TEARDOWN] Deferred release: Pointer=%p\n", data);
    free(data);
    return G_SOURCE_REMOVE;
}

/**
 * @brief Defers memory deallocation to the GTK main loop, preventing Use-After-Free crashes during drawing.
 * @param ptr Pointer to the memory block to free.
 */
static inline void safe_deferred_free(void *ptr) {
    if (ptr) g_idle_add(deferred_free_cb, ptr);
}

/**
 * @brief GTK idle callback to safely destroy a RubberBand state on the UI thread.
 * @param data Pointer to the RubberBandState.
 * @return G_SOURCE_REMOVE
 */
static gboolean deferred_rb_free_cb(gpointer data) {
    log_and_notify(0, "[MEM-TEARDOWN] Deferred release: RubberBandState=%p\n", data);
    rubberband_delete((RubberBandState)data);
    return G_SOURCE_REMOVE;
}

/**
 * @brief Defers RubberBand state destruction to the GTK main loop.
 * @param rb The RubberBandState to destroy.
 */
static inline void safe_deferred_rb_free(RubberBandState rb) {
    if (rb) g_idle_add(deferred_rb_free_cb, rb);
}

/** Serializes access to the RT detachment barrier to prevent UI and background thread collisions. */
static pthread_mutex_t dsp_barrier_mutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * @brief Centralized synchronization barrier. Requests all DSP threads (RT and Stretcher) to detach from active memory buffers and spins until safe.
 * @return true if both threads successfully detached, false if the operation timed out.
 */
bool await_rt_thread_detach(void) {
    pthread_mutex_lock(&dsp_barrier_mutex);

    atomic_store_explicit(&safe_to_free_track, false, memory_order_relaxed);
    atomic_store_explicit(&stretcher_safe_to_free, false, memory_order_relaxed);

    // PRE-FLIGHT CHECK: Determine if PipeWire has suspended the audio graph
    uint32_t h1 = atomic_load_explicit(&dsp_heartbeat, memory_order_relaxed);
    usleep(25000); // 25ms is enough for any buffer size under 1024 to tick
    uint32_t h2 = atomic_load_explicit(&dsp_heartbeat, memory_order_relaxed);
    bool rt_is_ticking = (client != NULL && h1 != h2);

    atomic_store_explicit(&request_track_free, true, memory_order_release);
    int timeout = 500;

    while (timeout > 0) {
        bool rt_safe = rt_is_ticking ? atomic_load_explicit(&safe_to_free_track, memory_order_acquire) : true;
        bool stretcher_safe = atomic_load_explicit(&stretcher_safe_to_free, memory_order_acquire);
        bool ui_safe = !atomic_load_explicit(&ui_scanner_is_busy, memory_order_acquire);

        if (rt_safe && stretcher_safe && ui_safe) break;

        usleep(1000);
        timeout--;
    }

    if (timeout == 0) {
        log_and_notify(0, "[DSP-SYNC:CRITICAL] Detach Timeout (500ms). Handshake aborted. (rt_ticking=%d)\n", rt_is_ticking);
        atomic_store_explicit(&request_track_free, false, memory_order_release);
        pthread_mutex_unlock(&dsp_barrier_mutex);
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
    pthread_mutex_unlock(&dsp_barrier_mutex);
}

/**
 * @brief RAM Gatekeeper: Dynamically calculates the safe maximum history capacity for the active canvas size.
 * Enforces a strict 2GB OS floor to protect the kernel and background muxing from OOM kills.
 * @param source_frames Total frames in the source file or target canvas.
 * @param source_rate Sample rate of the source file.
 * @param target_rate The active JACK hardware sample rate.
 * @return Safe integer multiplier for the UndoLedger pool (minimum 1, maximum 64).
 */
static int calculate_safe_history_capacity(sf_count_t source_frames, int source_rate, int target_rate) {
    if (source_rate <= 0 || target_rate <= 0) return 1;

    double ratio = (double)target_rate / (double)source_rate;
    size_t pristine_frames_est = (size_t)(source_frames * ratio);
    size_t track_bytes = pristine_frames_est * 2 * sizeof(float);

    size_t base_monolith_bytes = track_bytes + (track_bytes * MAX_TRACKS);

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

    if (available_ram == 0) {
        long pages = sysconf(_SC_AVPHYS_PAGES);
        long page_size = sysconf(_SC_PAGE_SIZE);
        if (pages > 0 && page_size > 0) available_ram = (size_t)pages * (size_t)page_size;
    }

    size_t os_floor_bytes = 2048ULL * 1024 * 1024; // 2GB Absolute Floor

    if (available_ram <= (os_floor_bytes + base_monolith_bytes)) {
        log_and_notify(0, "[MEM-GATE] Track allocation: OS Floor hit during swap validation.\n");
        return 0;
    }

    size_t currently_held_bytes = 0;
    if (pristine_bt_buf) currently_held_bytes += pristine_frames * 2 * sizeof(float);
    for (int i = 0; i < MAX_TRACKS; i++) {
        if (master_tracks[i].active_buffer) currently_held_bytes += pristine_frames * 2 * sizeof(float);
    }

    currently_held_bytes += global_undo_ledger.max_capacity * (pristine_frames * 2 * sizeof(float));

    size_t long_term_ram = available_ram + currently_held_bytes;
    size_t remaining_bytes = long_term_ram - os_floor_bytes - base_monolith_bytes;

    int capacity = (int)(remaining_bytes / track_bytes);

    if (capacity > 64) capacity = 64;
    if (capacity < MAX_TRACKS) capacity = 0;

    log_and_notify(0, "[MEM-GATE] Dynamic History Calc: Floor=2GB | Base=%.1f MB | Remaining=%.1f MB | Capacity=%d states\n",
                   (double)base_monolith_bytes / (1024 * 1024), (double)remaining_bytes / (1024 * 1024), capacity);

    return capacity;
}

/**
 * @brief Drains all available frames from the provided decoder context and extracts them as normalized interleaved floats.
 * Dynamically resizes the destination buffer if the extracted samples exceed current capacity.
 * @param codec_ctx The FFmpeg codec context.
 * @param frame The pre-allocated FFmpeg frame.
 * @param raw_buf Pointer to the destination float array.
 * @param capacity Pointer to the current capacity (in frames).
 * @param total_samples Pointer to the total extracted samples.
 * @param channels Number of audio channels.
 * @return 0 on success, -1 on allocation failure or integer overflow.
 */
static int drain_and_extract_frames(AVCodecContext *codec_ctx, AVFrame *frame, float **raw_buf, size_t *capacity, size_t *total_samples, int channels) {
    while (avcodec_receive_frame(codec_ctx, frame) == 0) {
        int nb_samples = frame->nb_samples;

        // CRITICAL: Prevent integer overflow during sample counting and allocation size calculations
        if ((size_t)nb_samples > SIZE_MAX / channels) return -1;
        size_t new_samples = (size_t)nb_samples * channels;

        if (SIZE_MAX - *total_samples < new_samples) return -1;

        if (*total_samples + new_samples > *capacity * channels) {
            if (*capacity > SIZE_MAX / 2) return -1;
            size_t new_cap = *capacity * 2;

            if (new_cap > SIZE_MAX / (channels * sizeof(float))) return -1;

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
        if (calculate_safe_history_capacity(estimated_frames, sample_rate, target_jack_rate) == 0) {
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
                    av_packet_unref(pkt); // Prevent leak on abort
                    goto loop_cleanup;
                }
            }
        } else if (codec_ctx2 && pkt->stream_index == stream_idx2) {
            if (avcodec_send_packet(codec_ctx2, pkt) == 0) {
                if (drain_and_extract_frames(codec_ctx2, frame, &raw_buf2, &capacity2, &total_samples2, ch2) < 0) {
                    av_packet_unref(pkt); // Prevent leak on abort
                    goto loop_cleanup;
                }
            }
        }
        av_packet_unref(pkt); // Safely drops the internal payload
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
            float current_speed = atomic_load_explicit(&playback_speed, memory_order_relaxed);
            size_t sync_pos = atomic_load_explicit(&playback_pos, memory_order_acquire);
            size_t target_pristine = (size_t)(sync_pos * current_speed);
            if (target_pristine > pristine_frames) target_pristine = pristine_frames;
            atomic_store_explicit(&pristine_read_pos, target_pristine, memory_order_release);
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

/**
* @brief Core lock-free audio processing loop running at SCHED_FIFO priority.
* Iterates through the unified master_tracks struct array to perform real-time mixing,
* routing, and encoding without memory allocation or blocking calls.
* @param nframes Number of frames to process.
* @param arg Opaque user data.
* @return 0 on success.
*/
int process_audio(jack_nframes_t nframes, void *arg) {
    (void)arg;

    atomic_fetch_add_explicit(&dsp_heartbeat, 1, memory_order_relaxed);

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
            case CMD_PLAY_CLICK: {
                int rate = atomic_load_explicit(&active_sample_rate, memory_order_acquire);
                click_frames_remaining = (int)((rate > 0 ? rate : 48000) * 0.05); // 50ms burst
                click_phase = 0.0;
                break;
            }
            default:
                break;
        }
    }

    // SAFETY [Critical]: Acknowledge memory barrier BEFORE the audio gatekeeper to prevent UI deadlocks during imports
    if (atomic_load_explicit(&request_track_free, memory_order_acquire)) {
        atomic_store_explicit(&safe_to_free_track, true, memory_order_release);
        for (jack_nframes_t i = 0; i < nframes; i++) {
            out_1[i] = 0.0f;
            out_2[i] = 0.0f;
        }
        return 0;
    }

    // SAFETY [Critical]: Bypass processing if hardware is actively dropping/re-routing
    if (!atomic_load_explicit(&audio_available, memory_order_acquire)) {
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

            extern size_t video_start_frame;
            extern size_t video_end_frame;
            double v_start_frac = (backing_track_frames > 0) ? ((double)video_start_frame / (double)backing_track_frames) : 0.0;
            double v_end_frac = (backing_track_frames > 0) ? ((double)video_end_frame / (double)backing_track_frames) : 0.0;

            size_t new_bt_frames = (size_t)(pristine_frames / speed);
            atomic_store_explicit(&backing_track_frames, new_bt_frames, memory_order_release);

            current_pos = (size_t)(fraction * new_bt_frames);

            // (Removed the invalid active_pos assignment here)
            atomic_store_explicit(&playback_pos, current_pos, memory_order_release);

            if (atomic_load_explicit(&loop_active, memory_order_acquire)) {
                atomic_store_explicit(&loop_start_frame, (size_t)(loop_s_frac * new_bt_frames), memory_order_relaxed);
                atomic_store_explicit(&loop_end_frame, (size_t)(loop_e_frac * new_bt_frames), memory_order_relaxed);
            }

            video_start_frame = (size_t)(v_start_frac * new_bt_frames);
            video_end_frame = (size_t)(v_end_frac * new_bt_frames);

            atomic_store_explicit(&stretch_queue.read_index, atomic_load_explicit(&stretch_queue.write_index, memory_order_relaxed), memory_order_release);
            atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);
        }
    }

    if (do_seek) {
        double frac = atomic_load_explicit(&seek_target, memory_order_relaxed);
        current_pos = (size_t)(frac * backing_track_frames);

        // (Removed the invalid active_pos assignment here)
        atomic_store_explicit(&playback_pos, current_pos, memory_order_release);

        atomic_store_explicit(&stretch_queue.read_index, atomic_load_explicit(&stretch_queue.write_index, memory_order_relaxed), memory_order_release);
        atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);
        output_fade_remaining = 128;
    }

    bool is_looping = atomic_load_explicit(&loop_active, memory_order_acquire);
    size_t l_start = atomic_load_explicit(&loop_start_frame, memory_order_relaxed);
    size_t l_end = atomic_load_explicit(&loop_end_frame, memory_order_relaxed);
    size_t active_pos = current_pos;
    bool pushed_audio = false;

    for (jack_nframes_t i = 0; i < nframes; i++) {
        if (active && !paused && is_looping && active_pos >= l_end) {
            if (recording) {
                // Pause the RT playhead and signal the UI thread to cycle the take
                atomic_store_explicit(&loop_wrap_pending, true, memory_order_release);
                atomic_store_explicit(&engine_is_paused, true, memory_order_release);
            } else {
                active_pos = l_start + (active_pos - l_end);
                atomic_store_explicit(&stretch_queue.read_index, atomic_load_explicit(&stretch_queue.write_index, memory_order_relaxed), memory_order_release);
                atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);
                output_fade_remaining = 128;
            }
        }

        float raw_bt_l = 0.0f;
        float raw_bt_r = 0.0f;

        int current_timebase = atomic_load_explicit(&timebase_track, memory_order_relaxed);
        float current_speed = atomic_load_explicit(&playback_speed, memory_order_relaxed);

        // Scale raw index by current speed to ensure overdubs sync with stretched timebase
        size_t raw_pos = (size_t)(active_pos * current_speed);

        if (active && !paused) {
            float l0_gain = atomic_load_explicit(&master_tracks[current_timebase].gain, memory_order_relaxed);

            if (current_speed == 1.0f && master_tracks[current_timebase].active_buffer) {
                if (raw_pos < pristine_frames) {
                    raw_bt_l = master_tracks[current_timebase].active_buffer[raw_pos * 2] * l0_gain;
                    raw_bt_r = master_tracks[current_timebase].active_buffer[raw_pos * 2 + 1] * l0_gain;
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
            if (atomic_load_explicit(&master_tracks[l].is_soloed, memory_order_relaxed)) {
                any_solo_active = true;
                break;
            }
        }

        if (active && !paused && active_pos < backing_track_frames) {
            bool base_audible = true;
            if (any_solo_active && !atomic_load_explicit(&master_tracks[current_timebase].is_soloed, memory_order_relaxed)) base_audible = false;

            if (!base_audible) {
                raw_bt_l = 0.0f;
                raw_bt_r = 0.0f;
            }

            for (int l = 0; l < layers && l < MAX_TRACKS; l++) {
                if (l == current_timebase) continue;
                if (!master_tracks[l].active_buffer) continue;

                bool overdub_audible = true;
                if (any_solo_active && !atomic_load_explicit(&master_tracks[l].is_soloed, memory_order_relaxed)) overdub_audible = false;

                if (overdub_audible && raw_pos < pristine_frames) {
                    float l_gain = atomic_load_explicit(&master_tracks[l].gain, memory_order_relaxed);
                    raw_bt_l += master_tracks[l].active_buffer[raw_pos * 2] * l_gain;
                    raw_bt_r += master_tracks[l].active_buffer[raw_pos * 2 + 1] * l_gain;
                }
            }
        }

        for (int l = 0; l < MAX_TRACKS; l++) {
            if (atomic_load_explicit(&master_tracks[l].is_soloed, memory_order_relaxed)) {
                any_solo_active = true;
                break;
            }
        }

        if (active && !paused && active_pos < backing_track_frames) {
            bool base_audible = true;
            if (any_solo_active && !atomic_load_explicit(&master_tracks[0].is_soloed, memory_order_relaxed)) base_audible = false;

            if (!base_audible) {
                raw_bt_l = 0.0f;
                raw_bt_r = 0.0f;
            }

            for (int l = 1; l < layers && l < MAX_TRACKS; l++) {
                if (!master_tracks[l].active_buffer) continue;

                bool overdub_audible = true;
                if (any_solo_active && !atomic_load_explicit(&master_tracks[l].is_soloed, memory_order_relaxed)) overdub_audible = false;

                if (overdub_audible && active_pos < pristine_frames) {
                    float l_gain = atomic_load_explicit(&master_tracks[l].gain, memory_order_relaxed);
                    raw_bt_l += master_tracks[l].active_buffer[active_pos * 2] * l_gain;
                    raw_bt_r += master_tracks[l].active_buffer[active_pos * 2 + 1] * l_gain;
                }
            }
        }

        if (active && !paused && is_looping && l_end > l_start + 256) {
            float fade_mult = 1.0f;
            if (active_pos >= l_end) {
                fade_mult = 0.0f;
            } else if (active_pos >= l_end - 128) {
                fade_mult = (float)(l_end - active_pos) / 128.0f;
            } else if (active_pos < l_start + 128) {
                fade_mult = (float)(active_pos - l_start) / 128.0f;
            }
            raw_bt_l *= fade_mult;
            raw_bt_r *= fade_mult;
        }

        float scaled_bt_l = raw_bt_l * current_bt_gain;
        float scaled_bt_r = raw_bt_r * current_bt_gain;

        if (output_fade_remaining > 0) {
            float fade_factor = (float)(128 - output_fade_remaining) / 128.0f;
            scaled_bt_l *= fade_factor;
            scaled_bt_r *= fade_factor;
            output_fade_remaining--;
        }

        if (fabsf(scaled_bt_l) > max_bt_l) max_bt_l = fabsf(scaled_bt_l);
        if (fabsf(scaled_bt_r) > max_bt_r) max_bt_r = fabsf(scaled_bt_r);

        out_1[i] = soft_clip(scaled_bt_l);
        out_2[i] = soft_clip(scaled_bt_r);

        // Synthesize 1500Hz percussive metronome tick directly to hardware output
        if (click_frames_remaining > 0) {
            int rate = atomic_load_explicit(&active_sample_rate, memory_order_acquire);
            if (rate <= 0) rate = 48000;
            int total_click_frames = (int)(rate * 0.05); // 50ms total window

            // 1.5kHz tone cuts through clearly without being abrasive
            double phase_inc = 2.0 * M_PI * 1500.0 / (double)rate;

            // Create a steep cubic decay envelope (1.0 -> 0.0) for a natural "tick" sound
            float env = (float)click_frames_remaining / (float)total_click_frames;
            env = env * env * env;

            // Apply envelope to the sine wave at -12dBFS (0.25 linear)
            float click_samp = (float)(sin(click_phase) * 0.25f) * env;

            click_phase += phase_inc;
            if (click_phase >= 2.0 * M_PI) click_phase -= 2.0 * M_PI;

            out_1[i] = soft_clip(out_1[i] + click_samp);
            out_2[i] = soft_clip(out_2[i] + click_samp);

            click_frames_remaining--;
        }

        float raw_input_l = in_1[i];
        float raw_input_r = in_2[i];
        float scaled_input_l = raw_input_l * current_input_gain;
        float scaled_input_r = raw_input_r * current_input_gain;

        if (fabsf(scaled_input_l) > max_l) max_l = fabsf(scaled_input_l);
        if (fabsf(scaled_input_r) > max_r) max_r = fabsf(scaled_input_r);

        if (recording && !paused && raw_pos < pristine_frames) {
            int rec_track = atomic_load_explicit(&current_recording_track, memory_order_relaxed);
            if (rec_track >= 0 && rec_track < MAX_TRACKS && master_tracks[rec_track].active_buffer) {
                master_tracks[rec_track].active_buffer[raw_pos * 2] = scaled_input_l;
                master_tracks[rec_track].active_buffer[raw_pos * 2 + 1] = scaled_input_r;
            }

            extern _Atomic bool video_is_armed;
            if (atomic_load_explicit(&video_is_armed, memory_order_acquire)) {
                AudioFrame *frame = acquire_audio_frame(&audio_queue);
                if (frame) {
                    // 1. MASTER MIX (Destructive routing for MKV Stream 0)
                    frame->mix_l = soft_clip(scaled_bt_l + scaled_input_l);
                    frame->mix_r = soft_clip(scaled_bt_r + scaled_input_r);

                    // 2. ISOLATED TRACKS (Mapped exactly to UI Layers for MKV Streams 1-N)
                    int active_count = atomic_load_explicit(&active_track_count, memory_order_relaxed);

                    for (int t = 0; t < MAX_TRACKS; t++) {
                        if (t == rec_track) {
                            frame->tracks_l[t] = scaled_input_l;
                            frame->tracks_r[t] = scaled_input_r;
                        } else if (t < active_count && master_tracks[t].active_buffer && atomic_load_explicit(&master_tracks[t].has_audio, memory_order_relaxed)) {
                            float l_gain = atomic_load_explicit(&master_tracks[t].gain, memory_order_relaxed);
                            frame->tracks_l[t] = master_tracks[t].active_buffer[raw_pos * 2] * l_gain;
                            frame->tracks_r[t] = master_tracks[t].active_buffer[raw_pos * 2 + 1] * l_gain;
                        } else {
                            frame->tracks_l[t] = 0.0f;
                            frame->tracks_r[t] = 0.0f;
                        }
                    }

                    frame->active_count = active_count;
                    frame->pts = (int64_t)(cycle_start_time + i);
                    commit_audio_frame(&audio_queue);
                    pushed_audio = true;
                } else {
                    atomic_fetch_add_explicit(&rt_dropped_audio_frames, 1, memory_order_relaxed);
                }
            }
        }

        if (active && !paused) {
            active_pos++;
        }
    }

    if (pushed_audio) wake_encoder();

    update_peak_decay(&vu_peak_input_l, max_l);
    update_peak_decay(&vu_peak_input_r, max_r);
    update_peak_decay(&vu_peak_bt_l, max_bt_l);
    update_peak_decay(&vu_peak_bt_r, max_bt_r);

    if (active && !paused) {
        atomic_store_explicit(&playback_pos, active_pos, memory_order_release);
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
 * @brief Immediately severs JACK graph links to clear PipeWire GC locks during an unplug.
 */
void qjams_drop_connections(void) {
    atomic_store_explicit(&audio_available, false, memory_order_release);
    if (client && input_port_1 && input_port_2) {
        jack_port_disconnect(client, input_port_1);
        jack_port_disconnect(client, input_port_2);
        jack_port_disconnect(client, output_port_1);
        jack_port_disconnect(client, output_port_2);
        log_and_notify(0, "[JACK DSP] Graph links severed to clear PipeWire GC locks.\n");
    }
}

/**
 * @brief Connects physical USB hardware inputs to the internal QJams capture ports.
 * Integrates ghost-pruning to bypass dead file descriptors during rapid hot-plugging.
 * @param target_audio_device The name identifier of the hardware to connect.
 * @return 0 on success, 2 if the ports could not be located or routed.
 */
int patch_audio_ports(const char* target_audio_device) {
    if (!client) return 2;

    log_and_notify(0, "Attempting to patch capture device: %s\n", target_audio_device);

    atomic_store_explicit(&audio_available, false, memory_order_release);
    jack_port_disconnect(client, input_port_1);
    jack_port_disconnect(client, input_port_2);

    const char **ports = wait_for_jack_ports(client, target_audio_device, JackPortIsOutput);
    int connected = 0;

    if (ports) {
        // SAFETY [Critical]: Ghost Pruning. Scan all ports for the highest suffix to bypass dead file descriptors.
        const char *best_l = NULL;
        const char *best_r = NULL;
        int highest_l = -1, highest_r = -1;

        char prefix[256];
        snprintf(prefix, sizeof(prefix), "%s:", target_audio_device);
        size_t prefix_len = strlen(prefix);

        for (int i = 0; ports[i] != NULL; i++) {
            if (strncmp(ports[i], prefix, prefix_len) == 0) {
                const char *local_name = ports[i] + prefix_len;
                int suffix = 0;
                char base_name[256] = {0};
                strncpy(base_name, local_name, sizeof(base_name) - 1);
                char *dash = strrchr(base_name, '-');
                if (dash && isdigit(*(dash + 1))) {
                    suffix = atoi(dash + 1);
                    *dash = '\0';
                }

                bool is_l = (strstr(base_name, "AUX0") || strstr(base_name, "FL") || strstr(base_name, "capture_1") || strstr(base_name, "left"));
                bool is_r = (strstr(base_name, "AUX1") || strstr(base_name, "FR") || strstr(base_name, "capture_2") || strstr(base_name, "right"));

                if (!is_l && !is_r) {
                    if (!best_l || highest_l < suffix) is_l = true;
                    else if (!best_r || highest_r < suffix) is_r = true;
                }

                if (is_l && suffix > highest_l) { highest_l = suffix; best_l = ports[i]; }
                else if (is_r && suffix > highest_r) { highest_r = suffix; best_r = ports[i]; }
            }
        }

        if (best_l && jack_connect(client, best_l, jack_port_name(input_port_1)) == 0) {
            log_and_notify(0, "[JACK DSP] Connected Capture L: %s -> %s\n", best_l, jack_port_name(input_port_1));
            connected++;
        }
        if (best_r && jack_connect(client, best_r, jack_port_name(input_port_2)) == 0) {
            log_and_notify(0, "[JACK DSP] Connected Capture R: %s -> %s\n", best_r, jack_port_name(input_port_2));
            connected++;
        }
        jack_free(ports);
    }

    if (connected < 2) {
        log_and_notify(0, "Warning: Failure mapping complete stereo ports for audio device '%s'.\n", target_audio_device);
        return 2;
    }

    atomic_store_explicit(&audio_available, true, memory_order_release);
    return 0;
}

/**
 * @brief Connects QJams output ports to the physical hardware playback ports.
 * @param target_playback_device The name identifier of the hardware to connect.
 * @return 0 on success, 2 if the ports could not be located or routed.
 */
int patch_playback_ports(const char* target_playback_device) {
    if (!client) return 2;

    log_and_notify(0, "Attempting to patch playback device: %s\n", target_playback_device);

    jack_port_disconnect(client, output_port_1);
    jack_port_disconnect(client, output_port_2);

    const char **ports = wait_for_jack_ports(client, target_playback_device, JackPortIsPhysical | JackPortIsInput);
    int connected = 0;

    if (ports) {
        // SAFETY [Critical]: Ghost Pruning for Playback
        const char *best_l = NULL;
        const char *best_r = NULL;
        int highest_l = -1, highest_r = -1;

        char prefix[256];
        snprintf(prefix, sizeof(prefix), "%s:", target_playback_device);
        size_t prefix_len = strlen(prefix);

        for (int i = 0; ports[i] != NULL; i++) {
            if (strncmp(ports[i], prefix, prefix_len) == 0) {
                const char *local_name = ports[i] + prefix_len;
                int suffix = 0;
                char base_name[256] = {0};
                strncpy(base_name, local_name, sizeof(base_name) - 1);
                char *dash = strrchr(base_name, '-');
                if (dash && isdigit(*(dash + 1))) {
                    suffix = atoi(dash + 1);
                    *dash = '\0';
                }

                bool is_l = (strstr(base_name, "AUX0") || strstr(base_name, "FL") || strstr(base_name, "playback_1") || strstr(base_name, "left"));
                bool is_r = (strstr(base_name, "AUX1") || strstr(base_name, "FR") || strstr(base_name, "playback_2") || strstr(base_name, "right"));

                if (!is_l && !is_r) {
                    if (!best_l || highest_l < suffix) is_l = true;
                    else if (!best_r || highest_r < suffix) is_r = true;
                }

                if (is_l && suffix > highest_l) { highest_l = suffix; best_l = ports[i]; }
                else if (is_r && suffix > highest_r) { highest_r = suffix; best_r = ports[i]; }
            }
        }

        if (best_l && jack_connect(client, jack_port_name(output_port_1), best_l) == 0) {
            log_and_notify(0, "[JACK DSP] Connected Playback L: %s -> %s\n", jack_port_name(output_port_1), best_l);
            connected++;
        }
        if (best_r && jack_connect(client, jack_port_name(output_port_2), best_r) == 0) {
            log_and_notify(0, "[JACK DSP] Connected Playback R: %s -> %s\n", jack_port_name(output_port_2), best_r);
            connected++;
        }
        jack_free(ports);
    }

    if (connected == 0) {
        log_and_notify(0, "Warning: Failure mapping ports for playback device '%s'.\n", target_playback_device);
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
    log_and_notify(0, "JACK sample rate dynamically updated to %d Hz\n", nframes);
    return 0;
}

static int event_pipe[2] = {-1, -1};

/**
 * @brief Returns the read end file descriptor of the JACK event synchronization pipe.
 * @return File descriptor integer, or -1 if uninitialized.
 */
int get_jack_event_fd(void) {
    return event_pipe[0];
}

/**
 * @brief Callback triggered when the JACK server forcefully shuts down or disconnects.
 * @param arg Opaque pointer to user data.
 */
void jack_shutdown(void *arg) {
    (void)arg;
    // SAFETY [Critical]: Lock-free async signal handler safely notifying the GTK thread via IPC.
    // Absolutely no mutexes, memory allocations, or GTK calls are permitted here.
    if (event_pipe[1] != -1) {
        const uint8_t msg = 1;
        (void)write(event_pipe[1], &msg, sizeof(msg));
    }
}

/**
* @brief Callback triggered when JACK detects a hardware port connection or disconnection.
* Logs the specific hardware port name to the unified logger before alerting the UI.
* @param port_id The ID of the port.
* @param register_port Non-zero if registering, zero if unregistering.
* @param arg Opaque user data.
*/
void jack_port_registration_cb(jack_port_id_t port_id, int register_port, void *arg) {
    (void)arg;

    // Attempt to grab the exact name of the hardware that triggered the event
    const char *port_name = "Unknown Port";
    if (client) {
        jack_port_t *port = jack_port_by_id(client, port_id);
        if (port) {
            port_name = jack_port_name(port);
        }
    }

    // Log the specific event directly to QJams.log
    log_and_notify(0, "[JACK EVENT] Hardware Port %s: %s\n",
                   register_port ? "ADDED" : "REMOVED", port_name);

    // SAFETY [Critical]: Strict lock-free IPC write, completely decoupled from GTK main loop overhead.
    if (event_pipe[1] != -1) {
        const uint8_t msg = register_port ? 2 : 3;
        (void)write(event_pipe[1], &msg, sizeof(msg));
    }
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
 * @warning CRITICAL: Pointer swapping must occur exclusively while the RT thread is detached to prevent use-after-free faults.
 * @param filepath The absolute path to the audio file.
 * @param track_idx The target layer index (0 to MAX_TRACKS - 1).
 * @return 0 on success, -1 on format error, -2 on memory failure, -3 on corrupt media.
 */
int import_to_layer(const char* filepath, int track_idx) {
    if (!client || track_idx < 0 || track_idx >= MAX_TRACKS || pristine_frames == 0) return -1;

    SF_INFO sfinfo = {0};
    float* raw_data = NULL;
    jack_nframes_t jack_rate = jack_get_sample_rate(client);

    SNDFILE *file = NULL;
    if (!g_str_has_suffix(filepath, ".mp3") && !g_str_has_suffix(filepath, ".MP3")) {
        file = sf_open(filepath, SFM_READ, &sfinfo);
    }

    if (file) {
        if (calculate_safe_history_capacity(sfinfo.frames, sfinfo.samplerate, jack_rate) == 0) {
            sf_close(file);
            return -2;
        }

        raw_data = calloc(sfinfo.frames * sfinfo.channels, sizeof(float));
        if (!raw_data) {
            sf_close(file);
            return -2;
        }
        sf_readf_float(file, raw_data, sfinfo.frames);
        sf_close(file);
    } else {
        int rate = 0, channels = 0;
        sf_count_t frames = 0;
        int ret = load_audio_ffmpeg(filepath, &raw_data, &frames, &channels, &rate, NULL, NULL, jack_rate);
        if (ret != 0) {
            return ret; // FIX: Propagate the actual error code (-2 for RAM, etc.)
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

    size_t current_pos = atomic_load_explicit(&playback_pos, memory_order_acquire);
    if (current_pos + resampled_frames > pristine_frames) {
        int required_seconds = (int)((current_pos + resampled_frames) / jack_rate) + 1;
        resize_loop_canvas_seconds(required_seconds);
    }

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

    current_transaction_id++;
    push_undo_state(track_idx, current_transaction_id);

    int current_timebase = atomic_load_explicit(&timebase_track, memory_order_acquire);
    if (!atomic_load_explicit(&master_tracks[current_timebase].has_audio, memory_order_acquire)) {
        atomic_store_explicit(&timebase_track, track_idx, memory_order_release);
        current_timebase = track_idx;
    }

    memcpy(master_tracks[track_idx].active_buffer, new_layer_buf, pristine_frames * 2 * sizeof(float));
    free(new_layer_buf);

    if (track_idx == current_timebase && pristine_bt_buf) {
        memcpy(pristine_bt_buf, master_tracks[current_timebase].active_buffer, pristine_frames * 2 * sizeof(float));
        atomic_store_explicit(&stretch_queue.read_index, atomic_load_explicit(&stretch_queue.write_index, memory_order_relaxed), memory_order_release);
        atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);
    }

    int active = atomic_load_explicit(&active_track_count, memory_order_acquire);
    if (track_idx >= active) {
        atomic_store_explicit(&active_track_count, track_idx + 1, memory_order_release);
    }

    atomic_store_explicit(&master_tracks[track_idx].has_audio, true, memory_order_release);

    // Auto-focus the newly imported track into the bitmask so Ctrl+Z works immediately
    atomic_store_explicit(&current_recording_track, track_idx, memory_order_release);
    atomic_store_explicit(&selected_tracks_mask, (1 << track_idx), memory_order_release);

    resume_rt_thread();

    // TELEMETRY: Safely emit the decode summary
    char *base_name = g_path_get_basename(filepath);
    log_and_notify(0, "[IO-DECODE] Loaded: \"%s\" | Track %d | %dHz -> %dHz (Ratio=%.3f) | Frames=%zu | Status=%s\n",
           base_name, track_idx, sfinfo.samplerate, jack_rate, ratio, resampled_frames, truncated ? "TRUNCATED" : "OK");
    g_free(base_name);

    return truncated ? 1 : 0;
}

/**
 * @brief Loads an audio file, resamples it to match the JACK sample rate, and initializes the RubberBand stretcher.
 * @warning CRITICAL: Memory allocations occur outside the barrier. Array assignments swap pointers atomically inside the barrier.
 * @param filepath The absolute path to the audio file.
 * @param client_ptr Pointer to the active JACK client.
 * @return 0 on success, -1 on format error, -2 if RAM check fails, -3 on corrupt decode.
 */
int load_backing_track(const char* filepath, jack_client_t* client_ptr) {
    if (!client_ptr) return -1;

    float* stolen_pristine = NULL;
    float* stolen_active[MAX_TRACKS] = {NULL};
    UndoLedger stolen_ledger = {0};
    RubberBandState stolen_rb = NULL;

    if (await_rt_thread_detach()) {
        stolen_pristine = pristine_bt_buf;
        pristine_bt_buf = NULL;
        for (int i = 0; i < MAX_TRACKS; i++) {
            stolen_active[i] = master_tracks[i].active_buffer;
            master_tracks[i].active_buffer = NULL;
        }
        stolen_ledger = global_undo_ledger;
        global_undo_ledger.states = NULL;
        global_undo_ledger.max_capacity = 0;
        stolen_rb = rt_rb_state;
        rt_rb_state = NULL;

        size_t old_frames = pristine_frames;
        pristine_frames = 0;
        atomic_store_explicit(&backing_track_frames, 0, memory_order_release);
        resume_rt_thread();

        // CRITICAL PATCH: Synchronously free memory in the background thread.
        // This guarantees RAM is physically unmapped before the new capacity check.
        if (stolen_pristine) free_audio_buffer(stolen_pristine, old_frames);
        for (int i = 0; i < MAX_TRACKS; i++) {
            if (stolen_active[i]) free_audio_buffer(stolen_active[i], old_frames);
        }
        free_detached_ledger_sync(&stolen_ledger, old_frames);
    } else {
        return -1;
    }

    if (stolen_rb) rubberband_delete(stolen_rb);

    SF_INFO sfinfo = {0};
    float* raw_data = NULL;
    float* raw_data2 = NULL;
    sf_count_t frames2 = 0;

    jack_nframes_t jack_rate = jack_get_sample_rate(client_ptr);

    SNDFILE *file = NULL;
    if (!g_str_has_suffix(filepath, ".mp3") && !g_str_has_suffix(filepath, ".MP3")) {
        file = sf_open(filepath, SFM_READ, &sfinfo);
    }

    if (file) {
        if (calculate_safe_history_capacity(sfinfo.frames, sfinfo.samplerate, jack_rate) == 0) {
            sf_close(file);
            return -2;
        }
        raw_data = calloc(sfinfo.frames * sfinfo.channels, sizeof(float));
        if (!raw_data) {
            sf_close(file);
            return -2;
        }
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

    int capacity = calculate_safe_history_capacity(new_pristine_frames, jack_rate, jack_rate);
    if (capacity == 0) {
        if (raw_data) free(raw_data);
        if (raw_data2) free(raw_data2);
        return -2;
    }

    float* new_pristine_buf = alloc_audio_buffer(new_pristine_frames);
    float* src_out = alloc_audio_buffer(new_pristine_frames);
    float* src_out2 = raw_data2 ? alloc_audio_buffer(new_pristine_frames) : NULL;

    if (!new_pristine_buf || !src_out || (raw_data2 && !src_out2)) {
        if (new_pristine_buf) free_audio_buffer(new_pristine_buf, new_pristine_frames);
        if (src_out) free_audio_buffer(src_out, new_pristine_frames);
        if (src_out2) free_audio_buffer(src_out2, new_pristine_frames);
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

    float* temp_active[MAX_TRACKS] = {NULL};
    for (int i = 0; i < MAX_TRACKS; i++) {
        temp_active[i] = alloc_audio_buffer(new_pristine_frames);
        if (!temp_active[i]) {
            for (int j = 0; j <= i; j++) {
                if (temp_active[j]) free_audio_buffer(temp_active[j], new_pristine_frames);
            }
            free_audio_buffer(new_pristine_buf, new_pristine_frames);
            if (src_out) free_audio_buffer(src_out, new_pristine_frames);
            if (src_out2) free_audio_buffer(src_out2, new_pristine_frames);
            if (raw_data) free(raw_data);
            if (raw_data2) free(raw_data2);
            rubberband_delete(new_rb_state);
            return -2;
        }
    }

    UndoState *temp_states = calloc(capacity, sizeof(UndoState));
    if (!temp_states) {
        for (int i = 0; i < MAX_TRACKS; i++) free_audio_buffer(temp_active[i], new_pristine_frames);
        free_audio_buffer(new_pristine_buf, new_pristine_frames);
        if (src_out) free_audio_buffer(src_out, new_pristine_frames);
        if (src_out2) free_audio_buffer(src_out2, new_pristine_frames);
        if (raw_data) free(raw_data);
        if (raw_data2) free(raw_data2);
        rubberband_delete(new_rb_state);
        return -2;
    }

    for (int i = 0; i < capacity; i++) {
        temp_states[i].buffer = alloc_audio_buffer(new_pristine_frames);
        temp_states[i].track_idx = -1;
        temp_states[i].has_audio = false;

        if (!temp_states[i].buffer) {
            for (int j = 0; j <= i; j++) free_audio_buffer(temp_states[j].buffer, new_pristine_frames);
            free(temp_states);
            for (int j = 0; j < MAX_TRACKS; j++) free_audio_buffer(temp_active[j], new_pristine_frames);
            free_audio_buffer(new_pristine_buf, new_pristine_frames);
            if (src_out) free_audio_buffer(src_out, new_pristine_frames);
            if (src_out2) free_audio_buffer(src_out2, new_pristine_frames);
            if (raw_data) free(raw_data);
            if (raw_data2) free(raw_data2);
            rubberband_delete(new_rb_state);
            return -2;
        }
    }

    for (size_t i = 0; i < new_pristine_frames * 2; i++) {
        temp_active[0][i] = src_out[i];
    }

    free_audio_buffer(src_out, new_pristine_frames);
    free(raw_data);

    if (src_out2) {
        for (size_t i = 0; i < new_pristine_frames * 2; i++) {
            temp_active[1][i] = src_out2[i];
        }
        free_audio_buffer(src_out2, new_pristine_frames);
    }
    if (raw_data2) free(raw_data2);

    if (!await_rt_thread_detach()) {
        for (int i = 0; i < capacity; i++) free_audio_buffer(temp_states[i].buffer, new_pristine_frames);
        free(temp_states);
        for (int i = 0; i < MAX_TRACKS; i++) free_audio_buffer(temp_active[i], new_pristine_frames);
        free_audio_buffer(new_pristine_buf, new_pristine_frames);
        rubberband_delete(new_rb_state);
        return -1;
    }

    pristine_bt_buf = new_pristine_buf;
    for (int i = 0; i < MAX_TRACKS; i++) {
        master_tracks[i].active_buffer = temp_active[i];
    }

    global_undo_ledger.states = temp_states;
    global_undo_ledger.max_capacity = capacity;
    global_undo_ledger.head = 0;
    global_undo_ledger.tail = 0;
    global_undo_ledger.count = 0;
    atomic_store_explicit(&global_undo_ledger.is_dirty, false, memory_order_release);

    rt_rb_state = new_rb_state;
    pristine_read_pos = 0;
    atomic_store_explicit(&playback_pos, 0, memory_order_release);
    pristine_frames = new_pristine_frames;
    atomic_store_explicit(&backing_track_frames, new_pristine_frames, memory_order_release);

    atomic_store_explicit(&stretch_queue.read_index, atomic_load_explicit(&stretch_queue.write_index, memory_order_relaxed), memory_order_release);
    atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);

    atomic_store_explicit(&active_track_count, 2, memory_order_release);
    atomic_store_explicit(&current_recording_track, 1, memory_order_release);

    atomic_store_explicit(&master_tracks[0].has_audio, true, memory_order_release);
    if (master_tracks[1].active_buffer && atomic_load_explicit(&master_tracks[1].has_audio, memory_order_acquire) == false) {
        atomic_store_explicit(&master_tracks[1].has_audio, true, memory_order_release);
    }

    resume_rt_thread();

    float current_speed = atomic_load_explicit(&playback_speed, memory_order_relaxed);
    set_playback_speed(current_speed);

    char *base_name = g_path_get_basename(filepath);
    log_and_notify(0, "[IO-DECODE] Base Track Loaded: \"%s\" | %dHz -> %dHz (Ratio=%.3f) | Frames=%zu | Status=OK\n",
           base_name, sfinfo.samplerate, jack_rate, ratio, new_pristine_frames);
    g_free(base_name);

    return 0;
}

/**
 * @brief Initializes a multi-track loop canvas of an exact frame count.
 * @warning CRITICAL: Uses staging pointers to swap memory safely inside the RT barrier.
 * @param exact_frames The exact length of the canvas in frames.
 * @return 0 on success, -2 on memory failure, -1 on timeout.
 */
int init_exact_loop_canvas(size_t exact_frames) {
    if (!client) return -1;
    jack_nframes_t jack_rate = jack_get_sample_rate(client);
    atomic_store_explicit(&active_sample_rate, (int)jack_rate, memory_order_release);

    int capacity = calculate_safe_history_capacity(exact_frames, jack_rate, jack_rate);

    // CRITICAL PATCH: Destructive Teardown Fallback
    // If the new canvas hits the OS memory floor because the old bloated canvas
    // is still holding the RAM, forcefully nuke the old canvas and retry.
    if (capacity == 0) {
        if (!await_rt_thread_detach()) return -1;

        float* stolen_pristine = pristine_bt_buf;
        pristine_bt_buf = NULL;
        float* stolen_active[MAX_TRACKS] = {NULL};
        for (int i = 0; i < MAX_TRACKS; i++) {
            stolen_active[i] = master_tracks[i].active_buffer;
            master_tracks[i].active_buffer = NULL;
            atomic_store_explicit(&master_tracks[i].has_audio, false, memory_order_release);
            atomic_store_explicit(&master_tracks[i].is_soloed, false, memory_order_release);
        }

        UndoLedger stolen_ledger = global_undo_ledger;
        global_undo_ledger.states = NULL;
        global_undo_ledger.max_capacity = 0;
        global_undo_ledger.head = 0;
        global_undo_ledger.tail = 0;
        global_undo_ledger.count = 0;

        size_t old_frames = pristine_frames;
        pristine_frames = 0;
        atomic_store_explicit(&backing_track_frames, 0, memory_order_release);
        atomic_store_explicit(&playback_pos, 0, memory_order_release);

        resume_rt_thread();

        // Synchronously return RAM to OS
        if (stolen_pristine) free_audio_buffer(stolen_pristine, old_frames);
        for (int i = 0; i < MAX_TRACKS; i++) {
            if (stolen_active[i]) free_audio_buffer(stolen_active[i], old_frames);
        }
        free_detached_ledger_sync(&stolen_ledger, old_frames);

        // Retry calculation now that RAM is physically zeroed
        capacity = calculate_safe_history_capacity(exact_frames, jack_rate, jack_rate);
        if (capacity == 0) return -2; // Legitimately out of RAM for this requested duration
    }

    if (exact_frames == pristine_frames && pristine_bt_buf) {
        if (!await_rt_thread_detach()) return -1;
        memset(pristine_bt_buf, 0, exact_frames * 2 * sizeof(float));
        for (int i = 0; i < MAX_TRACKS; i++) {
            if (master_tracks[i].active_buffer) memset(master_tracks[i].active_buffer, 0, exact_frames * 2 * sizeof(float));
            atomic_store_explicit(&master_tracks[i].has_audio, false, memory_order_release);
            atomic_store_explicit(&master_tracks[i].is_soloed, false, memory_order_release);
        }

        global_undo_ledger.head = 0;
        global_undo_ledger.tail = 0;
        global_undo_ledger.count = 0;
        if (global_undo_ledger.states) {
            for (int i = 0; i < global_undo_ledger.max_capacity; i++) {
                global_undo_ledger.states[i].has_audio = false;
                global_undo_ledger.states[i].track_idx = -1;
            }
        }

        if (rt_rb_state) rubberband_reset(rt_rb_state);
        pristine_read_pos = 0;
        atomic_store_explicit(&playback_pos, 0, memory_order_release);
        atomic_store_explicit(&backing_track_frames, exact_frames, memory_order_release);
        atomic_store_explicit(&active_track_count, 1, memory_order_release);
        atomic_store_explicit(&current_recording_track, 0, memory_order_release);
        atomic_store_explicit(&stretch_queue.read_index, atomic_load_explicit(&stretch_queue.write_index, memory_order_relaxed), memory_order_release);
        atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);
        resume_rt_thread();
        return 0;
    }

    float* stolen_pristine = NULL;
    float* stolen_active[MAX_TRACKS] = {NULL};
    UndoLedger stolen_ledger = {0};

    if (!await_rt_thread_detach()) return -1;
    stolen_pristine = pristine_bt_buf;
    pristine_bt_buf = NULL;
    for (int i = 0; i < MAX_TRACKS; i++) {
        stolen_active[i] = master_tracks[i].active_buffer;
        master_tracks[i].active_buffer = NULL;
    }

    stolen_ledger = global_undo_ledger;
    global_undo_ledger.states = NULL;
    global_undo_ledger.max_capacity = 0;

    size_t old_frames = pristine_frames;
    pristine_frames = 0;
    atomic_store_explicit(&backing_track_frames, 0, memory_order_release);
    resume_rt_thread();

    if (stolen_pristine) safe_deferred_locked_free(stolen_pristine, old_frames);
    for (int i = 0; i < MAX_TRACKS; i++) {
        if (stolen_active[i]) safe_deferred_locked_free(stolen_active[i], old_frames);
    }
    free_detached_ledger(&stolen_ledger, old_frames);

    RubberBandState new_rb_state = rt_rb_state;
    if (!new_rb_state) {
        int rb_options = RubberBandOptionProcessRealTime | RubberBandOptionEngineFiner | RubberBandOptionPitchHighQuality | RubberBandOptionPhaseIndependent | RubberBandOptionWindowLong | RubberBandOptionPitchHighConsistency;

        new_rb_state = rubberband_new(jack_rate, 2, rb_options, 1.0, 1.0);
    }

    float *temp_pristine = alloc_audio_buffer(exact_frames);
    if (!temp_pristine) return -2;

    float *temp_active[MAX_TRACKS] = {NULL};
    for (int i = 0; i < MAX_TRACKS; i++) {
        temp_active[i] = alloc_audio_buffer(exact_frames);
        if (!temp_active[i]) {
            free_audio_buffer(temp_pristine, exact_frames);
            for (int j = 0; j <= i; j++) {
                if (temp_active[j]) free_audio_buffer(temp_active[j], exact_frames);
            }
            return -2;
        }
    }

    UndoState *temp_states = calloc(capacity, sizeof(UndoState));
    if (!temp_states) {
        free_audio_buffer(temp_pristine, exact_frames);
        for (int i = 0; i < MAX_TRACKS; i++) free_audio_buffer(temp_active[i], exact_frames);
        return -2;
    }

    for (int i = 0; i < capacity; i++) {
        temp_states[i].buffer = alloc_audio_buffer(exact_frames);
        temp_states[i].track_idx = -1;
        temp_states[i].has_audio = false;

        if (!temp_states[i].buffer) {
            for (int j = 0; j <= i; j++) free_audio_buffer(temp_states[j].buffer, exact_frames);
            free(temp_states);
            free_audio_buffer(temp_pristine, exact_frames);
            for (int j = 0; j < MAX_TRACKS; j++) free_audio_buffer(temp_active[j], exact_frames);
            return -2;
        }
    }

    if (!await_rt_thread_detach()) {
        for (int i = 0; i < capacity; i++) free_audio_buffer(temp_states[i].buffer, exact_frames);
        free(temp_states);
        free_audio_buffer(temp_pristine, exact_frames);
        for (int i = 0; i < MAX_TRACKS; i++) free_audio_buffer(temp_active[i], exact_frames);
        return -1;
    }

    pristine_bt_buf = temp_pristine;
    for (int i = 0; i < MAX_TRACKS; i++) {
        master_tracks[i].active_buffer = temp_active[i];
        atomic_store_explicit(&master_tracks[i].has_audio, false, memory_order_release);
        atomic_store_explicit(&master_tracks[i].is_soloed, false, memory_order_release);
    }

    global_undo_ledger.states = temp_states;
    global_undo_ledger.max_capacity = capacity;
    global_undo_ledger.head = 0;
    global_undo_ledger.tail = 0;
    global_undo_ledger.count = 0;

    rt_rb_state = new_rb_state;
    if (rt_rb_state) rubberband_reset(rt_rb_state);

    pristine_read_pos = 0;
    atomic_store_explicit(&playback_pos, 0, memory_order_release);
    pristine_frames = exact_frames;
    atomic_store_explicit(&backing_track_frames, exact_frames, memory_order_release);
    atomic_store_explicit(&active_track_count, 1, memory_order_release);
    atomic_store_explicit(&current_recording_track, 0, memory_order_release);

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
    if (!client || duration_seconds <= 0) return -1;

    jack_nframes_t jack_rate = jack_get_sample_rate(client);

    // Explicitly cast to size_t prior to multiplication to guarantee 64-bit arithmetic
    return init_exact_loop_canvas((size_t)jack_rate * (size_t)duration_seconds);
}

/**
 * @brief Non-destructively resizes the active recording canvas.
 * @warning CRITICAL: Relies on external buffer allocation to safely copy memory during the RT barrier lock.
 * @param duration_seconds The new length of the canvas in seconds.
 * @return 0 on success, -1 on timeout, -2 on memory failure.
 */
int resize_loop_canvas_seconds(int duration_seconds) {
    if (!client) return -1;
    jack_nframes_t jack_rate = atomic_load_explicit(&active_sample_rate, memory_order_acquire);
    if (jack_rate <= 0) jack_rate = 48000;

    size_t new_exact_frames = (size_t)jack_rate * duration_seconds;

    // ALLOW SHRINKING: Only abort if the canvas is already the exact requested size
    if (new_exact_frames == pristine_frames) return 0;

    int new_capacity = calculate_safe_history_capacity(new_exact_frames, jack_rate, jack_rate);
    if (new_capacity == 0) return -2;

    // CLAMP COPY SIZE: Prevents out-of-bounds reads when shrinking the canvas
    size_t frames_to_copy = (new_exact_frames < pristine_frames) ? new_exact_frames : pristine_frames;

    float *new_active[MAX_TRACKS] = {NULL};
    for (int i = 0; i < MAX_TRACKS; i++) {
        new_active[i] = alloc_audio_buffer(new_exact_frames);
        if (!new_active[i]) {
            for (int j = 0; j < i; j++) free_audio_buffer(new_active[j], new_exact_frames);
            return -2;
        }
    }

    UndoState *new_states = calloc(new_capacity, sizeof(UndoState));
    if (!new_states) {
        for (int i = 0; i < MAX_TRACKS; i++) free_audio_buffer(new_active[i], new_exact_frames);
        return -2;
    }

    for (int i = 0; i < new_capacity; i++) {
        new_states[i].buffer = alloc_audio_buffer(new_exact_frames);
        new_states[i].track_idx = -1;
        new_states[i].has_audio = false;
        if (!new_states[i].buffer) {
            for (int j = 0; j <= i; j++) free_audio_buffer(new_states[j].buffer, new_exact_frames);
            free(new_states);
            for (int j = 0; j < MAX_TRACKS; j++) free_audio_buffer(new_active[j], new_exact_frames);
            return -2;
        }
    }

    float *new_pristine = alloc_audio_buffer(new_exact_frames);
    if (!new_pristine) {
        for (int i = 0; i < new_capacity; i++) free_audio_buffer(new_states[i].buffer, new_exact_frames);
        free(new_states);
        for (int i = 0; i < MAX_TRACKS; i++) free_audio_buffer(new_active[i], new_exact_frames);
        return -2;
    }

    int target_count = global_undo_ledger.count;
    int shrink_diff = target_count - new_capacity;
    if (shrink_diff > 0) target_count = new_capacity;

    int new_head = global_undo_ledger.head - (shrink_diff > 0 ? shrink_diff : 0);
    if (new_head < 0) new_head = 0;

    if (!await_rt_thread_detach()) {
        for (int i = 0; i < new_capacity; i++) free_audio_buffer(new_states[i].buffer, new_exact_frames);
        free(new_states);
        for (int i = 0; i < MAX_TRACKS; i++) free_audio_buffer(new_active[i], new_exact_frames);
        free_audio_buffer(new_pristine, new_exact_frames);
        return -1;
    }

    UndoLedger old_ledger = global_undo_ledger;

    for (int i = 0; i < target_count; i++) {
        int old_idx = (global_undo_ledger.tail + (shrink_diff > 0 ? shrink_diff : 0) + i) % global_undo_ledger.max_capacity;
        if (global_undo_ledger.states[old_idx].buffer) {
            memcpy(new_states[i].buffer, global_undo_ledger.states[old_idx].buffer, frames_to_copy * 2 * sizeof(float));
        }
        new_states[i].track_idx = global_undo_ledger.states[old_idx].track_idx;
        new_states[i].has_audio = global_undo_ledger.states[old_idx].has_audio;
        new_states[i].transaction_id = global_undo_ledger.states[old_idx].transaction_id;
        new_states[i].canvas_frames = global_undo_ledger.states[old_idx].canvas_frames;
        new_states[i].v_start = global_undo_ledger.states[old_idx].v_start;
        new_states[i].v_end = global_undo_ledger.states[old_idx].v_end;
        new_states[i].gain = global_undo_ledger.states[old_idx].gain;
        new_states[i].is_soloed = global_undo_ledger.states[old_idx].is_soloed;
        strncpy(new_states[i].name, global_undo_ledger.states[old_idx].name, 64);
    }

    global_undo_ledger.states = new_states;
    global_undo_ledger.max_capacity = new_capacity;
    global_undo_ledger.head = new_head;
    global_undo_ledger.tail = 0;
    global_undo_ledger.count = target_count;

    for (int i = 0; i < MAX_TRACKS; i++) {
        if (master_tracks[i].active_buffer) {
            memcpy(new_active[i], master_tracks[i].active_buffer, frames_to_copy * 2 * sizeof(float));
            safe_deferred_locked_free(master_tracks[i].active_buffer, frames_to_copy);
        }
        master_tracks[i].active_buffer = new_active[i];
    }

    if (pristine_bt_buf) {
        memcpy(new_pristine, pristine_bt_buf, frames_to_copy * 2 * sizeof(float));
        safe_deferred_locked_free(pristine_bt_buf, frames_to_copy);
    }
    pristine_bt_buf = new_pristine;

    pristine_frames = new_exact_frames;
    atomic_store_explicit(&backing_track_frames, new_exact_frames, memory_order_release);

    size_t current_pos = atomic_load_explicit(&playback_pos, memory_order_acquire);
    if (current_pos > new_exact_frames) {
        atomic_store_explicit(&playback_pos, new_exact_frames, memory_order_release);
    }

    atomic_store_explicit(&stretch_queue.read_index, atomic_load_explicit(&stretch_queue.write_index, memory_order_relaxed), memory_order_release);
    atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);

    resume_rt_thread();
    free_detached_ledger(&old_ledger, frames_to_copy);

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
        atomic_store_explicit(&master_tracks[track_idx].gain, multiplier, memory_order_relaxed);
        mark_session_dirty();
    }
}

void delete_multitrack_layer(int track_idx) {
    if (track_idx < 0 || track_idx >= MAX_TRACKS) return;

    bool has_audio = atomic_load_explicit(&master_tracks[track_idx].has_audio, memory_order_acquire);

    // FAST-PATH: If the track is already empty, just reset metadata and bump focus up.
    // Bypasses the Undo ledger and RT thread lock entirely.
    if (!has_audio) {
        atomic_store_explicit(&master_tracks[track_idx].gain, 1.0f, memory_order_release);
        atomic_store_explicit(&master_tracks[track_idx].is_soloed, false, memory_order_release);

        char default_name[64];
        snprintf(default_name, sizeof(default_name), "Track %d", track_idx + 1);
        extern void set_multitrack_track_name(int, const char*);
        set_multitrack_track_name(track_idx, default_name);

        int new_focus = (track_idx > 0) ? track_idx - 1 : 0;
        atomic_store_explicit(&current_recording_track, new_focus, memory_order_release);
        atomic_store_explicit(&selected_tracks_mask, (1 << new_focus), memory_order_release);
        return;
    }

    if (!await_rt_thread_detach()) return;

    current_transaction_id++;

    // 1. Backup only the single targeted track
    push_undo_state(track_idx, current_transaction_id);

    // ... [Rest of the existing deletion logic follows below] ...

    // 2. Wipe the audio buffer in-place
    if (master_tracks[track_idx].active_buffer) {
        memset(master_tracks[track_idx].active_buffer, 0, pristine_frames * 2 * sizeof(float));
    }

    // 3. Reset metadata to defaults
    atomic_store_explicit(&master_tracks[track_idx].gain, 1.0f, memory_order_release);
    atomic_store_explicit(&master_tracks[track_idx].is_soloed, false, memory_order_release);
    atomic_store_explicit(&master_tracks[track_idx].has_audio, false, memory_order_release);

    char default_name[64];
    snprintf(default_name, sizeof(default_name), "Track %d", track_idx + 1);
    extern void set_multitrack_track_name(int, const char*);
    set_multitrack_track_name(track_idx, default_name);

    // 4. Safely flush the time-stretcher if the timebase track was wiped
    int current_timebase = atomic_load_explicit(&timebase_track, memory_order_acquire);
    if (current_timebase == track_idx && pristine_bt_buf && master_tracks[track_idx].active_buffer) {
        memcpy(pristine_bt_buf, master_tracks[track_idx].active_buffer, pristine_frames * 2 * sizeof(float));
        atomic_store_explicit(&stretch_queue.read_index, atomic_load_explicit(&stretch_queue.write_index, memory_order_relaxed), memory_order_release);
        atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);
    }

    // 5. Shift UI focus UP by one row (bounded at 0)
    int new_focus = (track_idx > 0) ? track_idx - 1 : 0;
    atomic_store_explicit(&current_recording_track, new_focus, memory_order_release);
    atomic_store_explicit(&selected_tracks_mask, (1 << new_focus), memory_order_release);

    // 6. Recalculate the absolute ceiling for the engine loop
    int highest_active = 0;
    for (int i = MAX_TRACKS - 1; i >= 0; i--) {
        if (atomic_load_explicit(&master_tracks[i].has_audio, memory_order_acquire)) {
            highest_active = i;
            break;
        }
    }

    int new_active_count = highest_active + 1;
    if (new_focus >= new_active_count) new_active_count = new_focus + 1;
    atomic_store_explicit(&active_track_count, new_active_count, memory_order_release);

    resume_rt_thread();
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
    mark_session_dirty();
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
        mark_session_dirty();
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
    mark_session_dirty();
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
        if (master_tracks[current].active_buffer && pristine_frames > 0) {
            memset(master_tracks[current].active_buffer, 0, pristine_frames * 2 * sizeof(float));
        }
        atomic_store_explicit(&current_recording_track, current - 1, memory_order_release);
        atomic_store_explicit(&active_track_count, current, memory_order_release);
    } else if (current == 1) {
        if (master_tracks[current].active_buffer && pristine_frames > 0) {
            memset(master_tracks[current].active_buffer, 0, pristine_frames * 2 * sizeof(float));
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

    // SAFETY [Low]: Initialize POSIX non-blocking IPC pipe for GTK event loop sync.
    if (event_pipe[0] == -1) {
        if (pipe(event_pipe) == 0) {
            fcntl(event_pipe[0], F_SETFL, fcntl(event_pipe[0], F_GETFL, 0) | O_NONBLOCK);
            fcntl(event_pipe[1], F_SETFL, fcntl(event_pipe[1], F_GETFL, 0) | O_NONBLOCK);
        }
    }

    // Immediately query the active hardware sample rate and register the dynamic callback
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
        log_and_notify(0, "Cannot activate JACK client\n");
        return 1;
    }

    log_and_notify(0, "Audio Engine Live: CachyOS RT thread active.\n");

    patch_playback_ports(target_playback_device);
    return patch_audio_ports(target_audio_device);
}

/**
 * @brief Safely reboots the JACK client mid-session without destroying memory canvases or lock-free queues.
 * @return 0 on success, -1 on failure.
 */
int reboot_audio_engine(const char* target_audio_device, const char* target_playback_device, float input_gain_multiplier) {
    if (client) {
        jack_deactivate(client);
        jack_client_close(client);
        client = NULL;
    }

    jack_status_t status;
    client = jack_client_open("QJams_Engine", JackNullOption, &status);
    if (!client) return -1;

    input_gain = input_gain_multiplier;
    jack_nframes_t current_rate = jack_get_sample_rate(client);
    atomic_store_explicit(&active_sample_rate, (int)current_rate, memory_order_release);

    jack_set_sample_rate_callback(client, jack_sample_rate_cb, 0);
    jack_set_process_callback(client, process_audio, 0);
    jack_on_shutdown(client, jack_shutdown, 0);
    jack_set_port_registration_callback(client, jack_port_registration_cb, 0);

    input_port_1 = jack_port_register(client, "capture_1", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    input_port_2 = jack_port_register(client, "capture_2", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    output_port_1 = jack_port_register(client, "playback_1", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    output_port_2 = jack_port_register(client, "playback_2", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);

    if (jack_activate(client)) {
        log_and_notify(0, "[HW-REBOOT] Cannot activate JACK client on reboot\n");
        return -1;
    }

    log_and_notify(0, "[HW-REBOOT] Audio Engine Rebooted: CachyOS RT thread active.\n");
    patch_playback_ports(target_playback_device);
    return patch_audio_ports(target_audio_device);
}

/**
 * @brief Closes the JACK client and frees all allocated DSP buffers, queues, and states.
 * @return void
 */
void shutdown_audio_engine(void) {
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
    if (pristine_bt_buf) free_audio_buffer(pristine_bt_buf, pristine_frames);

    for (int i = 0; i < MAX_TRACKS; i++) {
        if (master_tracks[i].active_buffer) {
            free_audio_buffer(master_tracks[i].active_buffer, pristine_frames);
            master_tracks[i].active_buffer = NULL;
        }
        if (clipboard_buffers[i]) { free(clipboard_buffers[i]); clipboard_buffers[i] = NULL; }
    }

    free_detached_ledger_sync(&global_undo_ledger, pristine_frames);
    global_undo_ledger.states = NULL;
    global_undo_ledger.max_capacity = 0;

    clipboard_frames = 0;

    atomic_store_explicit(&vu_peak_input_l, 0.0f, memory_order_relaxed);
    atomic_store_explicit(&vu_peak_input_r, 0.0f, memory_order_relaxed);

    log_and_notify(0, "Audio engine cleanly shut down.\n");
}

void swap_multitrack_tracks(int idx_a, int idx_b) {
    if (idx_a >= 0 && idx_b >= 0 && idx_a < MAX_TRACKS && idx_b < MAX_TRACKS) {
        if (!await_rt_thread_detach()) return;

        // Swap complete structures safely inside the barrier
        QJTrackBuffer tmp = master_tracks[idx_a];
        master_tracks[idx_a] = master_tracks[idx_b];
        master_tracks[idx_b] = tmp;

        int current_timebase = atomic_load_explicit(&timebase_track, memory_order_acquire);
        if (current_timebase == idx_a) {
            atomic_store_explicit(&timebase_track, idx_b, memory_order_release);
            current_timebase = idx_b;
        } else if (current_timebase == idx_b) {
            atomic_store_explicit(&timebase_track, idx_a, memory_order_release);
            current_timebase = idx_a;
        }

        // Synchronize the time-stretcher buffer if the timebase was moved
        if ((idx_a == current_timebase || idx_b == current_timebase) && pristine_bt_buf && pristine_frames > 0) {
            memcpy(pristine_bt_buf, master_tracks[current_timebase].active_buffer, pristine_frames * 2 * sizeof(float));
            atomic_store_explicit(&stretch_queue.read_index, atomic_load_explicit(&stretch_queue.write_index, memory_order_relaxed), memory_order_release);
            atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);
        }

        mark_session_dirty();
        resume_rt_thread();
    }
}

/**
 * @brief Clears a layer to absolute silence. Restoring is natively handled via Ctrl+Z ledger pop.
 * @param track_idx The target layer index.
 * @return 0 if cleared, 1 if ignored, -1 on error.
 */
int clear_or_restore_track(int track_idx) {
    if (track_idx < 0 || track_idx >= MAX_TRACKS || !master_tracks[track_idx].active_buffer) return -1;

    size_t total_samples = pristine_frames * 2;
    bool has_audio = atomic_load_explicit(&master_tracks[track_idx].has_audio, memory_order_acquire);

    if (!has_audio) return 1;

    if (!await_rt_thread_detach()) return -1;

    current_transaction_id++;
    push_undo_state(track_idx, current_transaction_id);

    int active = atomic_load_explicit(&active_track_count, memory_order_acquire);
    if (track_idx >= active && track_idx < MAX_TRACKS) {
        atomic_store_explicit(&active_track_count, track_idx + 1, memory_order_release);
    }

    memset(master_tracks[track_idx].active_buffer, 0, total_samples * sizeof(float));

    atomic_store_explicit(&master_tracks[track_idx].has_audio, false, memory_order_release);
    atomic_store_explicit(&master_tracks[track_idx].is_soloed, false, memory_order_release);

    if (track_idx == 0 && pristine_bt_buf) {
        memcpy(pristine_bt_buf, master_tracks[0].active_buffer, total_samples * sizeof(float));
        atomic_store_explicit(&stretch_queue.read_index, atomic_load_explicit(&stretch_queue.write_index, memory_order_relaxed), memory_order_release);
        atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);
    }

    resume_rt_thread();
    return 0;
}

void clear_multitrack_track(int track_idx) {
    if (track_idx < 0 || track_idx >= MAX_TRACKS || !master_tracks[track_idx].active_buffer) return;

    if (!await_rt_thread_detach()) return;

    current_transaction_id++;
    push_undo_state(track_idx, current_transaction_id);

    memset(master_tracks[track_idx].active_buffer, 0, pristine_frames * 2 * sizeof(float));
    atomic_store_explicit(&master_tracks[track_idx].has_audio, false, memory_order_release);

    resume_rt_thread();
}

void backup_track_to_ledger(int track_idx) {
    if (track_idx >= 0 && track_idx < MAX_TRACKS) {
        current_transaction_id++;
        push_undo_state(track_idx, current_transaction_id);
    }
}

// ==============================================================================
// TRUE MULTI-SELECT NLE CORE (Unified DSP)
// ==============================================================================

/**
 * @brief Pushes a track's current state to the chronological ledger.
 * @warning CRITICAL: Must be called within await_rt_thread_detach() barrier to prevent use-after-free faults.
 * @param track_idx The multitrack layer to backup.
 * @param trans_id The transaction group ID.
 */
static void push_undo_state(int track_idx, uint32_t trans_id) {
    if (global_undo_ledger.max_capacity <= 0 || !global_undo_ledger.states) return;

    mark_session_dirty();

    if (global_undo_ledger.count > global_undo_ledger.head) {
        global_undo_ledger.count = global_undo_ledger.head;
    }

    if (global_undo_ledger.count == global_undo_ledger.max_capacity) {
        uint32_t drop_id = global_undo_ledger.states[global_undo_ledger.tail].transaction_id;
        while (global_undo_ledger.count > 0 && global_undo_ledger.states[global_undo_ledger.tail].transaction_id == drop_id) {
            global_undo_ledger.tail = (global_undo_ledger.tail + 1) % global_undo_ledger.max_capacity;
            global_undo_ledger.count--;
        }
    }

    int target_idx = (global_undo_ledger.tail + global_undo_ledger.count) % global_undo_ledger.max_capacity;

    UndoState *state = &global_undo_ledger.states[target_idx];
    if (state->buffer && master_tracks[track_idx].active_buffer) {
        memcpy(state->buffer, master_tracks[track_idx].active_buffer, pristine_frames * 2 * sizeof(float));
    }
    state->track_idx = track_idx;
    state->has_audio = atomic_load_explicit(&master_tracks[track_idx].has_audio, memory_order_acquire);
    state->transaction_id = trans_id;
    state->canvas_frames = atomic_load_explicit(&backing_track_frames, memory_order_acquire);

    state->gain = atomic_load_explicit(&master_tracks[track_idx].gain, memory_order_acquire);
    state->is_soloed = atomic_load_explicit(&master_tracks[track_idx].is_soloed, memory_order_acquire);
    extern void get_multitrack_track_name(int, char*);
    get_multitrack_track_name(track_idx, state->name);

    extern size_t video_start_frame;
    extern size_t video_end_frame;
    state->v_start = video_start_frame;
    state->v_end = video_end_frame;

    global_undo_ledger.count++;
    global_undo_ledger.head = global_undo_ledger.count;
}

static inline void internal_backup_masked_tracks(uint32_t mask) {
    current_transaction_id++;
    for (int i = 0; i < MAX_TRACKS; i++) {
        if ((mask & (1 << i)) && master_tracks[i].active_buffer) {
            push_undo_state(i, current_transaction_id);
        }
    }
}

static inline void apply_nle_cut_math(int track_idx, size_t start, size_t end) {
    size_t cut_len = end - start;
    size_t remaining = pristine_frames - end;
    int rate = atomic_load_explicit(&active_sample_rate, memory_order_acquire);
    size_t fade_len = (size_t)((rate > 0 ? rate : 48000) * 0.005);
    if (fade_len > cut_len) fade_len = cut_len;

    if (start >= fade_len) {
        for (size_t i = 0; i < fade_len; i++) {
            float mult = (float)(fade_len - i) / (float)fade_len;
            master_tracks[track_idx].active_buffer[(start - fade_len + i) * 2] *= mult;
            master_tracks[track_idx].active_buffer[(start - fade_len + i) * 2 + 1] *= mult;
        }
    }

    if (remaining > 0) {
        size_t apply_fade = (remaining < fade_len) ? remaining : fade_len;
        for (size_t i = 0; i < apply_fade; i++) {
            float mult = (float)i / (float)apply_fade;
            master_tracks[track_idx].active_buffer[(end + i) * 2] *= mult;
            master_tracks[track_idx].active_buffer[(end + i) * 2 + 1] *= mult;
        }
        memmove(&master_tracks[track_idx].active_buffer[start * 2], &master_tracks[track_idx].active_buffer[end * 2], remaining * 2 * sizeof(float));
    }
    memset(&master_tracks[track_idx].active_buffer[(pristine_frames - cut_len) * 2], 0, cut_len * 2 * sizeof(float));
}

static inline void apply_nle_clear_math(int track_idx, size_t start, size_t end) {
    size_t clear_len = end - start;
    int rate = atomic_load_explicit(&active_sample_rate, memory_order_acquire);
    size_t fade_len = (size_t)((rate > 0 ? rate : 48000) * 0.005);
    if (fade_len > clear_len / 2) fade_len = clear_len / 2;

    if (start >= fade_len) {
        for (size_t i = 0; i < fade_len; i++) {
            float mult = (float)(fade_len - i) / (float)fade_len;
            master_tracks[track_idx].active_buffer[(start - fade_len + i) * 2] *= mult;
            master_tracks[track_idx].active_buffer[(start - fade_len + i) * 2 + 1] *= mult;
        }
    }
    size_t remaining = pristine_frames - end;
    if (remaining >= fade_len) {
        for (size_t i = 0; i < fade_len; i++) {
            float mult = (float)i / (float)fade_len;
            master_tracks[track_idx].active_buffer[(end + i) * 2] *= mult;
            master_tracks[track_idx].active_buffer[(end + i) * 2 + 1] *= mult;
        }
    }
    memset(&master_tracks[track_idx].active_buffer[start * 2], 0, clear_len * 2 * sizeof(float));
}

void copy_selection(void) {
    if (!atomic_load_explicit(&loop_active, memory_order_acquire)) return;
    size_t start = atomic_load_explicit(&loop_start_frame, memory_order_acquire);
    size_t end = atomic_load_explicit(&loop_end_frame, memory_order_acquire);
    if (start >= end || end > pristine_frames) return;

    size_t copy_len = end - start;
    if (!await_rt_thread_detach()) return;

    if (end > pristine_frames) {
        resume_rt_thread();
        return;
    }

    uint32_t mask = atomic_load_explicit(&selected_tracks_mask, memory_order_acquire);
    bool mem_success = true;

    for (int i = 0; i < MAX_TRACKS; i++) {
        if (mask & (1 << i)) {
            if (master_tracks[i].active_buffer) {
                float *new_buf = realloc(clipboard_buffers[i], copy_len * 2 * sizeof(float));
                if (new_buf) {
                    clipboard_buffers[i] = new_buf;
                    memcpy(clipboard_buffers[i], &master_tracks[i].active_buffer[start * 2], copy_len * 2 * sizeof(float));
                } else {
                    mem_success = false;
                }
            }
        } else {
            if (clipboard_buffers[i]) { free(clipboard_buffers[i]); clipboard_buffers[i] = NULL; }
        }
    }

    if (mem_success) {
        clipboard_frames = copy_len;
    } else {
        for (int i = 0; i < MAX_TRACKS; i++) {
            if (clipboard_buffers[i]) { free(clipboard_buffers[i]); clipboard_buffers[i] = NULL; }
        }
        clipboard_frames = 0;
        log_and_notify(1, "<span foreground='#ff4444'><b>Status: Insufficient RAM to copy selection</b></span>");
        log_and_notify(0, "[MEM] Critical: Clipboard memory allocation failed. Selection cleared.\n");
    }
    resume_rt_thread();
    clear_loop_points();
}

void cut_selection(void) {
    if (!atomic_load_explicit(&loop_active, memory_order_acquire)) return;
    size_t start = atomic_load_explicit(&loop_start_frame, memory_order_acquire);
    size_t end = atomic_load_explicit(&loop_end_frame, memory_order_acquire);
    if (start >= end || end > pristine_frames) return;

    if (!await_rt_thread_detach()) return;

    if (end > pristine_frames) {
        resume_rt_thread();
        return;
    }

    int current_timebase = atomic_load_explicit(&timebase_track, memory_order_acquire);
    uint32_t mask = atomic_load_explicit(&selected_tracks_mask, memory_order_acquire);
    internal_backup_masked_tracks(mask);

    size_t copy_len = end - start;
    bool mem_success = true;

    for (int i = 0; i < MAX_TRACKS; i++) {
        if ((mask & (1 << i)) && master_tracks[i].active_buffer) {
            float *new_buf = realloc(clipboard_buffers[i], copy_len * 2 * sizeof(float));
            if (new_buf) {
                clipboard_buffers[i] = new_buf;
                memcpy(clipboard_buffers[i], &master_tracks[i].active_buffer[start * 2], copy_len * 2 * sizeof(float));
            } else {
                mem_success = false;
            }
            apply_nle_cut_math(i, start, end);
        } else {
            if (clipboard_buffers[i]) { free(clipboard_buffers[i]); clipboard_buffers[i] = NULL; }
        }
    }

    if (mem_success) {
        clipboard_frames = copy_len;
    } else {
        for (int i = 0; i < MAX_TRACKS; i++) {
            if (clipboard_buffers[i]) { free(clipboard_buffers[i]); clipboard_buffers[i] = NULL; }
        }
        clipboard_frames = 0;
        log_and_notify(1, "<span foreground='#ff4444'><b>Status: Insufficient RAM to cut selection</b></span>");
        log_and_notify(0, "[MEM] Critical: Clipboard memory allocation failed. Cut acts as delete.\n");
    }

    if ((mask & (1 << current_timebase)) && pristine_bt_buf && master_tracks[current_timebase].active_buffer) {
        memcpy(pristine_bt_buf, master_tracks[current_timebase].active_buffer, pristine_frames * 2 * sizeof(float));
        atomic_store_explicit(&stretch_queue.read_index, atomic_load_explicit(&stretch_queue.write_index, memory_order_relaxed), memory_order_release);
        atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);
    }
    atomic_store_explicit(&playback_pos, start, memory_order_release);
    resume_rt_thread();
    clear_loop_points();
}

void delete_selection(void) {
    if (!atomic_load_explicit(&loop_active, memory_order_acquire)) return;
    size_t start = atomic_load_explicit(&loop_start_frame, memory_order_acquire);
    size_t end = atomic_load_explicit(&loop_end_frame, memory_order_acquire);
    if (start >= end || end > pristine_frames) return;

    if (!await_rt_thread_detach()) return;

    if (end > pristine_frames) {
        resume_rt_thread();
        return;
    }

    int current_timebase = atomic_load_explicit(&timebase_track, memory_order_acquire);
    uint32_t mask = atomic_load_explicit(&selected_tracks_mask, memory_order_acquire);
    internal_backup_masked_tracks(mask);

    for (int i = 0; i < MAX_TRACKS; i++) {
        if ((mask & (1 << i)) && master_tracks[i].active_buffer) {
            apply_nle_clear_math(i, start, end);
        }
    }

    if ((mask & (1 << current_timebase)) && pristine_bt_buf && master_tracks[current_timebase].active_buffer) {
        memcpy(pristine_bt_buf, master_tracks[current_timebase].active_buffer, pristine_frames * 2 * sizeof(float));
        atomic_store_explicit(&stretch_queue.read_index, atomic_load_explicit(&stretch_queue.write_index, memory_order_relaxed), memory_order_release);
        atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);
    }

    resume_rt_thread();
    clear_loop_points();
}

void global_ripple_delete_selection(void) {
    if (!atomic_load_explicit(&loop_active, memory_order_acquire)) return;
    size_t start = atomic_load_explicit(&loop_start_frame, memory_order_acquire);
    size_t end = atomic_load_explicit(&loop_end_frame, memory_order_acquire);
    if (start >= end || end > pristine_frames) return;

    if (!await_rt_thread_detach()) return;

    if (end > pristine_frames) {
        resume_rt_thread();
        return;
    }

    int current_timebase = atomic_load_explicit(&timebase_track, memory_order_acquire);
    uint32_t mask = 0xFFFFFFFF;
    internal_backup_masked_tracks(mask);

    for (int i = 0; i < MAX_TRACKS; i++) {
        if (master_tracks[i].active_buffer) {
            apply_nle_cut_math(i, start, end);
        }
    }

    if (pristine_bt_buf && master_tracks[current_timebase].active_buffer) {
        memcpy(pristine_bt_buf, master_tracks[current_timebase].active_buffer, pristine_frames * 2 * sizeof(float));
        atomic_store_explicit(&stretch_queue.read_index, atomic_load_explicit(&stretch_queue.write_index, memory_order_relaxed), memory_order_release);
        atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);
    }
    atomic_store_explicit(&playback_pos, start, memory_order_release);
    resume_rt_thread();
    clear_loop_points();
}

void paste_selection(void) {
    if (clipboard_frames == 0) return;
    size_t current_pos = atomic_load_explicit(&playback_pos, memory_order_acquire);

    if (current_pos + clipboard_frames > pristine_frames) {
        int rate = atomic_load_explicit(&active_sample_rate, memory_order_acquire);
        int required_seconds = (int)((current_pos + clipboard_frames) / (rate > 0 ? rate : 48000)) + 1;
        int resize_status = resize_loop_canvas_seconds(required_seconds);
        if (resize_status != 0) {
            if (resize_status == -2) {
                log_and_notify(1, "<span foreground='#ff4444'><b>Status: Insufficient RAM to expand canvas for paste</b></span>");
            }
            return;
        }
    }

    if (!await_rt_thread_detach()) return;

    if (current_pos + clipboard_frames > pristine_frames) {
        resume_rt_thread();
        return;
    }

    int current_timebase = atomic_load_explicit(&timebase_track, memory_order_acquire);
    uint32_t mask = atomic_load_explicit(&selected_tracks_mask, memory_order_acquire);
    internal_backup_masked_tracks(mask);

    bool bt_updated = false;
    for (int i = 0; i < MAX_TRACKS; i++) {
        if ((mask & (1 << i)) && clipboard_buffers[i] && master_tracks[i].active_buffer) {
            memcpy(&master_tracks[i].active_buffer[current_pos * 2], clipboard_buffers[i], clipboard_frames * 2 * sizeof(float));
            atomic_store_explicit(&master_tracks[i].has_audio, true, memory_order_release);
            if (i == current_timebase) bt_updated = true;

            int active = atomic_load_explicit(&active_track_count, memory_order_acquire);
            if (i >= active) atomic_store_explicit(&active_track_count, i + 1, memory_order_release);
        }
    }

    if (bt_updated && pristine_bt_buf && master_tracks[current_timebase].active_buffer) {
        memcpy(pristine_bt_buf, master_tracks[current_timebase].active_buffer, pristine_frames * 2 * sizeof(float));
        atomic_store_explicit(&stretch_queue.read_index, atomic_load_explicit(&stretch_queue.write_index, memory_order_relaxed), memory_order_release);
        atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);
    }

    evaluate_session_dirty_state(); // Check if we undid our way back to the save point
    resume_rt_thread();
}

void global_paste_selection(void) {
    if (clipboard_frames == 0) return;
    size_t current_pos = atomic_load_explicit(&playback_pos, memory_order_acquire);

    if (current_pos + clipboard_frames > pristine_frames) {
        int rate = atomic_load_explicit(&active_sample_rate, memory_order_acquire);
        int required_seconds = (int)((current_pos + clipboard_frames) / (rate > 0 ? rate : 48000)) + 1;
        int resize_status = resize_loop_canvas_seconds(required_seconds);
        if (resize_status != 0) {
            if (resize_status == -2) {
                log_and_notify(1, "<span foreground='#ff4444'><b>Status: Insufficient RAM to expand canvas for paste</b></span>");
            }
            return;
        }
    }

    if (!await_rt_thread_detach()) return;

    if (current_pos + clipboard_frames > pristine_frames) {
        resume_rt_thread();
        return;
    }

    int current_timebase = atomic_load_explicit(&timebase_track, memory_order_acquire);
    uint32_t mask = 0xFFFFFFFF;
    internal_backup_masked_tracks(mask);

    bool bt_updated = false;
    for (int i = 0; i < MAX_TRACKS; i++) {
        if (clipboard_buffers[i] && master_tracks[i].active_buffer) {
            memcpy(&master_tracks[i].active_buffer[current_pos * 2], clipboard_buffers[i], clipboard_frames * 2 * sizeof(float));
            atomic_store_explicit(&master_tracks[i].has_audio, true, memory_order_release);
            if (i == current_timebase) bt_updated = true;

            int active = atomic_load_explicit(&active_track_count, memory_order_acquire);
            if (i >= active) atomic_store_explicit(&active_track_count, i + 1, memory_order_release);
        }
    }

    if (bt_updated && pristine_bt_buf && master_tracks[current_timebase].active_buffer) {
        memcpy(pristine_bt_buf, master_tracks[current_timebase].active_buffer, pristine_frames * 2 * sizeof(float));
        atomic_store_explicit(&stretch_queue.read_index, atomic_load_explicit(&stretch_queue.write_index, memory_order_relaxed), memory_order_release);
        atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);
    }

    atomic_store_explicit(&playback_pos, current_pos + clipboard_frames, memory_order_release);
    evaluate_session_dirty_state();
    resume_rt_thread();
}

void apply_smart_fade_selection(void) {
    if (!atomic_load_explicit(&loop_active, memory_order_acquire)) return;
    size_t start = atomic_load_explicit(&loop_start_frame, memory_order_acquire);
    size_t end = atomic_load_explicit(&loop_end_frame, memory_order_acquire);
    if (start >= end || end > pristine_frames) return;

    size_t sel_len = end - start;
    if (sel_len < 4) { clear_loop_points(); return; }

    if (!await_rt_thread_detach()) return;

    if (end > pristine_frames) {
        resume_rt_thread();
        return;
    }

    int current_timebase = atomic_load_explicit(&timebase_track, memory_order_acquire);
    uint32_t mask = atomic_load_explicit(&selected_tracks_mask, memory_order_acquire);
    internal_backup_masked_tracks(mask);

    for (int i = 0; i < MAX_TRACKS; i++) {
        if ((mask & (1 << i)) && master_tracks[i].active_buffer) {
            bool touches_start = (start == 0 || master_tracks[i].active_buffer[(start - 1) * 2] == 0.0f);
            bool touches_end = (end >= pristine_frames || master_tracks[i].active_buffer[end * 2] == 0.0f);

            if (touches_start && !touches_end) {
                for (size_t j = 0; j < sel_len; j++) {
                    float mult = (float)j / (float)(sel_len - 1);
                    master_tracks[i].active_buffer[(start + j) * 2] *= mult;
                    master_tracks[i].active_buffer[(start + j) * 2 + 1] *= mult;
                }
            } else if (touches_end && !touches_start) {
                for (size_t j = 0; j < sel_len; j++) {
                    float mult = 1.0f - ((float)j / (float)(sel_len - 1));
                    master_tracks[i].active_buffer[(start + j) * 2] *= mult;
                    master_tracks[i].active_buffer[(start + j) * 2 + 1] *= mult;
                }
            } else {
                size_t half_len = sel_len / 2;
                for (size_t j = 0; j < half_len; j++) {
                    float mult = 1.0f - ((float)j / (float)half_len);
                    master_tracks[i].active_buffer[(start + j) * 2] *= mult;
                    master_tracks[i].active_buffer[(start + j) * 2 + 1] *= mult;
                }
                master_tracks[i].active_buffer[(start + half_len) * 2] = 0.0f;
                master_tracks[i].active_buffer[(start + half_len) * 2 + 1] = 0.0f;
                for (size_t j = 1; j < (sel_len - half_len); j++) {
                    float mult = (float)j / (float)(sel_len - half_len - 1);
                    master_tracks[i].active_buffer[(start + half_len + j) * 2] *= mult;
                    master_tracks[i].active_buffer[(start + half_len + j) * 2 + 1] *= mult;
                }
            }
        }
    }

    if ((mask & (1 << current_timebase)) && pristine_bt_buf && master_tracks[current_timebase].active_buffer) {
        memcpy(pristine_bt_buf, master_tracks[current_timebase].active_buffer, pristine_frames * 2 * sizeof(float));
        atomic_store_explicit(&stretch_queue.read_index, atomic_load_explicit(&stretch_queue.write_index, memory_order_relaxed), memory_order_release);
        atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);
    }
    resume_rt_thread();
    clear_loop_points();
}

void apply_blend_fade_selection(void) {
    if (!atomic_load_explicit(&loop_active, memory_order_acquire)) return;
    size_t start = atomic_load_explicit(&loop_start_frame, memory_order_acquire);
    size_t end = atomic_load_explicit(&loop_end_frame, memory_order_acquire);
    if (start >= end || end > pristine_frames) return;

    size_t sel_len = end - start;
    if (sel_len < 4) { clear_loop_points(); return; }

    if (!await_rt_thread_detach()) return;

    if (end > pristine_frames) {
        resume_rt_thread();
        return;
    }

    int current_timebase = atomic_load_explicit(&timebase_track, memory_order_acquire);
    uint32_t mask = atomic_load_explicit(&selected_tracks_mask, memory_order_acquire);
    internal_backup_masked_tracks(mask);

    size_t half_len = sel_len / 2;
    size_t shift_len = sel_len - half_len;
    size_t remaining = pristine_frames - end;

    for (int i = 0; i < MAX_TRACKS; i++) {
        if ((mask & (1 << i)) && master_tracks[i].active_buffer) {
            for (size_t j = 0; j < half_len; j++) {
                float mult = 1.0f - ((float)j / (float)half_len);
                master_tracks[i].active_buffer[(start + j) * 2] *= mult;
                master_tracks[i].active_buffer[(start + j) * 2 + 1] *= mult;
            }
            for (size_t j = 0; j < half_len; j++) {
                float mult = ((float)j / (float)half_len);
                master_tracks[i].active_buffer[(start + j) * 2] += master_tracks[i].active_buffer[(start + shift_len + j) * 2] * mult;
                master_tracks[i].active_buffer[(start + j) * 2 + 1] += master_tracks[i].active_buffer[(start + shift_len + j) * 2 + 1] * mult;
            }
            if (remaining > 0) {
                memmove(&master_tracks[i].active_buffer[(start + half_len) * 2],
                        &master_tracks[i].active_buffer[end * 2],
                        remaining * 2 * sizeof(float));
            }
            memset(&master_tracks[i].active_buffer[(pristine_frames - shift_len) * 2], 0, shift_len * 2 * sizeof(float));
        }
    }

    if ((mask & (1 << current_timebase)) && pristine_bt_buf && master_tracks[current_timebase].active_buffer) {
        memcpy(pristine_bt_buf, master_tracks[current_timebase].active_buffer, pristine_frames * 2 * sizeof(float));
        atomic_store_explicit(&stretch_queue.read_index, atomic_load_explicit(&stretch_queue.write_index, memory_order_relaxed), memory_order_release);
        atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);
    }
    resume_rt_thread();
    clear_loop_points();
}

void apply_gain_selection(float multiplier) {
    int current_timebase = atomic_load_explicit(&timebase_track, memory_order_acquire);
    uint32_t mask = atomic_load_explicit(&selected_tracks_mask, memory_order_acquire);

    static gint64 last_gain_edit_time = 0;
    gint64 now = g_get_monotonic_time();
    bool is_continuous = (now - last_gain_edit_time < 400000);
    last_gain_edit_time = now;

    if (!await_rt_thread_detach()) return;

    if (!is_continuous) {
        internal_backup_masked_tracks(mask);
    }

    size_t start = 0;
    size_t end = pristine_frames;

    if (atomic_load_explicit(&loop_active, memory_order_acquire)) {
        size_t l_start = atomic_load_explicit(&loop_start_frame, memory_order_acquire);
        size_t l_end = atomic_load_explicit(&loop_end_frame, memory_order_acquire);
        if (l_start < l_end && l_end <= pristine_frames) {
            start = l_start;
            end = l_end;
        }
    }

    bool bt_updated = false;
    for (int i = 0; i < MAX_TRACKS; i++) {
        if ((mask & (1 << i)) && master_tracks[i].active_buffer) {
            for (size_t f = start; f < end; f++) {
                master_tracks[i].active_buffer[f * 2] *= multiplier;
                master_tracks[i].active_buffer[f * 2 + 1] *= multiplier;
            }
            if (i == current_timebase) bt_updated = true;
        }
    }

    if (bt_updated && pristine_bt_buf && master_tracks[current_timebase].active_buffer) {
        memcpy(pristine_bt_buf, master_tracks[current_timebase].active_buffer, pristine_frames * 2 * sizeof(float));
        atomic_store_explicit(&stretch_queue.read_index, atomic_load_explicit(&stretch_queue.write_index, memory_order_relaxed), memory_order_release);
        atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);
    }

    mark_session_dirty();
    resume_rt_thread();
}

void undo_selection(void) {
    if (!await_rt_thread_detach()) return;

    if (global_undo_ledger.head <= 0 || global_undo_ledger.count <= 0) {
        resume_rt_thread();
        return;
    }

    int current_timebase = atomic_load_explicit(&timebase_track, memory_order_acquire);
    int last_idx = (global_undo_ledger.tail + global_undo_ledger.head - 1) % global_undo_ledger.max_capacity;
    uint32_t target_trans_id = global_undo_ledger.states[last_idx].transaction_id;
    bool bt_updated = false;

    bool stash_video = false;
    bool restore_video = false;

    if (video_creation_trans_id != 0 && target_trans_id == video_creation_trans_id) {
        stash_video = true;
    }
    if (video_deletion_trans_id != 0 && target_trans_id == video_deletion_trans_id) {
        restore_video = true;
    }

    size_t present_bounds = atomic_load_explicit(&backing_track_frames, memory_order_acquire);
    extern size_t video_start_frame;
    extern size_t video_end_frame;
    size_t present_v_start = video_start_frame;
    size_t present_v_end = video_end_frame;
    bool globals_applied = false;

    while (global_undo_ledger.head > 0) {
        int current_idx = (global_undo_ledger.tail + global_undo_ledger.head - 1) % global_undo_ledger.max_capacity;
        UndoState *state = &global_undo_ledger.states[current_idx];

        if (state->transaction_id != target_trans_id) break;

        int trk = state->track_idx;
        if (trk >= 0 && trk < MAX_TRACKS && state->buffer && master_tracks[trk].active_buffer) {
            float *tmp_buf = master_tracks[trk].active_buffer;
            master_tracks[trk].active_buffer = state->buffer;
            state->buffer = tmp_buf;

            bool tmp_audio = atomic_load_explicit(&master_tracks[trk].has_audio, memory_order_acquire);
            atomic_store_explicit(&master_tracks[trk].has_audio, state->has_audio, memory_order_release);
            state->has_audio = tmp_audio;

            // Structural Undo tracking
            float tmp_gain = atomic_load_explicit(&master_tracks[trk].gain, memory_order_acquire);
            atomic_store_explicit(&master_tracks[trk].gain, state->gain, memory_order_release);
            state->gain = tmp_gain;

            bool tmp_solo = atomic_load_explicit(&master_tracks[trk].is_soloed, memory_order_acquire);
            atomic_store_explicit(&master_tracks[trk].is_soloed, state->is_soloed, memory_order_release);
            state->is_soloed = tmp_solo;

            char tmp_name[64];
            extern void get_multitrack_track_name(int, char*);
            extern void set_multitrack_track_name(int, const char*);
            get_multitrack_track_name(trk, tmp_name);
            set_multitrack_track_name(trk, state->name);
            strncpy(state->name, tmp_name, 64);

            if (!globals_applied) {
                atomic_store_explicit(&backing_track_frames, state->canvas_frames, memory_order_release);

                // FIX: Restore absolute memory bounds based on the restored stretched frames
                float current_speed = atomic_load_explicit(&playback_speed, memory_order_relaxed);
                pristine_frames = (size_t)(state->canvas_frames * current_speed);

                video_start_frame = state->v_start;
                video_end_frame = state->v_end;
                globals_applied = true;
            }

            state->canvas_frames = present_bounds;
            state->v_start = present_v_start;
            state->v_end = present_v_end;

            if (trk == current_timebase) bt_updated = true;
        }

        global_undo_ledger.head--;
    }

    int highest = 0;
    for (int i = 0; i < MAX_TRACKS; i++) {
        if (atomic_load_explicit(&master_tracks[i].has_audio, memory_order_acquire)) highest = i;
    }

    int current_rec = atomic_load_explicit(&current_recording_track, memory_order_acquire);
    if (current_rec > highest && !atomic_load_explicit(&master_tracks[current_rec].has_audio, memory_order_acquire)) {
        current_rec = highest;
        atomic_store_explicit(&current_recording_track, current_rec, memory_order_release);
        atomic_store_explicit(&selected_tracks_mask, (1 << current_rec), memory_order_release);
    }

    if (current_rec > highest) highest = current_rec;
    atomic_store_explicit(&active_track_count, highest + 1, memory_order_release);

    if (bt_updated && pristine_bt_buf && master_tracks[current_timebase].active_buffer) {
        memcpy(pristine_bt_buf, master_tracks[current_timebase].active_buffer, pristine_frames * 2 * sizeof(float));
        atomic_store_explicit(&stretch_queue.read_index, atomic_load_explicit(&stretch_queue.write_index, memory_order_relaxed), memory_order_release);
        atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);
    }

    evaluate_session_dirty_state();
    resume_rt_thread();

    if (stash_video) {
        extern void abort_active_video_preview(bool stash);
        abort_active_video_preview(true);
    }
    if (restore_video) {
        extern void restore_stashed_video_preview(void);
        restore_stashed_video_preview();
    }
}

void redo_selection(void) {
    if (!await_rt_thread_detach()) return;

    if (global_undo_ledger.head >= global_undo_ledger.count) {
        resume_rt_thread();
        return;
    }

    int current_timebase = atomic_load_explicit(&timebase_track, memory_order_acquire);
    int next_idx = (global_undo_ledger.tail + global_undo_ledger.head) % global_undo_ledger.max_capacity;
    uint32_t target_trans_id = global_undo_ledger.states[next_idx].transaction_id;
    bool bt_updated = false;

    bool stash_video = false;
    bool restore_video = false;

    if (video_creation_trans_id != 0 && target_trans_id == video_creation_trans_id) {
        restore_video = true;
    }
    if (video_deletion_trans_id != 0 && target_trans_id == video_deletion_trans_id) {
        stash_video = true;
    }

    size_t present_bounds = atomic_load_explicit(&backing_track_frames, memory_order_acquire);
    extern size_t video_start_frame;
    extern size_t video_end_frame;
    size_t present_v_start = video_start_frame;
    size_t present_v_end = video_end_frame;
    bool globals_applied = false;

    while (global_undo_ledger.head < global_undo_ledger.count) {
        int current_idx = (global_undo_ledger.tail + global_undo_ledger.head) % global_undo_ledger.max_capacity;
        UndoState *state = &global_undo_ledger.states[current_idx];

        if (state->transaction_id != target_trans_id) break;

        int trk = state->track_idx;
        if (trk >= 0 && trk < MAX_TRACKS && state->buffer && master_tracks[trk].active_buffer) {
            float *tmp_buf = master_tracks[trk].active_buffer;
            master_tracks[trk].active_buffer = state->buffer;
            state->buffer = tmp_buf;

            bool tmp_audio = atomic_load_explicit(&master_tracks[trk].has_audio, memory_order_acquire);
            atomic_store_explicit(&master_tracks[trk].has_audio, state->has_audio, memory_order_release);
            state->has_audio = tmp_audio;

            // Structural Undo tracking
            float tmp_gain = atomic_load_explicit(&master_tracks[trk].gain, memory_order_acquire);
            atomic_store_explicit(&master_tracks[trk].gain, state->gain, memory_order_release);
            state->gain = tmp_gain;

            bool tmp_solo = atomic_load_explicit(&master_tracks[trk].is_soloed, memory_order_acquire);
            atomic_store_explicit(&master_tracks[trk].is_soloed, state->is_soloed, memory_order_release);
            state->is_soloed = tmp_solo;

            char tmp_name[64];
            extern void get_multitrack_track_name(int, char*);
            extern void set_multitrack_track_name(int, const char*);
            get_multitrack_track_name(trk, tmp_name);
            set_multitrack_track_name(trk, state->name);
            strncpy(state->name, tmp_name, 64);

            if (!globals_applied) {
                atomic_store_explicit(&backing_track_frames, state->canvas_frames, memory_order_release);
                video_start_frame = state->v_start;
                video_end_frame = state->v_end;
                globals_applied = true;
            }

            state->canvas_frames = present_bounds;
            state->v_start = present_v_start;
            state->v_end = present_v_end;

            if (trk == current_timebase) bt_updated = true;
        }

        global_undo_ledger.head++;
    }

    int highest = 0;
    for (int i = 0; i < MAX_TRACKS; i++) {
        if (atomic_load_explicit(&master_tracks[i].has_audio, memory_order_acquire)) highest = i;
    }

    int current_rec = atomic_load_explicit(&current_recording_track, memory_order_acquire);
    if (current_rec > highest && !atomic_load_explicit(&master_tracks[current_rec].has_audio, memory_order_acquire)) {
        current_rec = highest;
        atomic_store_explicit(&current_recording_track, current_rec, memory_order_release);
        atomic_store_explicit(&selected_tracks_mask, (1 << current_rec), memory_order_release);
    }

    if (current_rec > highest) highest = current_rec;
    atomic_store_explicit(&active_track_count, highest + 1, memory_order_release);

    if (bt_updated && pristine_bt_buf && master_tracks[current_timebase].active_buffer) {
        memcpy(pristine_bt_buf, master_tracks[current_timebase].active_buffer, pristine_frames * 2 * sizeof(float));
        atomic_store_explicit(&stretch_queue.read_index, atomic_load_explicit(&stretch_queue.write_index, memory_order_relaxed), memory_order_release);
        atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);
    }

    evaluate_session_dirty_state();
    resume_rt_thread();

    if (stash_video) {
        extern void abort_active_video_preview(bool stash);
        abort_active_video_preview(true);
    }
    if (restore_video) {
        extern void restore_stashed_video_preview(void);
        restore_stashed_video_preview();
    }
}

void crop_canvas(void) {
    size_t target_frames_1x = 0;

    if (atomic_load_explicit(&loop_active, memory_order_acquire)) {
        float current_speed = atomic_load_explicit(&playback_speed, memory_order_relaxed);
        target_frames_1x = (size_t)(atomic_load_explicit(&loop_end_frame, memory_order_acquire) * current_speed);
    } else {
        target_frames_1x = get_last_audible_frame();
    }

    int rate = atomic_load_explicit(&active_sample_rate, memory_order_acquire);
    if (rate <= 0) rate = 48000;

    if (target_frames_1x < (size_t)rate * 5) target_frames_1x = (size_t)rate * 5;
    if (target_frames_1x >= pristine_frames) return;

    if (!await_rt_thread_detach()) return;

    int current_timebase = atomic_load_explicit(&timebase_track, memory_order_acquire);
    uint32_t mask = 0xFFFFFFFF;
    internal_backup_masked_tracks(mask);

    size_t clear_len = pristine_frames - target_frames_1x;
    for (int i = 0; i < MAX_TRACKS; i++) {
        if (master_tracks[i].active_buffer) {
            memset(&master_tracks[i].active_buffer[target_frames_1x * 2], 0, clear_len * 2 * sizeof(float));
        }
    }

    if (pristine_bt_buf && master_tracks[current_timebase].active_buffer) {
        memcpy(pristine_bt_buf, master_tracks[current_timebase].active_buffer, target_frames_1x * 2 * sizeof(float));
    }

    pristine_frames = target_frames_1x;

    float current_speed = atomic_load_explicit(&playback_speed, memory_order_relaxed);
    size_t new_bt_frames = (size_t)(pristine_frames / current_speed);

    atomic_store_explicit(&backing_track_frames, new_bt_frames, memory_order_release);

    size_t current_pos = atomic_load_explicit(&playback_pos, memory_order_acquire);
    if (current_pos > new_bt_frames) {
        atomic_store_explicit(&playback_pos, new_bt_frames, memory_order_release);
    }

    atomic_store_explicit(&stretch_queue.read_index, atomic_load_explicit(&stretch_queue.write_index, memory_order_relaxed), memory_order_release);
    atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);

    resume_rt_thread();
    clear_loop_points();
}

/**
 * @brief Renders a dynamically allocated, fully mixed stereo buffer of the requested region.
 * Applies track gains, solo/mute states, and soft clipping identically to the RT playback engine.
 * @warning Must be called while the RT thread is detached to prevent memory races.
 * @param start The start frame.
 * @param end The end frame.
 * @return Allocated float array of the mixed audio, or NULL on failure.
 */
float* render_mixdown_region(size_t start, size_t end) {
    if (start >= end || end > pristine_frames) return NULL;
    size_t len = end - start;

    float *mix_buf = calloc(len * 2, sizeof(float));
    if (!mix_buf) return NULL;

    // Pass 1: Global Solo Check
    bool any_solo = false;
    for (int l = 0; l < MAX_TRACKS; l++) {
        if (atomic_load_explicit(&master_tracks[l].is_soloed, memory_order_relaxed)) {
            any_solo = true; break;
        }
    }

    // Pass 2: Sum all audible layers
    for (size_t i = 0; i < len; i++) {
        float sum_l = 0.0f;
        float sum_r = 0.0f;

        for (int l = 0; l < MAX_TRACKS; l++) {
            if (!master_tracks[l].active_buffer) continue;

            bool is_audible = atomic_load_explicit(&master_tracks[l].has_audio, memory_order_relaxed) &&
            (!any_solo || atomic_load_explicit(&master_tracks[l].is_soloed, memory_order_relaxed));

            if (is_audible) {
                float gain = atomic_load_explicit(&master_tracks[l].gain, memory_order_relaxed);
                sum_l += master_tracks[l].active_buffer[(start + i) * 2] * gain;
                sum_r += master_tracks[l].active_buffer[(start + i) * 2 + 1] * gain;
            }
        }

        mix_buf[i * 2] = soft_clip(sum_l);
        mix_buf[i * 2 + 1] = soft_clip(sum_r);
    }

    return mix_buf;
}

void get_engine_telemetry(EngineTelemetry *out_stats) {
    if (!out_stats) return;

    out_stats->dropped_frames = atomic_load_explicit(&rt_dropped_audio_frames, memory_order_relaxed);
    out_stats->sample_rate = atomic_load_explicit(&active_sample_rate, memory_order_relaxed);
    out_stats->ledger_capacity = global_undo_ledger.max_capacity;

    // Calculate unique user actions (Ctrl+Z steps)
    out_stats->ledger_undos = 0;
    uint32_t last_id = 0;
    for (int i = 0; i < global_undo_ledger.head; i++) {
        int idx = (global_undo_ledger.tail + i) % global_undo_ledger.max_capacity;
        if (global_undo_ledger.states[idx].transaction_id != last_id) {
            out_stats->ledger_undos++;
            last_id = global_undo_ledger.states[idx].transaction_id;
        }
    }

    out_stats->ledger_redos = 0;
    last_id = 0;
    for (int i = global_undo_ledger.head; i < global_undo_ledger.count; i++) {
        int idx = (global_undo_ledger.tail + i) % global_undo_ledger.max_capacity;
        if (global_undo_ledger.states[idx].transaction_id != last_id) {
            out_stats->ledger_redos++;
            last_id = global_undo_ledger.states[idx].transaction_id;
        }
    }

    out_stats->total_ram_bytes = 0;
    out_stats->available_ram_bytes = 0;
    FILE *meminfo = fopen("/proc/meminfo", "r");
    if (meminfo) {
        char line[256];
        while (fgets(line, sizeof(line), meminfo)) {
            long long kb_val;
            if (sscanf(line, "MemTotal: %lld", &kb_val) == 1) {
                out_stats->total_ram_bytes = (size_t)kb_val * 1024;
            } else if (sscanf(line, "MemAvailable: %lld", &kb_val) == 1) {
                out_stats->available_ram_bytes = (size_t)kb_val * 1024;
            }
        }
        fclose(meminfo);
    }

    // Parse true QJams allocated memory (Resident Set Size)
    out_stats->qjams_rss_bytes = 0;
    FILE *statm = fopen("/proc/self/statm", "r");
    if (statm) {
        long size, rss;
        if (fscanf(statm, "%ld %ld", &size, &rss) == 2) {
            out_stats->qjams_rss_bytes = (size_t)rss * sysconf(_SC_PAGESIZE);
        }
        fclose(statm);
    }
}

size_t get_last_audible_frame(void) {
    if (pristine_frames == 0) return 0;
    size_t last_frame = 0;

    // Threshold for "silence" (~ -76dB) to ignore analog ADC noise floors
    float noise_floor = 0.00015f;

    for (int t = 0; t < MAX_TRACKS; t++) {
        if (!master_tracks[t].active_buffer || !atomic_load_explicit(&master_tracks[t].has_audio, memory_order_acquire)) continue;

        // Scan backwards from the end of the canvas
        for (size_t f = pristine_frames; f > last_frame; f--) {
            // Check absolute float value against the noise gate rather than strict 0.0f
            if (fabsf(master_tracks[t].active_buffer[(f - 1) * 2]) > noise_floor ||
                fabsf(master_tracks[t].active_buffer[(f - 1) * 2 + 1]) > noise_floor) {
                last_frame = f;
            break;
                }
        }
    }
    return last_frame;
}

_Atomic size_t cached_audible_frames = 0;
static _Atomic bool is_scanning_audible = false;

static void audible_scan_thread(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable) {
    (void)source_object; (void)task_data; (void)cancellable;
    size_t last = get_last_audible_frame();
    atomic_store_explicit(&cached_audible_frames, last, memory_order_release);
    atomic_store_explicit(&is_scanning_audible, false, memory_order_release);
    g_task_return_boolean(task, TRUE);
}

void trigger_audible_scan_async(void) {
    if (!atomic_exchange_explicit(&is_scanning_audible, true, memory_order_acquire)) {
        GTask *task = g_task_new(NULL, NULL, NULL, NULL);
        g_task_run_in_thread(task, audible_scan_thread);
        g_object_unref(task);
    }
}
