#include "audio_nle.h"
#include "audio_core.h"
#include "audio_internal.h"
#include "audio_memory.h"
#include "config.h"
#include "media_io.h"
#include "ringbuffer.h"
#include "ui_globals.h"
#include "ui_multitrack.h"
#include "video_engine.h"
#include <gtk/gtk.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern _Atomic bool stretcher_flush_request;

void internal_backup_masked_tracks(uint32_t mask) {
    current_transaction_id++;
    for (int i = 0; i < QJ_MAX_TRACKS; i++) {
        if ((mask & (1 << i)) && master_tracks[i].active_buffer &&
            atomic_load_explicit(&master_tracks[i].has_audio, memory_order_acquire)) {
            push_undo_state(i, current_transaction_id);
        }
    }
}

void apply_nle_cut_math(int track_idx, size_t start, size_t end) {
    size_t cut_len = end - start;
    size_t remaining = pristine_frames - end;
    int rate =
        atomic_load_explicit(&get_transport_state()->active_sample_rate, memory_order_acquire);
    size_t fade_len = (size_t)((rate > 0 ? rate : 48000) * 0.005);
    if (fade_len > cut_len)
        fade_len = cut_len;

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
        memmove(&master_tracks[track_idx].active_buffer[start * 2],
                &master_tracks[track_idx].active_buffer[end * 2], remaining * 2 * sizeof(float));

        if (master_tracks[track_idx].parity_map) {
            memmove(&master_tracks[track_idx].parity_map[start],
                    &master_tracks[track_idx].parity_map[end], remaining * sizeof(uint8_t));

            if (start > 0) {
                uint8_t left_color = master_tracks[track_idx].parity_map[start - 1];
                uint8_t right_color = master_tracks[track_idx].parity_map[start];
                if (left_color != 0 && right_color != 0 && left_color == right_color) {
                    for (size_t f = start; f < start + remaining; f++) {
                        if (master_tracks[track_idx].parity_map[f] != 0) {
                            master_tracks[track_idx].parity_map[f] =
                                (master_tracks[track_idx].parity_map[f] == 1) ? 2 : 1;
                        }
                    }
                }
            }
        }
    }
    memset(&master_tracks[track_idx].active_buffer[(pristine_frames - cut_len) * 2], 0,
           cut_len * 2 * sizeof(float));
    if (master_tracks[track_idx].parity_map) {
        memset(&master_tracks[track_idx].parity_map[pristine_frames - cut_len], 0,
               cut_len * sizeof(uint8_t));
    }
}

void apply_nle_clear_math(int track_idx, size_t start, size_t end) {
    size_t clear_len = end - start;
    int rate =
        atomic_load_explicit(&get_transport_state()->active_sample_rate, memory_order_acquire);
    size_t fade_len = (size_t)((rate > 0 ? rate : 48000) * 0.005);
    if (fade_len > clear_len / 2)
        fade_len = clear_len / 2;

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
    if (master_tracks[track_idx].parity_map) {
        memset(&master_tracks[track_idx].parity_map[start], 0, clear_len * sizeof(uint8_t));
    }
}

/**
 * @brief Re-evaluates whether a track still contains any recorded/imported audio after a
 *        destructive NLE cut or delete, updating has_audio and active_track_count.
 *        Must be called while the RT thread is detached.
 * @param track_idx Target track index [0 .. QJ_MAX_TRACKS - 1].
 */
static void update_track_has_audio_after_edit(int track_idx) {
    if (track_idx < 0 || track_idx >= QJ_MAX_TRACKS || !master_tracks[track_idx].active_buffer)
        return;

    size_t limit = (pristine_frames <= master_tracks[track_idx].allocated_frames)
                       ? pristine_frames
                       : master_tracks[track_idx].allocated_frames;
    bool still_has_audio = false;
    uint8_t *pmap = master_tracks[track_idx].parity_map;

    if (pmap) {
        for (size_t f = 0; f < limit; f++) {
            if (pmap[f] != 0) {
                still_has_audio = true;
                break;
            }
        }
    }
    if (!still_has_audio) {
        const float *buf = master_tracks[track_idx].active_buffer;
        for (size_t f = 0; f < limit; f++) {
            if (fabsf(buf[f * 2]) > 0.00015f || fabsf(buf[f * 2 + 1]) > 0.00015f) {
                still_has_audio = true;
                break;
            }
        }
    }

    atomic_store_explicit(&master_tracks[track_idx].has_audio, still_has_audio,
                          memory_order_release);
}

/**
 * @brief Recomputes active_track_count after destructive NLE edits so empty trailing tracks
 *        do not keep transport playback erroneously enabled.
 */
static void recompute_active_track_count_locked(void) {
    int highest_active = 0;
    for (int i = QJ_MAX_TRACKS - 1; i >= 0; i--) {
        if (atomic_load_explicit(&master_tracks[i].has_audio, memory_order_acquire)) {
            highest_active = i;
            break;
        }
    }
    int cur_rec = atomic_load_explicit(&current_recording_track, memory_order_acquire);
    int new_active = (cur_rec > highest_active ? cur_rec : highest_active) + 1;
    atomic_store_explicit(&active_track_count, new_active, memory_order_release);
}

/**
 * @brief Copies the active loop selection of all selected tracks into the NLE clipboard.
 * Normalizes stretched loop coordinates to 1.0x pristine sample indices.
 */
void copy_selection(void) {
    if (!atomic_load_explicit(&loop_active, memory_order_acquire))
        return;
    size_t l_start = atomic_load_explicit(&loop_start_frame, memory_order_acquire);
    size_t l_end = atomic_load_explicit(&loop_end_frame, memory_order_acquire);
    float current_speed =
        atomic_load_explicit(&get_transport_state()->playback_speed, memory_order_relaxed);
    if (current_speed <= 0.0f)
        current_speed = 1.0f;

    size_t start = (size_t)((double)l_start * (double)current_speed);
    size_t end = (size_t)((double)l_end * (double)current_speed);
    if (start >= end || end > pristine_frames)
        return;

    size_t copy_len = end - start;
    if (!await_rt_thread_detach())
        return;

    if (end > pristine_frames) {
        resume_rt_thread();
        return;
    }

    uint32_t mask = atomic_load_explicit(&selected_tracks_mask, memory_order_acquire);
    bool mem_success = true;

    for (int i = 0; i < QJ_MAX_TRACKS; i++) {
        if (mask & (1 << i)) {
            if (master_tracks[i].active_buffer) {
                float *new_buf = realloc(clipboard_buffers[i], copy_len * 2 * sizeof(float));
                if (new_buf)
                    clipboard_buffers[i] = new_buf;

                uint8_t *new_p_buf = realloc(clipboard_parity[i], copy_len * sizeof(uint8_t));
                if (new_p_buf)
                    clipboard_parity[i] = new_p_buf;

                if (new_buf && new_p_buf) {
                    memcpy(clipboard_buffers[i], &master_tracks[i].active_buffer[start * 2],
                           copy_len * 2 * sizeof(float));
                    if (master_tracks[i].parity_map) {
                        memcpy(clipboard_parity[i], &master_tracks[i].parity_map[start],
                               copy_len * sizeof(uint8_t));
                    } else {
                        memset(clipboard_parity[i], 0, copy_len * sizeof(uint8_t));
                    }
                } else {
                    mem_success = false;
                }
            }
        } else {
            if (clipboard_buffers[i]) {
                free(clipboard_buffers[i]);
                clipboard_buffers[i] = NULL;
            }
            if (clipboard_parity[i]) {
                free(clipboard_parity[i]);
                clipboard_parity[i] = NULL;
            }
        }
    }

    if (mem_success) {
        clipboard_frames = copy_len;
    } else {
        for (int i = 0; i < QJ_MAX_TRACKS; i++) {
            if (clipboard_buffers[i]) {
                free(clipboard_buffers[i]);
                clipboard_buffers[i] = NULL;
            }
            if (clipboard_parity[i]) {
                free(clipboard_parity[i]);
                clipboard_parity[i] = NULL;
            }
        }
        clipboard_frames = 0;
        log_and_notify(
            1,
            "<span foreground='#ff4444'><b>Status: Insufficient RAM to copy selection</b></span>");
    }
    resume_rt_thread();
    clear_loop_points();
}

