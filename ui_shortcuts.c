#include "ui_shortcuts.h"
#include "audio_core.h"
#include "audio_memory.h"
#include "audio_nle.h"
#include "ui_globals.h"
#include "ui_multitrack.h"
#include "ui_playlist.h"
#include "ui_transport.h"
#include "media_io.h"
#include <math.h>
#include <string.h>
#include <unistd.h>

static void apply_keyboard_seek(double step_sign, GdkModifierType state) {
    if (atomic_load_explicit(&get_transport_state()->is_recording, memory_order_acquire))
        return;

    size_t frames =
        atomic_load_explicit(&get_transport_state()->backing_track_frames, memory_order_acquire);
    if (frames > 0) {

        // DYNAMIC ZOOM PROPORTIONALITY
        // Jumps exactly 1% of the currently visible timeline width (100 steps per screen)
        double step_fraction = 0.01 / zoom_multiplier;

        double current;
        if (atomic_load_explicit(&seek_flag, memory_order_acquire)) {
            current = atomic_load_explicit(&seek_target, memory_order_relaxed);
        } else {
            current = (double)atomic_load_explicit(&get_transport_state()->playback_pos,
                                                   memory_order_acquire) /
                      (double)frames;
        }

        double target = current + (step_fraction * step_sign);

        if (state & GDK_SHIFT_MASK) {
            // --- REGION SELECTION MODE ---
            double anchor;
            if (atomic_load_explicit(&loop_active, memory_order_acquire)) {
                double l_start =
                    (double)atomic_load_explicit(&loop_start_frame, memory_order_relaxed) /
                    (double)frames;
                double l_end = (double)atomic_load_explicit(&loop_end_frame, memory_order_relaxed) /
                               (double)frames;

                // Anchor to the edge furthest from the current playhead
                if (fabs(current - l_start) < fabs(current - l_end)) {
                    anchor = l_end;
                } else {
                    anchor = l_start;
                }
            } else {
                anchor = current;
            }

            target = fmax(0.0, fmin(1.0, target));

            set_loop_points(anchor, target);

            // Force redraw to visually update the blue loop region
            if (waveform_area_bt)
                gtk_widget_queue_draw(waveform_area_bt);
            if (waveform_area_input)
                gtk_widget_queue_draw(waveform_area_input);
        } else {
            // --- STANDARD SEEK MODE ---
            if (step_sign < 0.0) {
                target = fmax(0.0, target);
                if (atomic_load_explicit(&loop_active, memory_order_acquire)) {
                    double l_start =
                        (double)atomic_load_explicit(&loop_start_frame, memory_order_relaxed) /
                        (double)frames;
                    target = fmax(l_start, target);
                }
            } else {
                target = fmin(1.0, target);
                if (atomic_load_explicit(&loop_active, memory_order_acquire)) {
                    double l_end =
                        (double)atomic_load_explicit(&loop_end_frame, memory_order_relaxed) /
                        (double)frames;
                    target = fmin(l_end, target);
                }
            }
        }

        seek_backing_track(target);
    }
}

static bool is_time_shift_safe(uint32_t mask, bool is_global_edit) {
    bool has_video = false;
    if (has_active_live_video())
        has_video = true;

    if (is_video_file(ui_state.selected_track_path)) {
        has_video = true;
    }

    if (!has_video)
        return true;


    size_t edit_start = 0;
    if (atomic_load_explicit(&loop_active, memory_order_acquire)) {
        edit_start = atomic_load_explicit(&loop_start_frame, memory_order_relaxed);
    } else {
        edit_start =
            atomic_load_explicit(&get_transport_state()->playback_pos, memory_order_acquire);
    }

    // If the edit happens completely after the video ends, time-shifting is safe
    if (video_end_frame > 0 && edit_start >= video_end_frame) {
        return true;
    }

    // Global edits (Shift+Delete, 'x' blend) physically shrink the master canvas,
    // which inherently shifts ALL tracks and ruins video sync.
    if (is_global_edit)
        return false;


    // If a video track is mapped, lock ONLY that specific track index.
    if (video_track_index >= 0 && video_track_index < QJ_MAX_TRACKS) {
        if (mask & (1 << video_track_index)) {
            return false;
        }
    }

    // If the video track is not in the active selection mask, the user is modifying
    // a pure overdub (e.g., a solo). This is completely safe.
    return true;
}

