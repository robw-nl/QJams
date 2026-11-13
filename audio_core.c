#include <ctype.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <glib.h>
#include <jack/jack.h>
#include <math.h>
#include <pthread.h>
#include <rubberband/rubberband-c.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "audio_core.h"
#include "audio_internal.h"
#include "audio_memory.h"
#include "audio_routing.h"
#include "config.h"
#include "encoder.h"
#include "ringbuffer.h"
#include "video_ringbuffer.h"

alignas(64) _Atomic float input_gain = 1.0f;
static _Atomic float bt_gain = 1.0f;

static alignas(64) QJTransportState internal_transport = {.is_recording = false,
                                                          .is_armed = false,
                                                          .is_paused = false,
                                                          .is_playing = false,
                                                          .backing_track_frames = 0,
                                                          .playback_pos = 0,
                                                          .active_sample_rate = 44100,
                                                          .playback_speed = 1.0f};

QJTransportState *get_transport_state(void) {
    return &internal_transport;
}

_Atomic bool loop_active = false;
_Atomic size_t loop_start_frame = 0;
_Atomic size_t loop_end_frame = 0;

_Atomic uint32_t dsp_heartbeat = 0;
_Atomic bool audio_available = true;

alignas(64) _Atomic float vu_peak_input_l = 0.0f;
_Atomic float vu_peak_input_r = 0.0f;
_Atomic float vu_peak_bt_l = 0.0f;
_Atomic float vu_peak_bt_r = 0.0f;

/** @brief Cache-line isolated JACK client and port handles (read-mostly in RT callback). */
alignas(64) jack_client_t *client;
jack_port_t *input_port_1, *input_port_2;
jack_port_t *output_port_1, *output_port_2;
SPSC_Audio_Queue audio_queue;
SPSC_Video_Queue video_queue;
SPSC_Command_Queue cmd_queue;

static int click_frames_remaining = 0;
static double click_phase = 0.0;

RubberBandState rt_rb_state = NULL;
atomic_size_t pristine_read_pos = 0;

SPSC_Stereo_Queue stretch_queue;
pthread_t stretcher_thread;
_Atomic bool stretcher_keep_running = false;
_Atomic bool stretcher_flush_request = false;
_Atomic bool stretcher_ratio_update = false;

_Atomic int active_track_count = 0;
_Atomic int current_recording_track = 0;
_Atomic int timebase_track = 0;

_Atomic bool seek_flag = false;
_Atomic double seek_target = 0.0;
static int output_fade_remaining = 0;
static int fade_down_remaining = 0;
static size_t pending_seek_pos = 0;
_Atomic bool loop_wrap_pending = false;
_Atomic bool loop_wrap_was_recording = false;
_Atomic bool rt_loop_countdown_enabled = false;

_Atomic bool speed_change_flag = false;
_Atomic float speed_target = 1.0f;
_Atomic bool engine_is_shutting_down = false;

_Atomic uint32_t rt_dropped_audio_frames = 0;

_Atomic size_t cached_audible_frames = 0;
_Atomic bool is_scanning_audible = false;

int event_pipe[2] = {-1, -1};

extern atomic_size_t video_start_frame;
extern atomic_size_t video_end_frame;
extern _Atomic bool video_is_armed;

/**
 * @brief Strictly clamps floating-point audio to the [-1.0, 1.0] range.
 * Guarantees bit-perfect linear amplitude for all valid signals to preserve
 * commercial mastering, while preventing integer wrap-around in the 16-bit encoder.
 */
inline float soft_clip(float x) {
    if (x > 1.0f)
        return 1.0f;
    if (x < -1.0f)
        return -1.0f;
    return x;
}

static inline void update_peak_decay(_Atomic float *peak_var, float current_max) {
    float prev = atomic_load_explicit(peak_var, memory_order_relaxed);
    if (current_max < prev)
        current_max = prev * 0.98f;
    if (current_max < 0.0001f)
        current_max = 0.0f;
    atomic_store_explicit(peak_var, current_max, memory_order_relaxed);
}