/**
 * @brief Cuts the active loop selection from selected tracks into the clipboard with a 5ms V-fade.
 */
void cut_selection(void) {
    if (!atomic_load_explicit(&loop_active, memory_order_acquire))
        return;
    size_t l_start = atomic_load_explicit(&loop_start_frame, memory_order_acquire);
    size_t l_end = atomic_load_explicit(&loop_end_frame, memory_order_acquire);
    float current_speed =
        atomic_load_explicit(&get_transport_state()->playback_speed, memory_order_relaxed);
    if (current_speed <= 0.0f)
        current_speed = 1.0f;

    size_t start = (size_t)((double)l_start * (double)current_speed);
    size_t end = (size_t)((double)l_end * (double)current_speed);
    if (start >= end || end > pristine_frames)
        return;

    if (!await_rt_thread_detach())
        return;

    if (end > pristine_frames) {
        resume_rt_thread();
        return;
    }

    uint32_t mask = atomic_load_explicit(&selected_tracks_mask, memory_order_acquire);
    internal_backup_masked_tracks(mask);

    size_t copy_len = end - start;
    bool mem_success = true;

    for (int i = 0; i < QJ_MAX_TRACKS; i++) {
        if ((mask & (1 << i)) && master_tracks[i].active_buffer) {
            float *new_buf = realloc(clipboard_buffers[i], copy_len * 2 * sizeof(float));
            if (new_buf)
                clipboard_buffers[i] = new_buf;

            uint8_t *new_p_buf = realloc(clipboard_parity[i], copy_len * sizeof(uint8_t));
            if (new_p_buf)
                clipboard_parity[i] = new_p_buf;

            if (new_buf && new_p_buf) {
                memcpy(clipboard_buffers[i], &master_tracks[i].active_buffer[start * 2],
                       copy_len * 2 * sizeof(float));
                if (master_tracks[i].parity_map) {
                    memcpy(clipboard_parity[i], &master_tracks[i].parity_map[start],
                           copy_len * sizeof(uint8_t));
                } else {
                    memset(clipboard_parity[i], 0, copy_len * sizeof(uint8_t));
                }
            } else {
                mem_success = false;
            }
            apply_nle_cut_math(i, start, end);
            update_track_has_audio_after_edit(i);
        } else {
            if (clipboard_buffers[i]) {
                free(clipboard_buffers[i]);
                clipboard_buffers[i] = NULL;
            }
            if (clipboard_parity[i]) {
                free(clipboard_parity[i]);
                clipboard_parity[i] = NULL;
            }
        }
    }

    recompute_active_track_count_locked();

    if (mem_success) {
        clipboard_frames = copy_len;
    } else {
        for (int i = 0; i < QJ_MAX_TRACKS; i++) {
            if (clipboard_buffers[i]) {
                free(clipboard_buffers[i]);
                clipboard_buffers[i] = NULL;
            }
            if (clipboard_parity[i]) {
                free(clipboard_parity[i]);
                clipboard_parity[i] = NULL;
            }
        }
        clipboard_frames = 0;
        log_and_notify(
            1,
            "<span foreground='#ff4444'><b>Status: Insufficient RAM to cut selection</b></span>");
    }

    atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);
    atomic_store_explicit(&get_transport_state()->playback_pos, l_start, memory_order_release);
    mark_session_dirty();
    resume_rt_thread();
    clear_loop_points();
}

/**
 * @brief Pastes clipboard contents onto the currently selected tracks at the playhead position.
 */
void paste_selection(void) {
    if (clipboard_frames == 0)
        return;
    size_t raw_play_pos =
        atomic_load_explicit(&get_transport_state()->playback_pos, memory_order_acquire);
    float current_speed =
        atomic_load_explicit(&get_transport_state()->playback_speed, memory_order_relaxed);
    if (current_speed <= 0.0f)
        current_speed = 1.0f;

    size_t current_pos = (size_t)((double)raw_play_pos * (double)current_speed);

    if (current_pos + clipboard_frames > pristine_frames) {
        int rate =
            atomic_load_explicit(&get_transport_state()->active_sample_rate, memory_order_acquire);
            int required_seconds =
            (int)((current_pos + clipboard_frames) / (rate > 0 ? rate : 48000)) + 1;
            int resize_status = resize_loop_canvas_seconds(required_seconds);
            if (resize_status != 0) {
                if (resize_status == -2) {
                    log_and_notify(1, "<span foreground='#ff4444'><b>Status: Insufficient RAM to "
                    "expand canvas for paste</b></span>");
                }
                return;
            }
    }

    if (!await_rt_thread_detach())
        return;

    if (current_pos + clipboard_frames > pristine_frames) {
        resume_rt_thread();
        return;
    }

    uint32_t mask = atomic_load_explicit(&selected_tracks_mask, memory_order_acquire);
    internal_backup_masked_tracks(mask);

    for (int i = 0; i < QJ_MAX_TRACKS; i++) {
        if ((mask & (1 << i)) && clipboard_buffers[i] && master_tracks[i].active_buffer) {
            memcpy(&master_tracks[i].active_buffer[current_pos * 2], clipboard_buffers[i],
                   clipboard_frames * 2 * sizeof(float));

            if (master_tracks[i].parity_map && clipboard_parity[i]) {
                memcpy(&master_tracks[i].parity_map[current_pos], clipboard_parity[i],
                       clipboard_frames * sizeof(uint8_t));

                if (current_pos > 0) {
                    uint8_t left_color = master_tracks[i].parity_map[current_pos - 1];
                    uint8_t right_color = master_tracks[i].parity_map[current_pos];
                    if (left_color != 0 && right_color != 0 && left_color == right_color) {
                        for (size_t f = 0; f < clipboard_frames; f++) {
                            if (master_tracks[i].parity_map[current_pos + f] != 0) {
                                master_tracks[i].parity_map[current_pos + f] =
                                    (master_tracks[i].parity_map[current_pos + f] == 1) ? 2 : 1;
                            }
                        }
                    }
                }
            }

            atomic_store_explicit(&master_tracks[i].has_audio, true, memory_order_release);

            int active = atomic_load_explicit(&active_track_count, memory_order_acquire);
            if (i >= active)
                atomic_store_explicit(&active_track_count, i + 1, memory_order_release);
        }
    }

    atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);
    evaluate_session_dirty_state();
    resume_rt_thread();
}