typedef void (*NLE_Func)(void);

static void async_nle_thread(GTask *task, gpointer source, gpointer task_data, GCancellable *c) {
    (void)source;
    (void)c;
    NLE_Func func = (NLE_Func)task_data;
    if (func)
        func();
    atomic_store_explicit(&cached_audible_frames, get_last_audible_frame(), memory_order_release);
    g_task_return_boolean(task, TRUE);
}

static void async_nle_ready(GObject *source, GAsyncResult *res, gpointer data) {
    (void)source;
    (void)data;
    g_task_propagate_boolean(G_TASK(res), NULL);

    trigger_global_ui_refresh();

    if (main_spinner)
        gtk_spinner_stop(GTK_SPINNER(main_spinner));

    set_app_waiting_state(false);

    GtkWindow *win =
        gtk_application_get_active_window(GTK_APPLICATION(g_application_get_default()));
    if (win) {
        GtkWidget *master_box = gtk_window_get_child(win);
        if (master_box)
            gtk_widget_set_sensitive(master_box, TRUE);
    }
}

static void dispatch_nle_operation(NLE_Func op) {
    GtkWindow *win =
        gtk_application_get_active_window(GTK_APPLICATION(g_application_get_default()));
    if (win) {
        GtkWidget *focused = gtk_window_get_focus(win);
        if (focused && (GTK_IS_TEXT(focused) || GTK_IS_EDITABLE(focused))) {
            gtk_window_set_focus(win, NULL);
        }
        GtkWidget *master_box = gtk_window_get_child(win);
        if (master_box)
            gtk_widget_set_sensitive(master_box, FALSE);
    }

    if (main_spinner)
        gtk_spinner_start(GTK_SPINNER(main_spinner));

    set_app_waiting_state(true);

    GTask *task = g_task_new(NULL, NULL, async_nle_ready, NULL);
    g_task_set_task_data(task, op, NULL);
    g_task_run_in_thread(task, async_nle_thread);
    g_object_unref(task);
}

static void execute_delete_active_layer(void) {
    uint32_t mask = atomic_load_explicit(&selected_tracks_mask, memory_order_acquire);
    delete_masked_multitrack_layers(mask);
}