static void *stretcher_loop(void *arg) {
    (void)arg;
    float in_l[1024];
    float in_r[1024];
    float out_l[1024];
    float out_r[1024];

    while (atomic_load_explicit(&stretcher_keep_running, memory_order_acquire)) {
        if (atomic_load_explicit(&request_track_free, memory_order_acquire)) {
            atomic_store_explicit(&stretcher_safe_to_free, true, memory_order_release);
            usleep(5000);
            continue;
        }

        /**
         * @brief Background worker loop for real-time RubberBand time-stretching.
         *        Reads directly from active master_tracks buffers when playback_speed != 1.0f.
         */
        if (!rt_rb_state || atomic_load_explicit(&pristine_frames, memory_order_acquire) == 0) {
            usleep(5000);
            continue;
        }

        if (atomic_load_explicit(&stretcher_flush_request, memory_order_acquire)) {
            rubberband_reset(rt_rb_state);
            flush_stereo_queue_consumer(&stretch_queue);
            float current_speed =
                atomic_load_explicit(&get_transport_state()->playback_speed, memory_order_relaxed);
            size_t sync_pos =
                atomic_load_explicit(&get_transport_state()->playback_pos, memory_order_acquire);
            size_t target_pristine = (size_t)(sync_pos * current_speed);
            size_t max_pristine = atomic_load_explicit(&pristine_frames, memory_order_acquire);
            if (target_pristine > max_pristine)
                target_pristine = max_pristine;
            atomic_store_explicit(&pristine_read_pos, target_pristine, memory_order_release);
            atomic_store_explicit(&stretcher_flush_request, false, memory_order_release);
        }

        if (atomic_exchange_explicit(&stretcher_ratio_update, false, memory_order_acquire)) {
            float speed =
                atomic_load_explicit(&get_transport_state()->playback_speed, memory_order_relaxed);
            rubberband_set_time_ratio(rt_rb_state, 1.0 / speed);
        }

        bool active =
            atomic_load_explicit(&get_transport_state()->is_playing, memory_order_acquire) ||
            atomic_load_explicit(&get_transport_state()->is_recording, memory_order_acquire);
        bool paused = atomic_load_explicit(&get_transport_state()->is_paused, memory_order_acquire);
        float current_speed =
            atomic_load_explicit(&get_transport_state()->playback_speed, memory_order_relaxed);

        // FATAL FIZZ FIX: Do not process RubberBand frames if we are at exactly 1.0x speed
        if (!active || paused || current_speed == 1.0f) {
            usleep(5000);
            continue;
        }

        size_t write_idx = atomic_load_explicit(&stretch_queue.write_index, memory_order_relaxed);
        size_t read_idx = atomic_load_explicit(&stretch_queue.read_index, memory_order_acquire);

        if ((write_idx - read_idx) >= 16384) {
            usleep(2000);
            continue;
        }

        size_t current_read_pos = atomic_load_explicit(&pristine_read_pos, memory_order_acquire);
        size_t local_pristine = atomic_load_explicit(&pristine_frames, memory_order_acquire);

        if (rubberband_available(rt_rb_state) < 1024 && current_read_pos < local_pristine) {
            size_t push_chunk = 1024;
            if (current_read_pos + push_chunk > local_pristine) {
                push_chunk = local_pristine - current_read_pos;
            }

            bool any_solo = false;
            for (int l = 0; l < QJ_MAX_TRACKS; l++) {
                if (atomic_load_explicit(&master_tracks[l].is_soloed, memory_order_relaxed)) {
                    any_solo = true;
                    break;
                }
            }

            /**
             * @brief Determines if recording is currently active and identifies the armed track
             * to explicitly exclude it from the asynchronous time-stretcher playback summation.
             */
            bool is_rec =
                atomic_load_explicit(&get_transport_state()->is_recording, memory_order_acquire);
            int rec_track = atomic_load_explicit(&current_recording_track, memory_order_relaxed);

            for (size_t k = 0; k < push_chunk; k++) {
                float sum_l = 0.0f;
                float sum_r = 0.0f;

                for (int l = 0; l < QJ_MAX_TRACKS; l++) {
                    if (!master_tracks[l].active_buffer)
                        continue;
                    if (is_rec && l == rec_track)
                        continue;

                    bool is_audible =
                        atomic_load_explicit(&master_tracks[l].has_audio, memory_order_relaxed) &&
                        (!any_solo ||
                         atomic_load_explicit(&master_tracks[l].is_soloed, memory_order_relaxed));

                    if (is_audible) {
                        float gain =
                            atomic_load_explicit(&master_tracks[l].gain, memory_order_relaxed);
                        sum_l += master_tracks[l].active_buffer[(current_read_pos + k) * 2] * gain;
                        sum_r +=
                            master_tracks[l].active_buffer[(current_read_pos + k) * 2 + 1] * gain;
                    }
                }
                in_l[k] = sum_l;
                in_r[k] = sum_r;
            }

            const float *in_ptrs[2] = {in_l, in_r};
            if (atomic_load_explicit(&stretcher_flush_request, memory_order_acquire) ||
                atomic_load_explicit(&stretcher_ratio_update, memory_order_acquire) ||
                atomic_load_explicit(&request_track_free, memory_order_acquire)) {
                continue;
            }
            rubberband_process(rt_rb_state, in_ptrs, push_chunk, 0);

            atomic_store_explicit(&pristine_read_pos, current_read_pos + push_chunk,
                                  memory_order_release);
        }

        if (atomic_load_explicit(&stretcher_flush_request, memory_order_acquire) ||
            atomic_load_explicit(&stretcher_ratio_update, memory_order_acquire) ||
            atomic_load_explicit(&request_track_free, memory_order_acquire)) {
            continue;
        }

        int available = rubberband_available(rt_rb_state);
        int frames_to_read = (available >= 1024) ? 1024 : available;

        if (frames_to_read <= 0 && current_read_pos >= local_pristine) {
            usleep(2000);
            continue;
        }

        if (frames_to_read > 0) {
            float *out_ptrs[2] = {out_l, out_r};
            rubberband_retrieve(rt_rb_state, out_ptrs, frames_to_read);
            for (int i = 0; i < frames_to_read; i++) {
                if (atomic_load_explicit(&stretcher_flush_request, memory_order_acquire) ||
                    atomic_load_explicit(&request_track_free, memory_order_acquire)) {
                    break;
                }
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
 * @brief Real-time JACK process callback executing lock-free multi-track playback, overdub
 * recording, and metering.
 */
int process_audio(jack_nframes_t nframes, void *arg) {
    (void)arg;

    atomic_fetch_add_explicit(&dsp_heartbeat, 1, memory_order_relaxed);

    jack_nframes_t cycle_start_time = jack_last_frame_time(client);

    jack_default_audio_sample_t *out_1 =
        (jack_default_audio_sample_t *)jack_port_get_buffer(output_port_1, nframes);
    jack_default_audio_sample_t *out_2 =
        (jack_default_audio_sample_t *)jack_port_get_buffer(output_port_2, nframes);
    jack_default_audio_sample_t *in_1 =
        (jack_default_audio_sample_t *)jack_port_get_buffer(input_port_1, nframes);
    jack_default_audio_sample_t *in_2 =
        (jack_default_audio_sample_t *)jack_port_get_buffer(input_port_2, nframes);

    EngineCommand cmd;
    while (pop_command(&cmd_queue, &cmd)) {
        switch (cmd.type) {
            case CMD_PLAY_CLICK: {
                int rate = atomic_load_explicit(&get_transport_state()->active_sample_rate,
                                                memory_order_acquire);
                click_frames_remaining = (int)((rate > 0 ? rate : 48000) * 0.05);
                click_phase = 0.0;
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

    bool capture_available = atomic_load_explicit(&audio_available, memory_order_acquire);

    size_t current_pos =
        atomic_load_explicit(&get_transport_state()->playback_pos, memory_order_acquire);
    bool recording =
        atomic_load_explicit(&get_transport_state()->is_recording, memory_order_acquire);
    bool playing = atomic_load_explicit(&get_transport_state()->is_playing, memory_order_acquire);
    bool paused = atomic_load_explicit(&get_transport_state()->is_paused, memory_order_acquire);
    bool active = recording || playing;

    // --- BLOCK-LEVEL SNAPSHOTS ---
    // Hoisting atomics out of the per-sample loop for strict L1 cache efficiency
    float current_input_gain = atomic_load_explicit(&input_gain, memory_order_relaxed);
    float current_bt_gain = atomic_load_explicit(&bt_gain, memory_order_relaxed);
    float local_speed =
        atomic_load_explicit(&get_transport_state()->playback_speed, memory_order_relaxed);
    int local_sample_rate =
        atomic_load_explicit(&get_transport_state()->active_sample_rate, memory_order_relaxed);
    if (local_sample_rate <= 0)
        local_sample_rate = 48000;
    int local_timebase = atomic_load_explicit(&timebase_track, memory_order_relaxed);
    if (local_timebase < 0 || local_timebase >= QJ_MAX_TRACKS)
        local_timebase = 0;
    int local_rec_track = atomic_load_explicit(&current_recording_track, memory_order_relaxed);
    int local_layers = atomic_load_explicit(&active_track_count, memory_order_relaxed);
    size_t local_pristine_frames = atomic_load_explicit(&pristine_frames, memory_order_acquire);

    float local_gains[QJ_MAX_TRACKS];
    bool local_solo[QJ_MAX_TRACKS];
    bool local_has_audio[QJ_MAX_TRACKS];
    bool any_solo_active = false;

    for (int l = 0; l < QJ_MAX_TRACKS; l++) {
        local_gains[l] = atomic_load_explicit(&master_tracks[l].gain, memory_order_relaxed);
        local_solo[l] = atomic_load_explicit(&master_tracks[l].is_soloed, memory_order_relaxed);
        local_has_audio[l] =
            atomic_load_explicit(&master_tracks[l].has_audio, memory_order_relaxed);
        if (local_solo[l])
            any_solo_active = true;
    }

    // Drain the stretcher queue exactly once per block, outside the tight loop
    if (active && !paused && local_speed == 1.0f) {
        flush_stereo_queue_consumer(&stretch_queue);
    }

    float max_l = 0.0f, max_r = 0.0f;
    float max_bt_l = 0.0f, max_bt_r = 0.0f;

    bool do_seek = atomic_exchange_explicit(&seek_flag, false, memory_order_acquire);
    bool do_speed = atomic_exchange_explicit(&speed_change_flag, false, memory_order_acquire);
    bool is_video = atomic_load_explicit(&video_is_armed, memory_order_acquire);

    if (do_speed) {
        float speed = atomic_load_explicit(&speed_target, memory_order_relaxed);
        if (local_speed != speed) {
            atomic_store_explicit(&get_transport_state()->playback_speed, speed,
                                  memory_order_relaxed);
            atomic_store_explicit(&stretcher_ratio_update, true, memory_order_release);
            local_speed = speed;

            size_t bt_frames = atomic_load_explicit(&get_transport_state()->backing_track_frames,
                                                    memory_order_acquire);
            double fraction = (bt_frames > 0) ? ((double)current_pos / (double)bt_frames) : 0.0;
            double loop_s_frac =
                (bt_frames > 0)
                    ? ((double)atomic_load_explicit(&loop_start_frame, memory_order_relaxed) /
                       (double)bt_frames)
                    : 0.0;
            double loop_e_frac =
                (bt_frames > 0)
                    ? ((double)atomic_load_explicit(&loop_end_frame, memory_order_relaxed) /
                       (double)bt_frames)
                    : 0.0;

            size_t cur_v_start = atomic_load_explicit(&video_start_frame, memory_order_relaxed);
            size_t cur_v_end = atomic_load_explicit(&video_end_frame, memory_order_relaxed);
            double v_start_frac = (bt_frames > 0) ? ((double)cur_v_start / (double)bt_frames) : 0.0;
            double v_end_frac = (bt_frames > 0) ? ((double)cur_v_end / (double)bt_frames) : 0.0;

            size_t new_bt_frames = (size_t)(local_pristine_frames / speed);
            atomic_store_explicit(&get_transport_state()->backing_track_frames, new_bt_frames,
                                  memory_order_release);

            current_pos = (size_t)(fraction * new_bt_frames);
            atomic_store_explicit(&get_transport_state()->playback_pos, current_pos,
                                  memory_order_release);

            if (fade_down_remaining > 0 && bt_frames > 0) {
                pending_seek_pos =
                    (size_t)(((double)pending_seek_pos / (double)bt_frames) * new_bt_frames);
            }

            if (atomic_load_explicit(&loop_active, memory_order_acquire)) {
                atomic_store_explicit(&loop_start_frame, (size_t)(loop_s_frac * new_bt_frames),
                                      memory_order_relaxed);
                atomic_store_explicit(&loop_end_frame, (size_t)(loop_e_frac * new_bt_frames),
                                      memory_order_relaxed);
            }

            atomic_store_explicit(&video_start_frame, (size_t)(v_start_frac * new_bt_frames),
                                  memory_order_release);
            atomic_store_explicit(&video_end_frame, (size_t)(v_end_frac * new_bt_frames),
                                  memory_order_release);

            atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);
            flush_stereo_queue_consumer(&stretch_queue);
        }
    }

    if (do_seek) {
        double frac = atomic_load_explicit(&seek_target, memory_order_relaxed);
        size_t bt_frames = atomic_load_explicit(&get_transport_state()->backing_track_frames,
                                                memory_order_acquire);

        pending_seek_pos = (size_t)(frac * bt_frames);
        fade_down_remaining = 128; // Start fading down
        output_fade_remaining = 0; // Abort any active fade-ups
    }

    bool is_looping = atomic_load_explicit(&loop_active, memory_order_acquire);
    size_t l_start = atomic_load_explicit(&loop_start_frame, memory_order_relaxed);
    size_t l_end = atomic_load_explicit(&loop_end_frame, memory_order_relaxed);
    if (l_end <= l_start)
        is_looping = false;
    size_t active_pos = current_pos;
    size_t bt_frames =
        atomic_load_explicit(&get_transport_state()->backing_track_frames, memory_order_acquire);
    bool countdown_on_wrap = atomic_load_explicit(&rt_loop_countdown_enabled, memory_order_relaxed);
    for (jack_nframes_t i = 0; i < nframes; i++) {
        if (active && !paused && is_looping && active_pos >= l_end) {
            if (recording || countdown_on_wrap) {
                atomic_store_explicit(&loop_wrap_was_recording, recording, memory_order_relaxed);
                atomic_store_explicit(&loop_wrap_pending, true, memory_order_release);
                atomic_store_explicit(&get_transport_state()->is_paused, true,
                                      memory_order_release);
                paused = true;
            } else {
                size_t loop_len = l_end - l_start;
                active_pos = l_start + ((active_pos - l_end) % loop_len);
                atomic_store_explicit(&get_transport_state()->playback_pos, active_pos,
                                      memory_order_release);
                atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);
                flush_stereo_queue_consumer(&stretch_queue);
                output_fade_remaining = 128;
            }
        }
        float raw_bt_l = 0.0f;
        float raw_bt_r = 0.0f;
        size_t raw_pos = (size_t)((double)active_pos * (double)local_speed);
        if (active && !paused) {
            if (local_speed == 1.0f) {
                bool base_audible = local_has_audio[local_timebase] &&
                                    (!any_solo_active || local_solo[local_timebase]);
                if (master_tracks[local_timebase].active_buffer && base_audible &&
                    active_pos < bt_frames && active_pos < local_pristine_frames &&
                    (!recording || local_timebase != local_rec_track)) {
                    raw_bt_l = master_tracks[local_timebase].active_buffer[active_pos * 2] *
                               local_gains[local_timebase];
                    raw_bt_r = master_tracks[local_timebase].active_buffer[active_pos * 2 + 1] *
                               local_gains[local_timebase];
                }
            } else {
                StereoFrame sf;
                if (pop_stereo_frame(&stretch_queue, &sf)) {
                    raw_bt_l = sf.l;
                    raw_bt_r = sf.r;
                }
            }
        }
        if (active && !paused && active_pos < bt_frames && local_speed == 1.0f) {
            for (int l = 0; l < local_layers && l < QJ_MAX_TRACKS; l++) {
                if (l == local_timebase)
                    continue;
                if (!master_tracks[l].active_buffer || !local_has_audio[l])
                    continue;
                if (recording && l == local_rec_track)
                    continue;

                bool overdub_audible = true;
                if (any_solo_active && !local_solo[l])
                    overdub_audible = false;

                if (overdub_audible && active_pos < local_pristine_frames) {
                    raw_bt_l += master_tracks[l].active_buffer[active_pos * 2] * local_gains[l];
                    raw_bt_r += master_tracks[l].active_buffer[active_pos * 2 + 1] * local_gains[l];
                }
            }
        }

        if (active && !paused && is_looping && l_end > l_start + 256) {
            float fade_mult = 1.0f;
            if (active_pos >= l_end) {
                fade_mult = 0.0f;
            } else if (active_pos >= l_end - 128) {
                fade_mult = (float)(l_end - active_pos) / 128.0f;
            } else if (active_pos >= l_start && active_pos < l_start + 128) {
                fade_mult = (float)(active_pos - l_start) / 128.0f;
            }
            raw_bt_l *= fade_mult;
            raw_bt_r *= fade_mult;
        }

        float scaled_bt_l = raw_bt_l * current_bt_gain;
        float scaled_bt_r = raw_bt_r * current_bt_gain;

        // --- V-FADE STATE MACHINE ---
        if (fade_down_remaining > 0) {
            float fade_factor = (float)fade_down_remaining / 128.0f;
            scaled_bt_l *= fade_factor;
            scaled_bt_r *= fade_factor;
            fade_down_remaining--;

            if (fade_down_remaining == 0) {
                // Fade down complete. Snap the playhead to the new target.
                active_pos = pending_seek_pos;
                atomic_store_explicit(&get_transport_state()->playback_pos, active_pos,
                                      memory_order_release);

                // Flush the time-stretcher to clear stale samples
                atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);
                flush_stereo_queue_consumer(&stretch_queue);

                // Trigger the fade-up phase
                output_fade_remaining = 128;
            }
        } else if (output_fade_remaining > 0) {
            float fade_factor = (float)(128 - output_fade_remaining) / 128.0f;
            scaled_bt_l *= fade_factor;
            scaled_bt_r *= fade_factor;
            output_fade_remaining--;
        }

        if (fabsf(scaled_bt_l) > max_bt_l)
            max_bt_l = fabsf(scaled_bt_l);
        if (fabsf(scaled_bt_r) > max_bt_r)
            max_bt_r = fabsf(scaled_bt_r);

        out_1[i] = soft_clip(scaled_bt_l);
        out_2[i] = soft_clip(scaled_bt_r);

        if (click_frames_remaining > 0) {
            int total_click_frames = (int)(local_sample_rate * 0.05);

            double phase_inc = 2.0 * M_PI * 1500.0 / (double)local_sample_rate;
            float env = (float)click_frames_remaining / (float)total_click_frames;
            env = env * env * env;

            float click_samp = (float)(sin(click_phase) * 0.25f) * env;

            click_phase += phase_inc;
            if (click_phase >= 2.0 * M_PI)
                click_phase -= 2.0 * M_PI;

            out_1[i] = soft_clip(out_1[i] + click_samp);
            out_2[i] = soft_clip(out_2[i] + click_samp);

            click_frames_remaining--;
        }

        float raw_input_l = capture_available ? in_1[i] : 0.0f;
        float raw_input_r = capture_available ? in_2[i] : 0.0f;
        float scaled_input_l = raw_input_l * current_input_gain;
        float scaled_input_r = raw_input_r * current_input_gain;

        if (fabsf(scaled_input_l) > max_l)
            max_l = fabsf(scaled_input_l);
        if (fabsf(scaled_input_r) > max_r)
            max_r = fabsf(scaled_input_r);

        if (recording && !paused && raw_pos < local_pristine_frames) {
            if (local_rec_track >= 0 && local_rec_track < QJ_MAX_TRACKS &&
                master_tracks[local_rec_track].active_buffer) {
                // Ensure raw recordings are strictly clamped before saving
                master_tracks[local_rec_track].active_buffer[raw_pos * 2] = scaled_input_l;
                master_tracks[local_rec_track].active_buffer[raw_pos * 2 + 1] = scaled_input_r;
            }

            if (is_video) {
                AudioFrame *frame = acquire_audio_frame(&audio_queue);
                if (frame) {
                    frame->mix_l = soft_clip(scaled_bt_l + scaled_input_l);
                    frame->mix_r = soft_clip(scaled_bt_r + scaled_input_r);

                    for (int t = 0; t < QJ_MAX_TRACKS; t++) {
                        if (t == local_rec_track) {
                            frame->tracks_l[t] = scaled_input_l;
                            frame->tracks_r[t] = scaled_input_r;
                        } else if (t < local_layers && master_tracks[t].active_buffer &&
                                   local_has_audio[t]) {
                            frame->tracks_l[t] =
                                master_tracks[t].active_buffer[raw_pos * 2] * local_gains[t];
                            frame->tracks_r[t] =
                                master_tracks[t].active_buffer[raw_pos * 2 + 1] * local_gains[t];
                        } else {
                            frame->tracks_l[t] = 0.0f;
                            frame->tracks_r[t] = 0.0f;
                        }
                    }

                    frame->active_count = local_layers;
                    frame->pts = (int64_t)(cycle_start_time + i);
                    commit_audio_frame(&audio_queue);
                } else {
                    atomic_fetch_add_explicit(&rt_dropped_audio_frames, 1, memory_order_relaxed);
                }
            }
        }

        if (active && !paused) {
            active_pos++;
        }
    }

    update_peak_decay(&vu_peak_input_l, max_l);
    update_peak_decay(&vu_peak_input_r, max_r);
    update_peak_decay(&vu_peak_bt_l, max_bt_l);
    update_peak_decay(&vu_peak_bt_r, max_bt_r);

    if (active && !paused) {
        atomic_store_explicit(&get_transport_state()->playback_pos, active_pos,
                              memory_order_release);
    }

    return 0;
}

int jack_sample_rate_cb(jack_nframes_t nframes, void *arg) {
    (void)arg;
    atomic_store_explicit(&get_transport_state()->active_sample_rate, (int)nframes,
                          memory_order_release);
    return 0;
}

int get_jack_event_fd(void) {
    return event_pipe[0];
}

void jack_shutdown(void *arg) {
    (void)arg;
    if (event_pipe[1] != -1) {
        const uint8_t msg = 1;
        ssize_t dummy = write(event_pipe[1], &msg, sizeof(msg));
        (void)dummy;
    }
}

void set_input_gain(float multiplier) {
    atomic_store_explicit(&input_gain, multiplier, memory_order_relaxed);
}

void set_bt_gain(float multiplier) {
    atomic_store_explicit(&bt_gain, multiplier, memory_order_relaxed);
}

void set_multitrack_layer_gain(int track_idx, float multiplier) {
    if (track_idx >= 0 && track_idx < QJ_MAX_TRACKS) {
        atomic_store_explicit(&master_tracks[track_idx].gain, multiplier, memory_order_relaxed);
    }
}

void seek_backing_track(double fraction) {
    atomic_store_explicit(&seek_target, fraction, memory_order_relaxed);
    atomic_store_explicit(&seek_flag, true, memory_order_release);
}

void set_playback_speed(float speed) {
    atomic_store_explicit(&speed_target, speed, memory_order_relaxed);
    atomic_store_explicit(&speed_change_flag, true, memory_order_release);
}

void set_loop_points(double start_frac, double end_frac) {
    if (start_frac > end_frac) {
        double tmp = start_frac;
        start_frac = end_frac;
        end_frac = tmp;
    }
    size_t total_frames =
        atomic_load_explicit(&get_transport_state()->backing_track_frames, memory_order_acquire);
    size_t start = (size_t)(start_frac * total_frames);
    size_t end = (size_t)(end_frac * total_frames);
    if (end - start >= 1) {
        atomic_store_explicit(&loop_start_frame, start, memory_order_relaxed);
        atomic_store_explicit(&loop_end_frame, end, memory_order_relaxed);
        atomic_store_explicit(&loop_active, true, memory_order_release);
    }
}

void clear_loop_points(void) {
    atomic_store_explicit(&loop_active, false, memory_order_release);
    atomic_store_explicit(&loop_start_frame, 0, memory_order_relaxed);
    atomic_store_explicit(&loop_end_frame, 0, memory_order_relaxed);
}

int init_audio_engine(size_t queue_capacity, float init_input_gain, jack_client_t *existing_client,
                      const char *target_audio_device, const char *target_playback_device) {
    client = existing_client;
    set_input_gain(init_input_gain);

    if (event_pipe[0] == -1) {
        if (pipe(event_pipe) == 0) {
            fcntl(event_pipe[0], F_SETFL, fcntl(event_pipe[0], F_GETFL, 0) | O_NONBLOCK);
            fcntl(event_pipe[1], F_SETFL, fcntl(event_pipe[1], F_GETFL, 0) | O_NONBLOCK);
        }
    }

    jack_nframes_t current_rate = jack_get_sample_rate(client);
    atomic_store_explicit(&get_transport_state()->active_sample_rate, (int)current_rate,
                          memory_order_release);
    jack_set_sample_rate_callback(client, jack_sample_rate_cb, 0);

    init_spsc_queue(&audio_queue, queue_capacity);
    init_stereo_queue(&stretch_queue, 65536);
    init_command_queue(&cmd_queue, 32);

    atomic_store_explicit(&stretcher_keep_running, true, memory_order_release);
    pthread_create(&stretcher_thread, NULL, stretcher_loop, NULL);

    jack_set_process_callback(client, process_audio, 0);
    jack_on_shutdown(client, jack_shutdown, 0);
    jack_set_port_registration_callback(client, jack_port_registration_cb, 0);

    input_port_1 =
        jack_port_register(client, "capture_1", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    input_port_2 =
        jack_port_register(client, "capture_2", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    output_port_1 =
        jack_port_register(client, "playback_1", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    output_port_2 =
        jack_port_register(client, "playback_2", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);

    if (jack_activate(client)) {
        log_and_notify(0, "Cannot activate JACK client\n");
        return 1;
    }

    log_and_notify(0, "Audio Engine Live: CachyOS RT thread active.\n");

    patch_playback_ports(target_playback_device);
    return patch_audio_ports(target_audio_device);
}

int reboot_audio_engine(const char *target_audio_device, const char *target_playback_device,
                        float input_gain_multiplier) {
    if (client) {
        jack_deactivate(client);
        jack_client_close(client);
        client = NULL;
    }

    jack_status_t status;
    client = jack_client_open("QJams_Engine", JackNullOption, &status);
    if (!client)
        return -1;

    set_input_gain(input_gain_multiplier);
    jack_nframes_t current_rate = jack_get_sample_rate(client);
    atomic_store_explicit(&get_transport_state()->active_sample_rate, (int)current_rate,
                          memory_order_release);

    jack_set_sample_rate_callback(client, jack_sample_rate_cb, 0);
    jack_set_process_callback(client, process_audio, 0);
    jack_on_shutdown(client, jack_shutdown, 0);
    jack_set_port_registration_callback(client, jack_port_registration_cb, 0);

    input_port_1 =
        jack_port_register(client, "capture_1", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    input_port_2 =
        jack_port_register(client, "capture_2", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    output_port_1 =
        jack_port_register(client, "playback_1", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    output_port_2 =
        jack_port_register(client, "playback_2", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);

    if (jack_activate(client)) {
        log_and_notify(0, "[HW-REBOOT] Cannot activate JACK client on reboot\n");
        return -1;
    }

    log_and_notify(0, "[HW-REBOOT] Audio Engine Rebooted: CachyOS RT thread active.\n");

    patch_playback_ports(target_playback_device);
    return patch_audio_ports(target_audio_device);
}

void shutdown_audio_engine(void) {
    atomic_store_explicit(&engine_is_shutting_down, true, memory_order_release);

    if (atomic_exchange_explicit(&stretcher_keep_running, false, memory_order_release)) {
        pthread_join(stretcher_thread, NULL);
    }

    if (client) {
        jack_client_close(client);
        client = NULL;
    }
    if (rt_rb_state)
        rubberband_delete(rt_rb_state);

    if (audio_queue.buffer) {
        free(audio_queue.buffer);
        audio_queue.buffer = NULL;
        audio_queue.capacity = 0;
    }
    if (video_queue.buffer) {
        free(video_queue.buffer);
        video_queue.buffer = NULL;
        video_queue.capacity = 0;
    }
    if (stretch_queue.buffer) {
        free(stretch_queue.buffer);
        stretch_queue.buffer = NULL;
        stretch_queue.capacity = 0;
    }
    if (cmd_queue.buffer) {
        free(cmd_queue.buffer);
        cmd_queue.buffer = NULL;
        cmd_queue.capacity = 0;
    }

    if (event_pipe[0] != -1) {
        close(event_pipe[0]);
        event_pipe[0] = -1;
    }
    if (event_pipe[1] != -1) {
        close(event_pipe[1]);
        event_pipe[1] = -1;
    }

    for (int i = 0; i < QJ_MAX_TRACKS; i++) {
        if (master_tracks[i].active_buffer) {
            free_audio_buffer(master_tracks[i].active_buffer, master_tracks[i].allocated_frames);
            master_tracks[i].active_buffer = NULL;
        }
        if (master_tracks[i].parity_map) {
            free(master_tracks[i].parity_map);
            master_tracks[i].parity_map = NULL;
        }
        if (clipboard_buffers[i]) {
            free(clipboard_buffers[i]);
            clipboard_buffers[i] = NULL;
        }
        if (clipboard_parity[i]) {
            free(clipboard_parity[i]);
            clipboard_parity[i] = NULL;
        }
    }

    if (global_undo_ledger.states) {
        for (int i = 0; i < global_undo_ledger.max_capacity; i++) {
            if (global_undo_ledger.states[i].buffer) {
                free(global_undo_ledger.states[i].buffer);
            }
            if (global_undo_ledger.states[i].parity_buffer) {
                free(global_undo_ledger.states[i].parity_buffer);
            }
        }
        free(global_undo_ledger.states);
    }
    global_undo_ledger.states = NULL;
    global_undo_ledger.max_capacity = 0;

    clipboard_frames = 0;

    atomic_store_explicit(&vu_peak_input_l, 0.0f, memory_order_relaxed);
    atomic_store_explicit(&vu_peak_input_r, 0.0f, memory_order_relaxed);

    log_and_notify(0, "Audio engine cleanly shut down.\n");
}

void get_engine_telemetry(QJEngineTelemetry *out_stats) {
    if (!out_stats)
        return;
    out_stats->dropped_frames =
        atomic_load_explicit(&rt_dropped_audio_frames, memory_order_relaxed);
    out_stats->sample_rate =
        atomic_load_explicit(&get_transport_state()->active_sample_rate, memory_order_relaxed);
    out_stats->ledger_capacity = 0;
    out_stats->ledger_undos = 0;
    out_stats->ledger_redos = 0;
    if (try_acquire_track_read()) {
        out_stats->ledger_capacity = global_undo_ledger.max_capacity;
        if (global_undo_ledger.states && global_undo_ledger.max_capacity > 0) {
            uint32_t last_id = 0;
            for (int i = 0; i < global_undo_ledger.head; i++) {
                int idx = (global_undo_ledger.tail + i) % global_undo_ledger.max_capacity;
                if (global_undo_ledger.states[idx].transaction_id != last_id) {
                    out_stats->ledger_undos++;
                    last_id = global_undo_ledger.states[idx].transaction_id;
                }
            }
            last_id = 0;
            for (int i = global_undo_ledger.head; i < global_undo_ledger.count; i++) {
                int idx = (global_undo_ledger.tail + i) % global_undo_ledger.max_capacity;
                if (global_undo_ledger.states[idx].transaction_id != last_id) {
                    out_stats->ledger_redos++;
                    last_id = global_undo_ledger.states[idx].transaction_id;
                }
            }
        }
        release_track_read();
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

/**
 * @brief Scans all active tracks backward to locate the final frame above the analog noise floor.
 * Safely aborts if a memory detach barrier is requested by another thread.
 * @return The 1-based index of the last audible frame, or 0 if silent/aborted.
 */
size_t get_last_audible_frame(void) {
    if (!try_acquire_track_read())
        return 0;

    size_t local_pristine = atomic_load_explicit(&pristine_frames, memory_order_acquire);
    if (local_pristine == 0) {
        release_track_read();
        return 0;
    }
    size_t last_frame = 0;
    float noise_floor = 0.00015f;

    for (int t = 0; t < QJ_MAX_TRACKS; t++) {
        if (atomic_load_explicit(&request_track_free, memory_order_acquire)) {
            release_track_read();
            return 0;
        }
        float *buf = master_tracks[t].active_buffer;
        if (!buf || !atomic_load_explicit(&master_tracks[t].has_audio, memory_order_acquire))
            continue;

        size_t limit = (local_pristine <= master_tracks[t].allocated_frames)
                           ? local_pristine
                           : master_tracks[t].allocated_frames;
        for (size_t f = limit; f > last_frame; f--) {
            if ((f & 0xFFFF) == 0 &&
                atomic_load_explicit(&request_track_free, memory_order_relaxed)) {
                release_track_read();
                return 0;
            }
            if (fabsf(buf[(f - 1) * 2]) > noise_floor ||
                fabsf(buf[(f - 1) * 2 + 1]) > noise_floor) {
                last_frame = f;
                break;
            }
        }
    }
    release_track_read();
    return last_frame;
}

/**
 * @brief Scans all active tracks backward to locate the final frame of the last added recording.
 * Ignores the base imported backing track span on the timebase track unless overdubbed,
 * falling back to get_last_audible_frame() if no separate recordings exist.
 * @return The 1-based index of the last recorded frame, or 0 if silent/aborted.
 */
size_t get_last_recorded_frame(void) {
    if (!try_acquire_track_read())
        return 0;

    size_t local_pristine = atomic_load_explicit(&pristine_frames, memory_order_acquire);
    if (local_pristine == 0) {
        release_track_read();
        return 0;
    }
    size_t last_frame = 0;
    int current_tb = atomic_load_explicit(&timebase_track, memory_order_acquire);

    for (int t = 0; t < QJ_MAX_TRACKS; t++) {
        if (atomic_load_explicit(&request_track_free, memory_order_acquire)) {
            release_track_read();
            return 0;
        }
        uint8_t *pmap = master_tracks[t].parity_map;
        if (!pmap || !atomic_load_explicit(&master_tracks[t].has_audio, memory_order_acquire))
            continue;

        size_t limit = (local_pristine <= master_tracks[t].allocated_frames)
                           ? local_pristine
                           : master_tracks[t].allocated_frames;
        if (limit == 0)
            continue;

        bool is_full_base_track = (t == current_tb && pmap[limit - 1] == 1);

        for (size_t f = limit; f > last_frame; f--) {
            if ((f & 0xFFFF) == 0 &&
                atomic_load_explicit(&request_track_free, memory_order_relaxed)) {
                release_track_read();
                return 0;
            }
            uint8_t p = pmap[f - 1];
            if (is_full_base_track) {
                if (p > 1) {
                    last_frame = f;
                    break;
                }
            } else {
                if (p != 0) {
                    last_frame = f;
                    break;
                }
            }
        }
    }

    release_track_read();

    if (last_frame == 0) {
        return get_last_audible_frame();
    }
    return last_frame;
}

static void audible_scan_thread(GTask *task, gpointer source_object, gpointer task_data,
                                GCancellable *cancellable) {
    (void)source_object;
    (void)task_data;
    (void)cancellable;
    if (!atomic_load_explicit(&request_track_free, memory_order_acquire)) {
        size_t last = get_last_audible_frame();
        if (!atomic_load_explicit(&request_track_free, memory_order_acquire)) {
            atomic_store_explicit(&cached_audible_frames, last, memory_order_release);
        }
    }
    atomic_store_explicit(&is_scanning_audible, false, memory_order_release);
    g_task_return_boolean(task, TRUE);
}

/**
 * @brief Dispatches a non-blocking background GTask to update cached_audible_frames.
 */
void trigger_audible_scan_async(void) {
    if (atomic_load_explicit(&request_track_free, memory_order_acquire))
        return;
    if (!atomic_exchange_explicit(&is_scanning_audible, true, memory_order_acquire)) {
        GTask *task = g_task_new(NULL, NULL, NULL, NULL);
        g_task_run_in_thread(task, audible_scan_thread);
        g_object_unref(task);
    }
}