/**
 * @brief Pastes all populated clipboard tracks globally at the playhead and advances the playhead.
 */
void global_paste_selection(void) {
    if (clipboard_frames == 0)
        return;
    size_t raw_play_pos =
        atomic_load_explicit(&get_transport_state()->playback_pos, memory_order_acquire);
    float current_speed =
        atomic_load_explicit(&get_transport_state()->playback_speed, memory_order_relaxed);
    if (current_speed <= 0.0f)
        current_speed = 1.0f;

    size_t current_pos = (size_t)((double)raw_play_pos * (double)current_speed);

    if (current_pos + clipboard_frames > pristine_frames) {
        int rate =
            atomic_load_explicit(&get_transport_state()->active_sample_rate, memory_order_acquire);
        int required_seconds =
            (int)((current_pos + clipboard_frames) / (rate > 0 ? rate : 48000)) + 1;
        int resize_status = resize_loop_canvas_seconds(required_seconds);
        if (resize_status != 0) {
            if (resize_status == -2) {
                log_and_notify(1, "<span foreground='#ff4444'><b>Status: Insufficient RAM to "
                                  "expand canvas for paste</b></span>");
            }
            return;
        }
    }

    if (!await_rt_thread_detach())
        return;

    if (current_pos + clipboard_frames > pristine_frames) {
        resume_rt_thread();
        return;
    }

    uint32_t mask = 0xFFFFFFFF;
    internal_backup_masked_tracks(mask);

    for (int i = 0; i < QJ_MAX_TRACKS; i++) {
        if (clipboard_buffers[i] && master_tracks[i].active_buffer) {
            memcpy(&master_tracks[i].active_buffer[current_pos * 2], clipboard_buffers[i],
                   clipboard_frames * 2 * sizeof(float));

            if (master_tracks[i].parity_map && clipboard_parity[i]) {
                memcpy(&master_tracks[i].parity_map[current_pos], clipboard_parity[i],
                       clipboard_frames * sizeof(uint8_t));

                if (current_pos > 0) {
                    uint8_t left_color = master_tracks[i].parity_map[current_pos - 1];
                    uint8_t right_color = master_tracks[i].parity_map[current_pos];
                    if (left_color != 0 && right_color != 0 && left_color == right_color) {
                        for (size_t f = 0; f < clipboard_frames; f++) {
                            if (master_tracks[i].parity_map[current_pos + f] != 0) {
                                master_tracks[i].parity_map[current_pos + f] =
                                    (master_tracks[i].parity_map[current_pos + f] == 1) ? 2 : 1;
                            }
                        }
                    }
                }
            }

            atomic_store_explicit(&master_tracks[i].has_audio, true, memory_order_release);

            int active = atomic_load_explicit(&active_track_count, memory_order_acquire);
            if (i >= active)
                atomic_store_explicit(&active_track_count, i + 1, memory_order_release);
        }
    }

    atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);

    size_t next_play_pos =
        (size_t)((double)(current_pos + clipboard_frames) / (double)current_speed);
    atomic_store_explicit(&get_transport_state()->playback_pos, next_play_pos,
                          memory_order_release);
    evaluate_session_dirty_state();
    resume_rt_thread();
}

/**
 * @brief Clears (silences) the active loop selection on selected tracks without shifting time.
 */
void delete_selection(void) {
    if (!atomic_load_explicit(&loop_active, memory_order_acquire))
        return;
    size_t l_start = atomic_load_explicit(&loop_start_frame, memory_order_acquire);
    size_t l_end = atomic_load_explicit(&loop_end_frame, memory_order_acquire);
    float current_speed =
        atomic_load_explicit(&get_transport_state()->playback_speed, memory_order_relaxed);
    if (current_speed <= 0.0f)
        current_speed = 1.0f;

    size_t start = (size_t)((double)l_start * (double)current_speed);
    size_t end = (size_t)((double)l_end * (double)current_speed);
    if (start >= end || end > pristine_frames)
        return;

    if (!await_rt_thread_detach())
        return;

    if (end > pristine_frames) {
        resume_rt_thread();
        return;
    }

    uint32_t mask = atomic_load_explicit(&selected_tracks_mask, memory_order_acquire);
    internal_backup_masked_tracks(mask);

    for (int i = 0; i < QJ_MAX_TRACKS; i++) {
        if ((mask & (1 << i)) && master_tracks[i].active_buffer) {
            apply_nle_clear_math(i, start, end);
            update_track_has_audio_after_edit(i);
        }
    }

    recompute_active_track_count_locked();

    atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);
    mark_session_dirty();
    resume_rt_thread();
    clear_loop_points();
}

/**
 * @brief Performs a global ripple delete across all tracks, shrinking total canvas duration.
 */
void global_ripple_delete_selection(void) {
    if (!atomic_load_explicit(&loop_active, memory_order_acquire))
        return;
    size_t l_start = atomic_load_explicit(&loop_start_frame, memory_order_acquire);
    size_t l_end = atomic_load_explicit(&loop_end_frame, memory_order_acquire);
    float current_speed =
        atomic_load_explicit(&get_transport_state()->playback_speed, memory_order_relaxed);
    if (current_speed <= 0.0f)
        current_speed = 1.0f;

    size_t start = (size_t)((double)l_start * (double)current_speed);
    size_t end = (size_t)((double)l_end * (double)current_speed);
    if (start >= end || end > pristine_frames)
        return;

    if (!await_rt_thread_detach())
        return;

    if (end > pristine_frames) {
        resume_rt_thread();
        return;
    }

    uint32_t mask = 0xFFFFFFFF;
    internal_backup_masked_tracks(mask);

    for (int i = 0; i < QJ_MAX_TRACKS; i++) {
        if (master_tracks[i].active_buffer &&
            atomic_load_explicit(&master_tracks[i].has_audio, memory_order_acquire)) {
            apply_nle_cut_math(i, start, end);
        }
    }

    size_t cut_len = end - start;
    atomic_store_explicit(&pristine_frames, pristine_frames - cut_len, memory_order_release);

    for (int i = 0; i < QJ_MAX_TRACKS; i++) {
        if (master_tracks[i].active_buffer &&
            atomic_load_explicit(&master_tracks[i].has_audio, memory_order_acquire)) {
            update_track_has_audio_after_edit(i);
        }
    }
    recompute_active_track_count_locked();

    size_t new_bt_frames = (size_t)((double)pristine_frames / (double)current_speed);
    atomic_store_explicit(&get_transport_state()->backing_track_frames, new_bt_frames,
                          memory_order_release);

    if (video_end_frame > new_bt_frames)
        video_end_frame = new_bt_frames;
    if (video_start_frame > new_bt_frames)
        video_start_frame = new_bt_frames;

    atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);
    atomic_store_explicit(&get_transport_state()->playback_pos, l_start, memory_order_release);
    mark_session_dirty();
    resume_rt_thread();
    clear_loop_points();
}