gboolean on_window_key_pressed(GtkEventControllerKey *controller, guint keyval, guint keycode,
                               GdkModifierType state, gpointer user_data) {
    (void)keycode;
    (void)user_data;
    if (main_spinner && gtk_spinner_get_spinning(GTK_SPINNER(main_spinner))) {
        return TRUE;
    }

    GtkWidget *window = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller));
    GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(window));

    bool in_text_entry = false;
    bool in_spin_button = false;
    if (focus && (GTK_IS_EDITABLE(focus) || GTK_IS_TEXT(focus))) {
        in_text_entry = true;
        GtkWidget *parent = gtk_widget_get_parent(focus);
        if (GTK_IS_SPIN_BUTTON(focus) || (parent && GTK_IS_SPIN_BUTTON(parent))) {
            in_text_entry = false;
            in_spin_button = true;
        }
    }
    if (in_text_entry)
        return FALSE;

    if (in_spin_button) {
        /* Allow numeric editing keys to propagate into the GtkSpinButton's GtkText */
        if ((keyval >= GDK_KEY_0 && keyval <= GDK_KEY_9) ||
            (keyval >= GDK_KEY_KP_0 && keyval <= GDK_KEY_KP_9) || keyval == GDK_KEY_minus ||
            keyval == GDK_KEY_KP_Subtract || keyval == GDK_KEY_period || keyval == GDK_KEY_comma ||
            keyval == GDK_KEY_BackSpace || keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
            if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
                gtk_window_set_focus(GTK_WINDOW(window), NULL);
            }
            return FALSE;
        }
        /* Defocus the spin button's internal GtkText before executing global DAW shortcuts */
        gtk_window_set_focus(GTK_WINDOW(window), NULL);
    }

    bool has_ctrl = (state & GDK_CONTROL_MASK) != 0;
    bool has_shift = (state & GDK_SHIFT_MASK) != 0;

    switch (keyval) {
        case GDK_KEY_a:
        case GDK_KEY_A:
            if (has_ctrl && !has_shift) {
                GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(window));

                bool in_playlist =
                    focus && (focus == right_vbox || gtk_widget_is_ancestor(focus, right_vbox));
                bool in_cmd = focus && (focus == command_post_box ||
                                        gtk_widget_is_ancestor(focus, command_post_box));
                bool in_left = focus && (focus == left_column_box ||
                                         gtk_widget_is_ancestor(focus, left_column_box));
                bool in_multi = in_left && !in_playlist && !in_cmd;

                if (in_playlist) {
                    return playlist_handle_ctrl_a();
                } else if (in_multi) {
                    multitrack_select_all_active();
                    return TRUE;
                } else {
                    size_t last_f = get_last_audible_frame();

                    if (last_f > 0 && pristine_frames > 0) {
                        set_loop_points(0.0, (double)last_f / (double)pristine_frames);
                        seek_backing_track(0.0);
                        if (waveform_area_bt)
                            gtk_widget_queue_draw(waveform_area_bt);
                        if (waveform_area_input)
                            gtk_widget_queue_draw(waveform_area_input);
                    }
                    return TRUE;
                }
            }
            return FALSE;

        case GDK_KEY_c:
        case GDK_KEY_C:
            if (has_ctrl && !has_shift) {
                dispatch_nle_operation(copy_selection);
                return TRUE;
            } else if (!has_ctrl && !has_shift) {
                if (btn_speed && gtk_widget_is_sensitive(btn_speed)) {
                    g_signal_emit_by_name(btn_speed, "clicked");
                }
                return TRUE;
            }
            return FALSE;

        case GDK_KEY_f:
        case GDK_KEY_F:
            if (!has_ctrl && !has_shift) {
                dispatch_nle_operation(apply_smart_fade_selection);
                return TRUE;
            }
            return FALSE;

        case GDK_KEY_n:
        case GDK_KEY_N:
        case GDK_KEY_Page_Down:
            if (!has_ctrl && btn_next && gtk_widget_is_sensitive(btn_next)) {
                g_signal_emit_by_name(btn_next, "clicked");
            }
            return TRUE;

        case GDK_KEY_p:
        case GDK_KEY_P:
        case GDK_KEY_Page_Up:
            if (!has_ctrl && btn_prev && gtk_widget_is_sensitive(btn_prev)) {
                g_signal_emit_by_name(btn_prev, "clicked");
            }
            return TRUE;

        case GDK_KEY_r:
        case GDK_KEY_R:
            if (!has_ctrl && !has_shift) {
                if (btn_record && gtk_widget_is_sensitive(btn_record)) {
                    g_signal_emit_by_name(btn_record, "clicked");
                }
                return TRUE;
            }
            return FALSE;

        case GDK_KEY_s:
        case GDK_KEY_S:
            if (!has_ctrl && !has_shift && btn_stop && gtk_widget_is_sensitive(btn_stop)) {
                g_signal_emit_by_name(btn_stop, "clicked");
            }
            return TRUE;

        case GDK_KEY_t:
        case GDK_KEY_T:
            if (!has_ctrl && !has_shift) {
                // Replace 'btn_video_size' with your actual ↕ button variable name
                if (btn_video_size && gtk_widget_is_sensitive(btn_video_size)) {
                    g_signal_emit_by_name(btn_video_size, "clicked");
                }
                return TRUE;
            }
            return FALSE;

        case GDK_KEY_v:
        case GDK_KEY_V:
            if (has_ctrl) {
                uint32_t mask = atomic_load_explicit(&selected_tracks_mask, memory_order_acquire);

                if (!is_time_shift_safe(mask, has_shift)) {
                    log_and_notify(1, "Status: Time shifting edits disabled to protect video sync");
                    return TRUE;
                }

                if (has_shift) {
                    dispatch_nle_operation(global_paste_selection);
                } else {
                    dispatch_nle_operation(paste_selection);
                }
                return TRUE;
            } else if (!has_ctrl && !has_shift) {
                if (btn_record_video && gtk_widget_is_sensitive(btn_record_video)) {
                    g_signal_emit_by_name(btn_record_video, "clicked");
                }
                return TRUE;
            }
            return FALSE;

        case GDK_KEY_x:
        case GDK_KEY_X: {
            uint32_t mask = atomic_load_explicit(&selected_tracks_mask, memory_order_acquire);

            if (has_ctrl && !has_shift) {
                if (!is_time_shift_safe(mask, false)) {
                    log_and_notify(1, "Status: Time shifting edits disabled to protect video sync");
                    return TRUE;
                }
                dispatch_nle_operation(cut_selection);
                return TRUE;
            } else if (!has_ctrl && !has_shift) {
                // Ripple crossfade alters global canvas length, so is_global_edit must be true
                if (!is_time_shift_safe(mask, true)) {
                    log_and_notify(1, "Status: Time shifting edits disabled to protect video sync");
                    return TRUE;
                }
                dispatch_nle_operation(apply_blend_fade_selection);
                return TRUE;
            }
            return FALSE;
        }

        case GDK_KEY_y:
        case GDK_KEY_Y:
            if (has_ctrl && !has_shift) {
                if (global_undo_ledger.head >= global_undo_ledger.count)
                    return TRUE;

                static gint64 last_redo_time_y = 0;
                gint64 now = g_get_monotonic_time();
                if (now - last_redo_time_y < 250000)
                    return TRUE;
                last_redo_time_y = now;

                dispatch_nle_operation(redo_selection);
                return TRUE;
            }
            return FALSE;

        case GDK_KEY_z:
        case GDK_KEY_Z:
            if (has_ctrl && !has_shift) {
                if (global_undo_ledger.head <= 0 || global_undo_ledger.count <= 0)
                    return TRUE;

                static gint64 last_undo_time = 0;
                gint64 now = g_get_monotonic_time();
                if (now - last_undo_time < 250000)
                    return TRUE;
                last_undo_time = now;

                dispatch_nle_operation(undo_selection);
                return TRUE;
            } else if (has_ctrl && has_shift) {
                if (global_undo_ledger.head >= global_undo_ledger.count)
                    return TRUE;

                static gint64 last_redo_time_z = 0;
                gint64 now = g_get_monotonic_time();
                if (now - last_redo_time_z < 250000)
                    return TRUE;
                last_redo_time_z = now;

                dispatch_nle_operation(redo_selection);
                return TRUE;
            }
            return FALSE;

        case GDK_KEY_Delete: {
            if (has_ctrl && !has_shift) {
                static gint64 last_ctrl_del_time = 0;
                gint64 now = g_get_monotonic_time();
                if (now - last_ctrl_del_time < 250000)
                    return TRUE;
                last_ctrl_del_time = now;

                dispatch_nle_operation(execute_delete_active_layer);
                return TRUE;
            } else if (!has_ctrl && has_shift) {
                if (!is_time_shift_safe(0xFFFFFFFF, true)) {
                    log_and_notify(1, "Status: Time shifting edits disabled to protect video sync");
                    return TRUE;
                }
                clear_video_stash_async();

                dispatch_nle_operation(global_ripple_delete_selection);
                return TRUE;
            } else if (!has_ctrl && !has_shift) {
                dispatch_nle_operation(delete_selection);
                return TRUE;
            }
            return FALSE;
        }
        case GDK_KEY_Escape:
            if (atomic_load_explicit(&get_transport_state()->is_recording, memory_order_acquire) ||
                is_countdown_active()) {
                if (btn_stop && gtk_widget_is_sensitive(btn_stop)) {
                    g_signal_emit_by_name(btn_stop, "clicked");
                }
                return TRUE;
            }

            if (!atomic_load_explicit(&get_transport_state()->is_recording, memory_order_acquire)) {
                if (atomic_load_explicit(&loop_active, memory_order_acquire)) {
                    clear_loop_points();
                    gtk_widget_queue_draw(waveform_area_bt);
                    gtk_widget_queue_draw(waveform_area_input);
                } else {
                    if (btn_reset && gtk_widget_is_sensitive(btn_reset)) {
                        g_signal_emit_by_name(btn_reset, "clicked");
                    }
                }
            }
            return TRUE;

        case GDK_KEY_space:
            if (atomic_load_explicit(&get_transport_state()->is_recording, memory_order_acquire)) {
                if (atomic_load_explicit(&video_is_armed, memory_order_acquire)) {
                    if (btn_record_video && gtk_widget_is_sensitive(btn_record_video)) {
                        g_signal_emit_by_name(btn_record_video, "clicked");
                    }
                } else {
                    if (btn_record && gtk_widget_is_sensitive(btn_record)) {
                        g_signal_emit_by_name(btn_record, "clicked");
                    }
                }
            } else {
                if (btn_play && gtk_widget_is_sensitive(btn_play)) {
                    g_signal_emit_by_name(btn_play, "clicked");
                }
            }
            return TRUE;

        case GDK_KEY_Up:
        case GDK_KEY_Down:
            if (!has_ctrl) {
                // Determine exactly which quadrant of the DAW currently has focus
                GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(window));
                bool in_playlist =
                    focus && (focus == right_vbox || gtk_widget_is_ancestor(focus, right_vbox));
                bool in_cmd = focus && (focus == command_post_box ||
                                        gtk_widget_is_ancestor(focus, command_post_box));
                bool in_left = focus && (focus == left_column_box ||
                                         gtk_widget_is_ancestor(focus, left_column_box));
                bool in_multi = in_left && !in_playlist && !in_cmd;
                bool in_waveforms =
                    focus && (focus == waveform_area_bt || focus == waveform_area_input);

                // If the user is focused on the Playlist, Command Post, or Dashboard (dropdowns),
                // return FALSE to let GTK handle the navigation natively.
                if (focus != NULL && !in_multi && !in_waveforms) {
                    return FALSE;
                }

                int current = atomic_load_explicit(&current_recording_track, memory_order_acquire);
                int dir = (keyval == GDK_KEY_Down) ? 1 : -1;

                // Cycle selection with infinite wrap-around (0 to 11)
                int next = (current + dir + QJ_MAX_TRACKS) % QJ_MAX_TRACKS;

                if (has_shift) {
                    uint32_t mask =
                        atomic_load_explicit(&selected_tracks_mask, memory_order_acquire);
                    mask |= (1 << next);
                    atomic_store_explicit(&selected_tracks_mask, mask, memory_order_release);
                } else {
                    atomic_store_explicit(&selected_tracks_mask, (1 << next), memory_order_release);
                }

                atomic_store_explicit(&current_recording_track, next, memory_order_release);

                int active = atomic_load_explicit(&active_track_count, memory_order_acquire);
                if (next >= active) {
                    atomic_store_explicit(&active_track_count, next + 1, memory_order_release);
                }

                update_multitrack_status_ui();
                return TRUE;
            }
            return FALSE;

        case GDK_KEY_Left:
            apply_keyboard_seek(-1.0, state);
            return TRUE;

        case GDK_KEY_Right:
            apply_keyboard_seek(1.0, state);
            return TRUE;

        case GDK_KEY_bracketleft:
            if (!has_ctrl && !has_shift) {
                step_playback_speed(-0.1f);
                return TRUE;
            }
            return FALSE;

        case GDK_KEY_bracketright:
            if (!has_ctrl && !has_shift) {
                step_playback_speed(0.1f);
                return TRUE;
            }
            return FALSE;

        case GDK_KEY_Home:
            if (!atomic_load_explicit(&get_transport_state()->is_recording, memory_order_acquire)) {
                if (has_shift) {
                    size_t total = atomic_load_explicit(
                        &get_transport_state()->backing_track_frames, memory_order_acquire);
                    if (total > 0) {
                        double end_frac = 1.0;
                        if (atomic_load_explicit(&loop_active, memory_order_acquire)) {
                            end_frac = (double)atomic_load_explicit(&loop_end_frame,
                                                                    memory_order_acquire) /
                                       (double)total;
                        } else {
                            end_frac =
                                (double)atomic_load_explicit(&get_transport_state()->playback_pos,
                                                             memory_order_acquire) /
                                (double)total;
                        }
                        set_loop_points(0.0, end_frac);
                        seek_backing_track(0.0);
                        gtk_widget_queue_draw(waveform_area_bt);
                        gtk_widget_queue_draw(waveform_area_input);
                    }
                } else {
                    clear_loop_points();
                    seek_backing_track(0.0);
                }
            }
            return TRUE;

        case GDK_KEY_End:
            if (!atomic_load_explicit(&get_transport_state()->is_recording, memory_order_acquire)) {
                size_t total = atomic_load_explicit(&get_transport_state()->backing_track_frames,
                                                    memory_order_acquire);
                if (total > 0) {
                    size_t last_f = get_last_audible_frame();

                    double target_frac = 1.0;
                    if (!has_ctrl && last_f > 0 && pristine_frames > 0) {
                        target_frac = (double)last_f / (double)pristine_frames;
                    }

                    if (has_shift) {
                        double start_frac = 0.0;
                        if (atomic_load_explicit(&loop_active, memory_order_acquire)) {
                            start_frac = (double)atomic_load_explicit(&loop_start_frame,
                                                                      memory_order_acquire) /
                                         (double)total;
                        } else {
                            start_frac =
                                (double)atomic_load_explicit(&get_transport_state()->playback_pos,
                                                             memory_order_acquire) /
                                (double)total;
                        }
                        set_loop_points(start_frac, target_frac);
                        seek_backing_track(start_frac);
                        gtk_widget_queue_draw(waveform_area_bt);
                        gtk_widget_queue_draw(waveform_area_input);
                    } else {
                        clear_loop_points();
                        seek_backing_track(target_frac);
                    }
                }
            }
            return TRUE;

        case GDK_KEY_F2:
            multitrack_start_rename();
            return TRUE;

        case GDK_KEY_1:
        case GDK_KEY_2:
        case GDK_KEY_3:
        case GDK_KEY_4:
        case GDK_KEY_5:
        case GDK_KEY_6:
        case GDK_KEY_7:
        case GDK_KEY_8:
        case GDK_KEY_9:
        case GDK_KEY_0: {
            int slot = -1;
            if (keyval >= GDK_KEY_1 && keyval <= GDK_KEY_9) {
                slot = keyval - GDK_KEY_1;
            } else if (keyval == GDK_KEY_0) {
                slot = 9;
            }
            if (slot != -1) {
                activate_playlist_preset(slot);
                return TRUE;
            }
            return FALSE;
        }
    }
    return FALSE;
}