/**
 * @brief Applies a context-aware fade-in, fade-out, or V-fade to the active selection.
 */
void apply_smart_fade_selection(void) {
    if (!atomic_load_explicit(&loop_active, memory_order_acquire))
        return;
    size_t l_start = atomic_load_explicit(&loop_start_frame, memory_order_acquire);
    size_t l_end = atomic_load_explicit(&loop_end_frame, memory_order_acquire);

    float current_speed =
        atomic_load_explicit(&get_transport_state()->playback_speed, memory_order_relaxed);
    if (current_speed <= 0.0f)
        current_speed = 1.0f;
    size_t start = (size_t)((double)l_start * (double)current_speed);
    size_t end = (size_t)((double)l_end * (double)current_speed);

    if (start >= end || end > pristine_frames)
        return;

    size_t sel_len = end - start;
    if (sel_len < 4) {
        clear_loop_points();
        return;
    }

    if (!await_rt_thread_detach())
        return;

    if (end > pristine_frames) {
        resume_rt_thread();
        return;
    }

    uint32_t mask = atomic_load_explicit(&selected_tracks_mask, memory_order_acquire);
    internal_backup_masked_tracks(mask);

    for (int i = 0; i < QJ_MAX_TRACKS; i++) {
        if ((mask & (1 << i)) && master_tracks[i].active_buffer) {
            bool touches_start =
                (start == 0 || master_tracks[i].active_buffer[(start - 1) * 2] == 0.0f);
            bool touches_end =
                (end >= pristine_frames || master_tracks[i].active_buffer[end * 2] == 0.0f);

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

    atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);
    mark_session_dirty();
    resume_rt_thread();
    clear_loop_points();
}

/**
 * @brief Performs a ripple crossfade (blend) over the selected region and shrinks canvas bounds.
 */
void apply_blend_fade_selection(void) {
    if (!atomic_load_explicit(&loop_active, memory_order_acquire))
        return;
    size_t l_start = atomic_load_explicit(&loop_start_frame, memory_order_acquire);
    size_t l_end = atomic_load_explicit(&loop_end_frame, memory_order_acquire);

    float current_speed =
        atomic_load_explicit(&get_transport_state()->playback_speed, memory_order_relaxed);
    if (current_speed <= 0.0f)
        current_speed = 1.0f;
    size_t start = (size_t)((double)l_start * (double)current_speed);
    size_t end = (size_t)((double)l_end * (double)current_speed);

    if (start >= end || end > pristine_frames)
        return;

    size_t sel_len = end - start;
    if (sel_len < 4) {
        clear_loop_points();
        return;
    }

    if (!await_rt_thread_detach())
        return;

    if (end > pristine_frames) {
        resume_rt_thread();
        return;
    }

    uint32_t mask = atomic_load_explicit(&selected_tracks_mask, memory_order_acquire);
    internal_backup_masked_tracks(mask);

    size_t half_len = sel_len / 2;
    size_t shift_len = sel_len - half_len;
    size_t remaining = pristine_frames - end;

    for (int i = 0; i < QJ_MAX_TRACKS; i++) {
        if ((mask & (1 << i)) && master_tracks[i].active_buffer) {
            for (size_t j = 0; j < half_len; j++) {
                float mult = 1.0f - ((float)j / (float)half_len);
                master_tracks[i].active_buffer[(start + j) * 2] *= mult;
                master_tracks[i].active_buffer[(start + j) * 2 + 1] *= mult;
            }
            for (size_t j = 0; j < half_len; j++) {
                float mult = ((float)j / (float)half_len);
                master_tracks[i].active_buffer[(start + j) * 2] +=
                    master_tracks[i].active_buffer[(start + shift_len + j) * 2] * mult;
                master_tracks[i].active_buffer[(start + j) * 2 + 1] +=
                    master_tracks[i].active_buffer[(start + shift_len + j) * 2 + 1] * mult;
            }
            if (remaining > 0) {
                memmove(&master_tracks[i].active_buffer[(start + half_len) * 2],
                        &master_tracks[i].active_buffer[end * 2], remaining * 2 * sizeof(float));

                if (master_tracks[i].parity_map) {
                    memmove(&master_tracks[i].parity_map[start + half_len],
                            &master_tracks[i].parity_map[end], remaining * sizeof(uint8_t));

                    if (start + half_len > 0) {
                        uint8_t left_color = master_tracks[i].parity_map[start + half_len - 1];
                        uint8_t right_color = master_tracks[i].parity_map[start + half_len];
                        if (left_color != 0 && right_color != 0 && left_color == right_color) {
                            for (size_t f = start + half_len; f < start + half_len + remaining;
                                 f++) {
                                if (master_tracks[i].parity_map[f] != 0) {
                                    master_tracks[i].parity_map[f] =
                                        (master_tracks[i].parity_map[f] == 1) ? 2 : 1;
                                }
                            }
                        }
                    }
                }
            }
            memset(&master_tracks[i].active_buffer[(pristine_frames - shift_len) * 2], 0,
                   shift_len * 2 * sizeof(float));
            if (master_tracks[i].parity_map) {
                memset(&master_tracks[i].parity_map[pristine_frames - shift_len], 0,
                       shift_len * sizeof(uint8_t));
            }
        }
    }

    atomic_store_explicit(&pristine_frames, pristine_frames - shift_len, memory_order_release);

    size_t new_bt_frames = (size_t)((double)pristine_frames / (double)current_speed);
    atomic_store_explicit(&get_transport_state()->backing_track_frames, new_bt_frames,
                          memory_order_release);

    if (video_end_frame > new_bt_frames)
        video_end_frame = new_bt_frames;
    if (video_start_frame > new_bt_frames)
        video_start_frame = new_bt_frames;

    atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);
    mark_session_dirty();
    resume_rt_thread();
    clear_loop_points();
}

/**
 * @brief Applies destructive gain scaling to the selected region (or full track) using zero-RAM
 * math undo.
 * @param multiplier Linear amplitude multiplier.
 */
void apply_gain_selection(float multiplier) {
    if (multiplier == 0.0f)
        multiplier = 0.000001f;
    else if (fabsf(multiplier) < 0.000001f)
        multiplier = (multiplier > 0) ? 0.000001f : -0.000001f;

    uint32_t mask = atomic_load_explicit(&selected_tracks_mask, memory_order_acquire);
    bool is_playing =
        atomic_load_explicit(&get_transport_state()->is_playing, memory_order_acquire);
    float current_speed =
        atomic_load_explicit(&get_transport_state()->playback_speed, memory_order_relaxed);
    if (current_speed <= 0.0f)
        current_speed = 1.0f;

    size_t start = 0;
    size_t end = pristine_frames;

    if (atomic_load_explicit(&loop_active, memory_order_acquire)) {
        size_t l_start = atomic_load_explicit(&loop_start_frame, memory_order_acquire);
        size_t l_end = atomic_load_explicit(&loop_end_frame, memory_order_acquire);
        size_t p_start = (size_t)((double)l_start * (double)current_speed);
        size_t p_end = (size_t)((double)l_end * (double)current_speed);
        if (p_start < p_end && p_end <= pristine_frames) {
            start = p_start;
            end = p_end;
        }
    }

    static gint64 last_gain_edit_time = 0;
    static uint32_t active_gain_transaction_id = 0;
    static uint32_t last_mask = 0;
    static size_t last_start = 0;
    static size_t last_end = 0;
    static bool last_was_playing = false;

    gint64 now = g_get_monotonic_time();
    bool is_continuous = (now - last_gain_edit_time < 1500000) && (mask == last_mask) &&
                         (start == last_start) && (end == last_end) &&
                         (is_playing == last_was_playing) &&
                         (global_undo_ledger.head == global_undo_ledger.count);

    if (!await_rt_thread_detach())
        return;

    if (end > pristine_frames) {
        end = pristine_frames;
    }
    if (start >= end) {
        resume_rt_thread();
        return;
    }

    if (!is_continuous) {
        current_transaction_id++;
        active_gain_transaction_id = current_transaction_id;

        for (int i = 0; i < QJ_MAX_TRACKS; i++) {
            if ((mask & (1 << i)) && master_tracks[i].active_buffer &&
                atomic_load_explicit(&master_tracks[i].has_audio, memory_order_acquire)) {
                push_math_undo_state(i, active_gain_transaction_id, multiplier, start, end);
            }
        }
    } else {
        for (int i = 1; i <= global_undo_ledger.max_capacity && i <= global_undo_ledger.count;
             i++) {
            int idx = (global_undo_ledger.tail + global_undo_ledger.count - i) %
                      global_undo_ledger.max_capacity;
            QJUndoState *state = &global_undo_ledger.states[idx];
            if (state->transaction_id == active_gain_transaction_id && state->edit_type == 1) {
                state->applied_multiplier *= multiplier;
            } else if (state->transaction_id != active_gain_transaction_id) {
                break;
            }
        }
    }

    bool any_updated = false;
    for (int i = 0; i < QJ_MAX_TRACKS; i++) {
        if ((mask & (1 << i)) && master_tracks[i].active_buffer &&
            atomic_load_explicit(&master_tracks[i].has_audio, memory_order_acquire)) {
            for (size_t f = start; f < end; f++) {
                master_tracks[i].active_buffer[f * 2] *= multiplier;
                master_tracks[i].active_buffer[f * 2 + 1] *= multiplier;
            }
            any_updated = true;
        }
    }

    if (any_updated) {
        atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);
    }

    mark_session_dirty();
    resume_rt_thread();

    last_gain_edit_time = g_get_monotonic_time();
    last_mask = mask;
    last_start = start;
    last_end = end;
    last_was_playing = is_playing;
}

/**
 * @brief Reverts the most recent NLE or recording transaction from the undo ledger.
 *        Pre-allocates all required swap buffers across the transaction before mutating
 *        state so multi-track transactions remain strictly atomic on RAM shortage.
 */
void undo_selection(void) {
    if (!await_rt_thread_detach())
        return;
    if (global_undo_ledger.head <= 0 || global_undo_ledger.count <= 0) {
        resume_rt_thread();
        return;
    }
    int last_idx =
        (global_undo_ledger.tail + global_undo_ledger.head - 1) % global_undo_ledger.max_capacity;
    uint32_t target_trans_id = global_undo_ledger.states[last_idx].transaction_id;
    size_t present_bounds =
        atomic_load_explicit(&get_transport_state()->backing_track_frames, memory_order_acquire);
    float current_speed =
        atomic_load_explicit(&get_transport_state()->playback_speed, memory_order_relaxed);
    if (current_speed <= 0.0f)
        current_speed = 1.0f;
    for (int h = global_undo_ledger.head; h > 0; h--) {
        int chk_idx = (global_undo_ledger.tail + h - 1) % global_undo_ledger.max_capacity;
        QJUndoState *chk = &global_undo_ledger.states[chk_idx];
        if (chk->transaction_id != target_trans_id)
            break;
        int trk = chk->track_idx;
        if (trk >= 0 && trk < QJ_MAX_TRACKS && master_tracks[trk].active_buffer &&
            chk->edit_type != 1 && chk->buffer) {
            size_t max_canvas =
                (present_bounds > chk->canvas_frames) ? present_bounds : chk->canvas_frames;
            size_t swap_frames = (size_t)((double)max_canvas * (double)current_speed);
            if (swap_frames > master_tracks[trk].allocated_frames) {
                swap_frames = master_tracks[trk].allocated_frames;
            }
            float *safe_buf = realloc(chk->buffer, swap_frames * 2 * sizeof(float));
            if (!safe_buf) {
                log_and_notify(1, "<span foreground='#ff4444'><b>Status: Critical RAM shortage "
                                  "during Undo. Operation aborted.</b></span>");
                resume_rt_thread();
                return;
            }
            chk->buffer = safe_buf;
            if (chk->parity_buffer) {
                uint8_t *safe_p_buf = realloc(chk->parity_buffer, swap_frames * sizeof(uint8_t));
                if (!safe_p_buf) {
                    log_and_notify(1, "<span foreground='#ff4444'><b>Status: Critical RAM shortage "
                                      "during Undo. Operation aborted.</b></span>");
                    resume_rt_thread();
                    return;
                }
                chk->parity_buffer = safe_p_buf;
            }
        }
    }
    bool stash_video = false;
    bool restore_video = false;
    if (video_creation_trans_id != 0 && target_trans_id == video_creation_trans_id) {
        stash_video = true;
    }
    if (video_deletion_trans_id != 0 && target_trans_id == video_deletion_trans_id) {
        restore_video = true;
    }
    size_t present_v_start = video_start_frame;
    size_t present_v_end = video_end_frame;
    int present_v_track_idx = video_track_index;
    bool globals_applied = false;
    while (global_undo_ledger.head > 0) {
        int current_idx = (global_undo_ledger.tail + global_undo_ledger.head - 1) %
                          global_undo_ledger.max_capacity;
        QJUndoState *state = &global_undo_ledger.states[current_idx];
        if (state->transaction_id != target_trans_id)
            break;
        int trk = state->track_idx;
        if (trk >= 0 && trk < QJ_MAX_TRACKS && master_tracks[trk].active_buffer) {
            if (state->edit_type == 1) {
                float inverse = 1.0f / state->applied_multiplier;
                size_t m_start = state->edit_start;
                size_t m_end = state->edit_end;
                if (m_end > master_tracks[trk].allocated_frames)
                    m_end = master_tracks[trk].allocated_frames;
                for (size_t f = m_start; f < m_end; f++) {
                    master_tracks[trk].active_buffer[f * 2] *= inverse;
                    master_tracks[trk].active_buffer[f * 2 + 1] *= inverse;
                }
            } else if (state->buffer) {
                size_t max_canvas =
                    (present_bounds > state->canvas_frames) ? present_bounds : state->canvas_frames;
                size_t swap_frames = (size_t)((double)max_canvas * (double)current_speed);
                if (swap_frames > master_tracks[trk].allocated_frames) {
                    swap_frames = master_tracks[trk].allocated_frames;
                }
                size_t floats_left = swap_frames * 2;
                float *ptr_a = master_tracks[trk].active_buffer;
                float *ptr_b = state->buffer;
                while (floats_left > 0) {
                    size_t to_swap = floats_left > 8192 ? 8192 : floats_left;
                    float temp_chunk[8192];
                    memcpy(temp_chunk, ptr_a, to_swap * sizeof(float));
                    memcpy(ptr_a, ptr_b, to_swap * sizeof(float));
                    memcpy(ptr_b, temp_chunk, to_swap * sizeof(float));
                    ptr_a += to_swap;
                    ptr_b += to_swap;
                    floats_left -= to_swap;
                }
                if (state->parity_buffer && master_tracks[trk].parity_map) {
                    size_t bytes_left = swap_frames;
                    uint8_t *p_ptr_a = master_tracks[trk].parity_map;
                    uint8_t *p_ptr_b = state->parity_buffer;
                    while (bytes_left > 0) {
                        size_t to_swap = bytes_left > 8192 ? 8192 : bytes_left;
                        uint8_t temp_chunk[8192];
                        memcpy(temp_chunk, p_ptr_a, to_swap * sizeof(uint8_t));
                        memcpy(p_ptr_a, p_ptr_b, to_swap * sizeof(uint8_t));
                        memcpy(p_ptr_b, temp_chunk, to_swap * sizeof(uint8_t));
                        p_ptr_a += to_swap;
                        p_ptr_b += to_swap;
                        bytes_left -= to_swap;
                    }
                }
            }
            bool tmp_audio =
                atomic_load_explicit(&master_tracks[trk].has_audio, memory_order_acquire);
            atomic_store_explicit(&master_tracks[trk].has_audio, state->has_audio,
                                  memory_order_release);
            state->has_audio = tmp_audio;
            float tmp_gain = atomic_load_explicit(&master_tracks[trk].gain, memory_order_acquire);
            atomic_store_explicit(&master_tracks[trk].gain, state->gain, memory_order_release);
            state->gain = tmp_gain;
            bool tmp_solo =
                atomic_load_explicit(&master_tracks[trk].is_soloed, memory_order_acquire);
            atomic_store_explicit(&master_tracks[trk].is_soloed, state->is_soloed,
                                  memory_order_release);
            state->is_soloed = tmp_solo;
            char tmp_name[64];
            get_multitrack_track_name(trk, tmp_name);
            set_multitrack_track_name(trk, state->name);
            strncpy(state->name, tmp_name, 63);
            state->name[63] = '\0';
            if (!globals_applied) {
                size_t restored_pristine =
                    (size_t)((double)state->canvas_frames * (double)current_speed);
                if (pristine_allocated_frames > 0 && restored_pristine > pristine_allocated_frames)
                    restored_pristine = pristine_allocated_frames;
                if (restored_pristine > 0) {
                    pristine_frames = restored_pristine;
                    size_t safe_bt_frames =
                        (size_t)((double)restored_pristine / (double)current_speed);
                    atomic_store_explicit(&get_transport_state()->backing_track_frames,
                                          safe_bt_frames, memory_order_release);
                }
                video_start_frame = state->v_start;
                video_end_frame = state->v_end;
                video_track_index = state->v_track_idx;
                globals_applied = true;
            }
            state->canvas_frames = present_bounds;
            state->v_start = present_v_start;
            state->v_end = present_v_end;
            state->v_track_idx = present_v_track_idx;
        }
        global_undo_ledger.head--;
    }
    int highest = 0;
    for (int i = 0; i < QJ_MAX_TRACKS; i++) {
        if (atomic_load_explicit(&master_tracks[i].has_audio, memory_order_acquire))
            highest = i;
    }
    int current_rec = atomic_load_explicit(&current_recording_track, memory_order_acquire);
    if (current_rec > highest &&
        !atomic_load_explicit(&master_tracks[current_rec].has_audio, memory_order_acquire)) {
        current_rec = highest;
        atomic_store_explicit(&current_recording_track, current_rec, memory_order_release);
        atomic_store_explicit(&selected_tracks_mask, (1U << current_rec), memory_order_release);
    }
    if (current_rec > highest)
        highest = current_rec;
    atomic_store_explicit(&active_track_count, highest + 1, memory_order_release);
    atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);
    evaluate_session_dirty_state();
    resume_rt_thread();
    if (stash_video) {
        abort_active_video_preview(true);
        force_clear_mkv_video_preview();
        if (media_has_video(ui_state.selected_track_path) &&
            access(ui_state.selected_track_path, F_OK) == 0) {
            video_track_index = atomic_load_explicit(&timebase_track, memory_order_acquire);
            launch_mkv_video_preview(ui_state.selected_track_path);
        }
    }
    if (restore_video) {
        if (strlen(stashed_raw_path) > 0) {
            restore_stashed_video_preview();
        } else if (media_has_video(ui_state.selected_track_path) &&
                   access(ui_state.selected_track_path, F_OK) == 0) {
            launch_mkv_video_preview(ui_state.selected_track_path);
        }
    }
}

/**
 * @brief Re-applies the next undone transaction from the undo ledger.
 *        Pre-allocates all required swap buffers across the transaction before mutating
 *        state so multi-track transactions remain strictly atomic on RAM shortage.
 */
void redo_selection(void) {
    if (!await_rt_thread_detach())
        return;
    if (global_undo_ledger.head >= global_undo_ledger.count) {
        resume_rt_thread();
        return;
    }
    int next_idx =
        (global_undo_ledger.tail + global_undo_ledger.head) % global_undo_ledger.max_capacity;
    uint32_t target_trans_id = global_undo_ledger.states[next_idx].transaction_id;
    size_t present_bounds =
        atomic_load_explicit(&get_transport_state()->backing_track_frames, memory_order_acquire);
    float current_speed =
        atomic_load_explicit(&get_transport_state()->playback_speed, memory_order_relaxed);
    if (current_speed <= 0.0f)
        current_speed = 1.0f;
    for (int h = global_undo_ledger.head; h < global_undo_ledger.count; h++) {
        int chk_idx = (global_undo_ledger.tail + h) % global_undo_ledger.max_capacity;
        QJUndoState *chk = &global_undo_ledger.states[chk_idx];
        if (chk->transaction_id != target_trans_id)
            break;
        int trk = chk->track_idx;
        if (trk >= 0 && trk < QJ_MAX_TRACKS && master_tracks[trk].active_buffer &&
            chk->edit_type != 1 && chk->buffer) {
            size_t max_canvas =
                (present_bounds > chk->canvas_frames) ? present_bounds : chk->canvas_frames;
            size_t swap_frames = (size_t)((double)max_canvas * (double)current_speed);
            if (swap_frames > master_tracks[trk].allocated_frames) {
                swap_frames = master_tracks[trk].allocated_frames;
            }
            float *safe_buf = realloc(chk->buffer, swap_frames * 2 * sizeof(float));
            if (!safe_buf) {
                log_and_notify(1, "<span foreground='#ff4444'><b>Status: Critical RAM shortage "
                                  "during Redo. Operation aborted.</b></span>");
                resume_rt_thread();
                return;
            }
            chk->buffer = safe_buf;
            if (chk->parity_buffer) {
                uint8_t *safe_p_buf = realloc(chk->parity_buffer, swap_frames * sizeof(uint8_t));
                if (!safe_p_buf) {
                    log_and_notify(1, "<span foreground='#ff4444'><b>Status: Critical RAM shortage "
                                      "during Redo. Operation aborted.</b></span>");
                    resume_rt_thread();
                    return;
                }
                chk->parity_buffer = safe_p_buf;
            }
        }
    }
    bool stash_video = false;
    bool restore_video = false;
    if (video_creation_trans_id != 0 && target_trans_id == video_creation_trans_id) {
        restore_video = true;
    }
    if (video_deletion_trans_id != 0 && target_trans_id == video_deletion_trans_id) {
        stash_video = true;
    }
    size_t present_v_start = video_start_frame;
    size_t present_v_end = video_end_frame;
    int present_v_track_idx = video_track_index;
    bool globals_applied = false;
    while (global_undo_ledger.head < global_undo_ledger.count) {
        int current_idx =
            (global_undo_ledger.tail + global_undo_ledger.head) % global_undo_ledger.max_capacity;
        QJUndoState *state = &global_undo_ledger.states[current_idx];
        if (state->transaction_id != target_trans_id)
            break;
        int trk = state->track_idx;
        if (trk >= 0 && trk < QJ_MAX_TRACKS && master_tracks[trk].active_buffer) {
            if (state->edit_type == 1) {
                float mult = state->applied_multiplier;
                size_t m_start = state->edit_start;
                size_t m_end = state->edit_end;
                if (m_end > master_tracks[trk].allocated_frames)
                    m_end = master_tracks[trk].allocated_frames;
                for (size_t f = m_start; f < m_end; f++) {
                    master_tracks[trk].active_buffer[f * 2] *= mult;
                    master_tracks[trk].active_buffer[f * 2 + 1] *= mult;
                }
            } else if (state->buffer) {
                size_t max_canvas =
                    (present_bounds > state->canvas_frames) ? present_bounds : state->canvas_frames;
                size_t swap_frames = (size_t)((double)max_canvas * (double)current_speed);
                if (swap_frames > master_tracks[trk].allocated_frames) {
                    swap_frames = master_tracks[trk].allocated_frames;
                }
                size_t floats_left = swap_frames * 2;
                float *ptr_a = master_tracks[trk].active_buffer;
                float *ptr_b = state->buffer;
                while (floats_left > 0) {
                    size_t to_swap = floats_left > 8192 ? 8192 : floats_left;
                    float temp_chunk[8192];
                    memcpy(temp_chunk, ptr_a, to_swap * sizeof(float));
                    memcpy(ptr_a, ptr_b, to_swap * sizeof(float));
                    memcpy(ptr_b, temp_chunk, to_swap * sizeof(float));
                    ptr_a += to_swap;
                    ptr_b += to_swap;
                    floats_left -= to_swap;
                }
                if (state->parity_buffer && master_tracks[trk].parity_map) {
                    size_t bytes_left = swap_frames;
                    uint8_t *p_ptr_a = master_tracks[trk].parity_map;
                    uint8_t *p_ptr_b = state->parity_buffer;
                    while (bytes_left > 0) {
                        size_t to_swap = bytes_left > 8192 ? 8192 : bytes_left;
                        uint8_t temp_chunk[8192];
                        memcpy(temp_chunk, p_ptr_a, to_swap * sizeof(uint8_t));
                        memcpy(p_ptr_a, p_ptr_b, to_swap * sizeof(uint8_t));
                        memcpy(p_ptr_b, temp_chunk, to_swap * sizeof(uint8_t));
                        p_ptr_a += to_swap;
                        p_ptr_b += to_swap;
                        bytes_left -= to_swap;
                    }
                }
            }
            bool tmp_audio =
                atomic_load_explicit(&master_tracks[trk].has_audio, memory_order_acquire);
            atomic_store_explicit(&master_tracks[trk].has_audio, state->has_audio,
                                  memory_order_release);
            state->has_audio = tmp_audio;
            float tmp_gain = atomic_load_explicit(&master_tracks[trk].gain, memory_order_acquire);
            atomic_store_explicit(&master_tracks[trk].gain, state->gain, memory_order_release);
            state->gain = tmp_gain;
            bool tmp_solo =
                atomic_load_explicit(&master_tracks[trk].is_soloed, memory_order_acquire);
            atomic_store_explicit(&master_tracks[trk].is_soloed, state->is_soloed,
                                  memory_order_release);
            state->is_soloed = tmp_solo;
            char tmp_name[64];
            get_multitrack_track_name(trk, tmp_name);
            set_multitrack_track_name(trk, state->name);
            strncpy(state->name, tmp_name, 63);
            state->name[63] = '\0';
            if (!globals_applied) {
                size_t restored_pristine =
                    (size_t)((double)state->canvas_frames * (double)current_speed);
                if (pristine_allocated_frames > 0 && restored_pristine > pristine_allocated_frames)
                    restored_pristine = pristine_allocated_frames;
                if (restored_pristine > 0) {
                    pristine_frames = restored_pristine;
                    size_t safe_bt_frames =
                        (size_t)((double)restored_pristine / (double)current_speed);
                    atomic_store_explicit(&get_transport_state()->backing_track_frames,
                                          safe_bt_frames, memory_order_release);
                }
                video_start_frame = state->v_start;
                video_end_frame = state->v_end;
                video_track_index = state->v_track_idx;
                globals_applied = true;
            }
            state->canvas_frames = present_bounds;
            state->v_start = present_v_start;
            state->v_end = present_v_end;
            state->v_track_idx = present_v_track_idx;
        }
        global_undo_ledger.head++;
    }
    int highest = 0;
    for (int i = 0; i < QJ_MAX_TRACKS; i++) {
        if (atomic_load_explicit(&master_tracks[i].has_audio, memory_order_acquire))
            highest = i;
    }
    int current_rec = atomic_load_explicit(&current_recording_track, memory_order_acquire);
    if (current_rec > highest &&
        !atomic_load_explicit(&master_tracks[current_rec].has_audio, memory_order_acquire)) {
        current_rec = highest;
        atomic_store_explicit(&current_recording_track, current_rec, memory_order_release);
        atomic_store_explicit(&selected_tracks_mask, (1U << current_rec), memory_order_release);
    }
    if (current_rec > highest)
        highest = current_rec;
    atomic_store_explicit(&active_track_count, highest + 1, memory_order_release);
    atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);
    evaluate_session_dirty_state();
    resume_rt_thread();
    if (stash_video) {
        abort_active_video_preview(true);
        force_clear_mkv_video_preview();
    }
    if (restore_video) {
        if (strlen(stashed_raw_path) > 0) {
            restore_stashed_video_preview();
        } else if (media_has_video(ui_state.selected_track_path) &&
                   access(ui_state.selected_track_path, F_OK) == 0) {
            launch_mkv_video_preview(ui_state.selected_track_path);
        }
    }
}

/**
 * @brief Crops the multi-track canvas to the active loop end or the last audible frame (+2s
 * tail).
 */
void crop_canvas(void) {
    size_t target_frames_1x = 0;
    int rate =
        atomic_load_explicit(&get_transport_state()->active_sample_rate, memory_order_acquire);
    if (rate <= 0)
        rate = 48000;

    if (atomic_load_explicit(&loop_active, memory_order_acquire)) {
        float current_speed =
            atomic_load_explicit(&get_transport_state()->playback_speed, memory_order_relaxed);
        target_frames_1x =
            (size_t)(atomic_load_explicit(&loop_end_frame, memory_order_acquire) * current_speed);
    } else {
        target_frames_1x = get_last_audible_frame() + ((size_t)rate * 2);
        if (target_frames_1x < (size_t)rate * 5)
            target_frames_1x = (size_t)rate * 5;
    }

    if (target_frames_1x >= pristine_frames)
        return;

    if (!await_rt_thread_detach())
        return;

    uint32_t mask = 0xFFFFFFFF;
    internal_backup_masked_tracks(mask);

    size_t clear_len = pristine_frames - target_frames_1x;
    for (int i = 0; i < QJ_MAX_TRACKS; i++) {
        if (master_tracks[i].active_buffer &&
            atomic_load_explicit(&master_tracks[i].has_audio, memory_order_acquire)) {
            memset(&master_tracks[i].active_buffer[target_frames_1x * 2], 0,
                   clear_len * 2 * sizeof(float));
            if (master_tracks[i].parity_map) {
                memset(&master_tracks[i].parity_map[target_frames_1x], 0,
                       clear_len * sizeof(uint8_t));
            }
        }
    }

    atomic_store_explicit(&pristine_frames, target_frames_1x, memory_order_release);

    float current_speed =
        atomic_load_explicit(&get_transport_state()->playback_speed, memory_order_relaxed);
    if (current_speed <= 0.0f)
        current_speed = 1.0f;
    size_t new_bt_frames = (size_t)((double)pristine_frames / (double)current_speed);

    if (video_start_frame >= new_bt_frames) {
        video_start_frame = 0;
        video_end_frame = 0;
        video_track_index = -1;
        atomic_store_explicit(&video_is_armed, false, memory_order_release);
    } else if (video_end_frame > new_bt_frames) {
        video_end_frame = new_bt_frames;
    }

    atomic_store_explicit(&get_transport_state()->backing_track_frames, new_bt_frames,
                          memory_order_release);

    size_t current_pos =
        atomic_load_explicit(&get_transport_state()->playback_pos, memory_order_acquire);
    if (current_pos > new_bt_frames) {
        atomic_store_explicit(&get_transport_state()->playback_pos, new_bt_frames,
                              memory_order_release);
    }

    atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);

    mark_session_dirty();
    resume_rt_thread();
    clear_loop_points();
}

extern float soft_clip(float x);

/**
 * @brief Renders a stereo mixdown of the specified frame range across all audible tracks.
 *        Protected by shared track read lock against concurrent buffer reallocation/free.
 * @param start Starting frame index (1.0x pristine domain).
 * @param end Ending frame index (1.0x pristine domain).
 * @return Newly allocated interleaved stereo float buffer, or NULL on failure/abort.
 */
float *render_mixdown_region(size_t start, size_t end) {
    if (!try_acquire_track_read())
        return NULL;
    size_t local_pristine = atomic_load_explicit(&pristine_frames, memory_order_acquire);
    if (start >= end || end > local_pristine) {
        release_track_read();
        return NULL;
    }
    size_t len = end - start;
    float *mix_buf = calloc(len * 2, sizeof(float));
    if (!mix_buf) {
        release_track_read();
        return NULL;
    }
    bool any_solo = false;
    for (int l = 0; l < QJ_MAX_TRACKS; l++) {
        if (atomic_load_explicit(&master_tracks[l].is_soloed, memory_order_relaxed)) {
            any_solo = true;
            break;
        }
    }
    for (size_t i = 0; i < len; i++) {
        if ((i & 0x3FFF) == 0 && atomic_load_explicit(&request_track_free, memory_order_relaxed)) {
            free(mix_buf);
            release_track_read();
            return NULL;
        }
        float sum_l = 0.0f;
        float sum_r = 0.0f;
        for (int l = 0; l < QJ_MAX_TRACKS; l++) {
            float *buf = master_tracks[l].active_buffer;
            if (!buf || (start + i) >= master_tracks[l].allocated_frames)
                continue;
            bool is_audible =
                atomic_load_explicit(&master_tracks[l].has_audio, memory_order_relaxed) &&
                (!any_solo ||
                 atomic_load_explicit(&master_tracks[l].is_soloed, memory_order_relaxed));
            if (is_audible) {
                float gain = atomic_load_explicit(&master_tracks[l].gain, memory_order_relaxed);
                sum_l += buf[(start + i) * 2] * gain;
                sum_r += buf[(start + i) * 2 + 1] * gain;
            }
        }
        mix_buf[i * 2] = soft_clip(sum_l);
        mix_buf[i * 2 + 1] = soft_clip(sum_r);
    }
    release_track_read();
    return mix_buf;
}
