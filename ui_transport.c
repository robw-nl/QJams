#include "ui_transport.h"
#include "audio_core.h"
#include "audio_memory.h"
#include "audio_nle.h"
#include "encoder.h"
#include "media_io.h"
#include "ringbuffer.h"
#include "ui_dashboard.h"
#include "ui_globals.h"
#include "ui_multitrack.h"
#include "ui_playlist.h"
#include "ui_waveforms.h"
#include "video_engine.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

atomic_size_t live_video_start_frame = 0;
size_t live_audio_start_frame = 0;

/**
 * @brief Updates sensitivity of global Command Post and Transport widgets based on engine state.
 * @param is_playing true if playback is active.
 * @param is_recording true if recording is active.
 */
void update_global_ui_locks(bool is_playing, bool is_recording) {
    bool is_idle = (!is_playing && !is_recording);

    // --- Command Post (Fully locked unless IDLE) ---
    if (btn_load)
        gtk_widget_set_sensitive(btn_load, is_idle);
    if (btn_load_session)
        gtk_widget_set_sensitive(btn_load_session, is_idle);
    if (btn_load_playlist)
        gtk_widget_set_sensitive(btn_load_playlist, is_idle);
    if (btn_settings)
        gtk_widget_set_sensitive(btn_settings, is_idle);

    if (btn_save_session)
        gtk_widget_set_sensitive(btn_save_session, is_idle);
    if (btn_save_playlist)
        gtk_widget_set_sensitive(btn_save_playlist, is_idle);
    if (btn_save_mux)
        gtk_widget_set_sensitive(btn_save_mux, is_idle);

    int populated_tracks = 0;
    for (int i = 0; i < QJ_MAX_TRACKS; i++) {
        if (atomic_load_explicit(&master_tracks[i].has_audio, memory_order_acquire)) {
            populated_tracks++;
        }
    }

    // --- Transport Panel ---
    bool is_practice_mode =
        (populated_tracks <= 1 && !ui_state.is_existing_session && !is_session_dirty());
    bool can_navigate_playlist = !is_recording && (!is_playing || is_practice_mode);
    if (btn_prev)
        gtk_widget_set_sensitive(btn_prev, can_navigate_playlist);
    if (btn_next)
        gtk_widget_set_sensitive(btn_next, can_navigate_playlist);
    if (btn_reset)
        gtk_widget_set_sensitive(btn_reset, is_idle);
    if (btn_playlist_toggle)
        gtk_widget_set_sensitive(btn_playlist_toggle, is_idle);

    // Speed can be changed while playing, but not recording
    if (btn_speed)
        gtk_widget_set_sensitive(btn_speed, !is_recording);

    // Play: Disabled when recording. Enabled when playing (to pause) or idle (if audio exists)
    if (btn_play) {
        if (is_recording) {
            gtk_widget_set_sensitive(btn_play, FALSE);
        } else if (is_playing) {
            gtk_widget_set_sensitive(btn_play, TRUE);
        } else {
            gtk_widget_set_sensitive(btn_play, populated_tracks > 0);
        }
    }

    // Record buttons: Disabled when playing. Enabled when recording (to pause) or idle.
    if (btn_record)
        gtk_widget_set_sensitive(btn_record, !is_playing);
    if (btn_record_video)
        gtk_widget_set_sensitive(btn_record_video, !is_playing);

    // Stop: Only enabled when actively running
    if (btn_stop)
        gtk_widget_set_sensitive(btn_stop, !is_idle);
}

static guint speed_debounce_id = 0;
static float pending_speed = 1.0f;
static guint countdown_timer_id = 0;
static int countdown_val = 3;
static GtkWidget *countdown_window = NULL;
static GtkWidget *countdown_label = NULL;

/**
 * @brief Updates the zoom/crop button label to display the current backing track length.
 */
void update_zoom_button_label_to_length(void) {
    if (!btn_zoom)
        return;
    size_t frames =
        atomic_load_explicit(&get_transport_state()->backing_track_frames, memory_order_acquire);
    int rate =
        atomic_load_explicit(&get_transport_state()->active_sample_rate, memory_order_acquire);
    if (frames > 0 && rate > 0) {
        int total_secs = frames / rate;
        char len_str[64];
        if (total_secs >= 3600) {
            snprintf(len_str, sizeof(len_str), "Length %d:%02d:%02d", total_secs / 3600,
                     (total_secs % 3600) / 60, total_secs % 60);
        } else {
            snprintf(len_str, sizeof(len_str), "Length %d:%02d", total_secs / 60, total_secs % 60);
        }
        gtk_button_set_label(GTK_BUTTON(btn_zoom), len_str);
    } else {
        gtk_button_set_label(GTK_BUTTON(btn_zoom), "Length 0:00");
    }
}

static gboolean apply_speed_deferred(gpointer user_data) {
    (void)user_data;
    set_playback_speed(pending_speed);
    speed_debounce_id = 0;
    return G_SOURCE_REMOVE;
}

/**
 * @brief Toggles the 3-second countdown on loop wrap when recording.
 */
void on_loop_countdown_toggled(GtkToggleButton *btn, gpointer user_data) {
    (void)user_data;
    ui_state.config.loop_countdown_active = gtk_toggle_button_get_active(btn) ? 1 : 0;
    save_qjams_config(ui_state.config_path, &ui_state.config);

    if (ui_state.config.loop_countdown_active) {
        log_and_notify(1, "Status: Loop Wrap Countdown Enabled");
    } else {
        log_and_notify(1, "Status: Loop Wrap Countdown Disabled");
    }
}

void on_overlay_clip_toggled(GtkToggleButton *btn, gpointer user_data) {
    (void)user_data;
    ui_state.config.overlay_soft_clip = gtk_toggle_button_get_active(btn) ? 1 : 0;
    save_qjams_config(ui_state.config_path, &ui_state.config);

    if (ui_state.config.overlay_soft_clip) {
        log_and_notify(1, "Status: Audio Merge Mode: Hard clipping reduced");
    } else {
        log_and_notify(1, "Status: Audio Merge Mode: Unaltered audio");
    }
}

/**
 * @brief Resets playback speed to 1.0x.
 */
void on_speed_clicked(GtkButton *button, gpointer user_data) {
    (void)user_data;
    if (atomic_load_explicit(&get_transport_state()->is_recording, memory_order_acquire))
        return;

    pending_speed = 1.0f;
    gtk_button_set_label(button, "Speed: 1.0x");

    if (speed_debounce_id != 0)
        g_source_remove(speed_debounce_id);
    speed_debounce_id = g_timeout_add(500, apply_speed_deferred, NULL);
}

/**
 * @brief Adjusts the playback speed multiplier up or down by step increments.
 * @param delta Direction and magnitude of speed step.
 */
void step_playback_speed(float delta) {
    if (atomic_load_explicit(&get_transport_state()->is_recording, memory_order_acquire))
        return;

    int cur = (int)lroundf(pending_speed * 100.0f);
    int dir = (delta >= 0.0f) ? 1 : -1;

    // Use 0.05 steps below 1.0x, and 0.10 steps at 1.0x and above
    int edge = (dir > 0) ? cur : cur - 1;
    int step = (edge < 100) ? 5 : 10;

    cur += dir * step;
    if (cur < 20)
        cur = 20;
    if (cur > 500)
        cur = 500;

    pending_speed = cur / 100.0f;

    if (btn_speed) {
        char buf[32];
        snprintf(buf, sizeof(buf), (cur % 10) ? "Speed: %.2fx" : "Speed: %.1fx\u2007",
                 pending_speed);
        gtk_button_set_label(GTK_BUTTON(btn_speed), buf);
    }

    if (speed_debounce_id != 0)
        g_source_remove(speed_debounce_id);
    speed_debounce_id = g_timeout_add(500, apply_speed_deferred, NULL);
}

/**
 * @brief Synchronizes the UI speed button label and pending speed state with the engine.
 * @param speed Active playback speed multiplier.
 */
void sync_speed_ui_label(float speed) {
    if (speed_debounce_id != 0) {
        g_source_remove(speed_debounce_id);
        speed_debounce_id = 0;
    }
    pending_speed = (speed > 0.05f && speed <= 5.0f) ? speed : 1.0f;
    if (btn_speed) {
        int cur = (int)lroundf(pending_speed * 100.0f);
        char buf[32];
        snprintf(buf, sizeof(buf), (cur % 10) ? "Speed: %.2fx" : "Speed: %.1fx\u2007",
                 pending_speed);
        gtk_button_set_label(GTK_BUTTON(btn_speed), buf);
    }
}

static void async_crop_thread(GTask *task, gpointer source, gpointer task_data, GCancellable *c) {
    (void)source;
    (void)task_data;
    (void)c;
    crop_canvas();
    atomic_store_explicit(&cached_audible_frames, get_last_audible_frame(), memory_order_release);
    g_task_return_boolean(task, TRUE);
}

static void async_crop_ready(GObject *source, GAsyncResult *res, gpointer data) {
    (void)source;
    (void)data;
    g_task_propagate_boolean(G_TASK(res), NULL);
    mark_session_dirty();
    zoom_multiplier = 1.0;
    if (waveform_adj)
        gtk_adjustment_set_value(waveform_adj, 0.0);
    update_zoom_button_label_to_length();
    invalidate_waveform_caches();

    if (waveform_area_bt)
        gtk_widget_queue_draw(waveform_area_bt);
    if (waveform_area_input)
        gtk_widget_queue_draw(waveform_area_input);

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

    log_and_notify(1, "Status: Canvas cropped to recording length");
}

static void on_crop_confirm(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    (void)user_data;
    GtkAlertDialog *alert = GTK_ALERT_DIALOG(source_object);
    if (gtk_alert_dialog_choose_finish(alert, res, NULL) == 0) {
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

        GTask *task = g_task_new(NULL, NULL, async_crop_ready, NULL);
        g_task_run_in_thread(task, async_crop_thread);
        g_object_unref(task);
    }
}

/**
 * @brief Prompts the user to confirm cropping the canvas to the active selection or audio length.
 */
void on_crop_clicked(GtkButton *button, gpointer user_data) {
    (void)user_data;
    if (atomic_load_explicit(&get_transport_state()->is_recording, memory_order_acquire))
        return;

    bool has_selection = atomic_load_explicit(&loop_active, memory_order_acquire);

    GtkAlertDialog *alert = gtk_alert_dialog_new(has_selection ? "Crop track to selection?"
                                                               : "Crop track to recording length?");
    const char *detail = has_selection ? "This will shrink the session length to exactly match "
                                         "your current timeline selection. Are you sure?"
                                       : "This will shrink the session length to fit your active "
                                         "audio (plus a 2-second reverb tail). Are you sure?";

    gtk_alert_dialog_set_detail(alert, detail);

    const char *buttons[] = {"Crop Canvas", "Cancel", NULL};
    gtk_alert_dialog_set_buttons(alert, buttons);
    gtk_alert_dialog_set_cancel_button(alert, 1);
    gtk_alert_dialog_set_default_button(alert, 0);

    GtkWindow *parent = GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(button)));
    gtk_alert_dialog_choose(alert, parent, NULL, on_crop_confirm, NULL);
    g_object_unref(alert);
}

/**
 * @brief Cycles to the previous track in the active playlist.
 */
void on_prev_track_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    cycle_playlist_track(-1);
}

/**
 * @brief Cycles to the next track in the active playlist.
 */
void on_next_track_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    cycle_playlist_track(1);
}

/**
 * @brief Background thread to safely allocate the canvas.
 * @note Safety [Critical]: The RT thread must be alive and ticking before `init_empty_loop_canvas`
 * is allowed to attempt the `await_rt_thread_detach` lock-free handshake.
 */
static void init_canvas_thread(GTask *task, gpointer source_object, gpointer task_data,
                               GCancellable *cancellable) {
    (void)source_object;
    (void)cancellable;
    int duration_sec = GPOINTER_TO_INT(task_data);

    // Give PipeWire an initial 500ms grace period to negotiate the hardware graph
    usleep(500000);

    int retries = 10;
    int status = -1;
    while (retries > 0) {
        status = init_empty_loop_canvas(duration_sec);
        if (status == 0 || status == -2)
            break;

        usleep(500000);
        retries--;
    }
    g_task_return_int(task, status);
}

static void init_canvas_ready(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    (void)source_object;
    (void)user_data;
    int status = g_task_propagate_int(G_TASK(res), NULL);

    if (status == 0) {
        update_multitrack_status_ui();
        gtk_widget_set_sensitive(btn_play, FALSE);
        gtk_widget_set_sensitive(btn_record, TRUE);

        mark_session_saved();
        update_zoom_button_label_to_length();

        invalidate_waveform_caches();
        if (waveform_area_bt)
            gtk_widget_queue_draw(waveform_area_bt);
        if (waveform_area_input)
            gtk_widget_queue_draw(waveform_area_input);

        if (lbl_status)
            log_and_notify(1, "Status: Ready");

    } else {
        if (lbl_status) {
            if (status == -2) {
                log_and_notify(1, "<span foreground='#ff4444'><b>Status: Insufficient RAM for "
                                  "blank canvas</b></span>");
            } else {
                log_and_notify(1, "<span foreground='#ff4444'><b>Status: Audio Engine Timeout "
                                  "(Failed to allocate canvas)</b></span>");
            }
        }
    }

    // Present the window cleanly after all canvas math and layout rules have settled
    GList *windows = gtk_application_get_windows(GTK_APPLICATION(g_application_get_default()));
    if (windows) {
        gtk_window_present(GTK_WINDOW(windows->data));
    }
}

/**
 * @brief Dispatches an asynchronous GTask to initialize the default blank loop canvas on startup.
 */
void adjust_blank_canvas(void) {
    if (is_session_dirty())
        return;

    if (lbl_status)
        log_and_notify(1, "Status: Starting Audio Engine...");

    int canvas_limit_min =
        ui_state.config.default_canvas_limit_min > 0 ? ui_state.config.default_canvas_limit_min : 5;
    int duration_sec = canvas_limit_min * 60;

    // Launch the non-blocking background allocation
    GTask *task = g_task_new(NULL, NULL, init_canvas_ready, NULL);
    g_task_set_task_data(task, GINT_TO_POINTER(duration_sec), NULL);
    g_task_run_in_thread(task, init_canvas_thread);
    g_object_unref(task);
}

/**
 * @brief Periodic 100ms UI monitor checking for disk full errors and end-of-canvas auto-stop.
 *        Automatically rewinds the playhead to 00:00 when playback or recording auto-stops at the
 * end.
 */
gboolean transport_monitor_tick(gpointer user_data) {
    (void)user_data;

    if (atomic_exchange_explicit(&encoder_disk_error, false, memory_order_acquire)) {
        if (btn_stop && gtk_widget_is_sensitive(btn_stop)) {
            g_signal_emit_by_name(btn_stop, "clicked");
            log_and_notify(1, "<span foreground='#ff4444'><b>Status: RECORDING ABORTED (Disk Full "
                              "/ I/O Error)</b></span>");
        }
    }

    bool is_playing =
        atomic_load_explicit(&get_transport_state()->is_playing, memory_order_acquire);
    bool is_recording =
        atomic_load_explicit(&get_transport_state()->is_recording, memory_order_acquire);

    if (is_playing || is_recording) {
        size_t current_pos =
            atomic_load_explicit(&get_transport_state()->playback_pos, memory_order_acquire);
        size_t bt_frames = atomic_load_explicit(&get_transport_state()->backing_track_frames,
                                                memory_order_acquire);

        size_t stop_frame = bt_frames;

        /**
         * @brief Determine logical end of track for playback.
         * Scales 1.0x cached_audible_frames into the active playback_speed domain
         * and falls back to bt_frames if the background scanner is still pending.
         */
        if (!is_recording && !atomic_load_explicit(&loop_active, memory_order_acquire)) {
            size_t audible_1x = atomic_load_explicit(&cached_audible_frames, memory_order_acquire);
            float speed =
                atomic_load_explicit(&get_transport_state()->playback_speed, memory_order_relaxed);
            if (speed <= 0.05f) {
                speed = 1.0f;
            }
            size_t audible_scaled = (size_t)((double)audible_1x / (double)speed);
            size_t v_end = atomic_load_explicit(&video_end_frame, memory_order_acquire);
            size_t content_end = (audible_scaled > v_end) ? audible_scaled : v_end;

            if (content_end > 0) {
                int rate = atomic_load_explicit(&get_transport_state()->active_sample_rate,
                                                memory_order_acquire);
                stop_frame = content_end + (size_t)(rate > 0 ? rate : 48000);
                if (stop_frame > bt_frames) {
                    stop_frame = bt_frames;
                }
            }
        }

        if (bt_frames > 0 && current_pos >= stop_frame) {
            if (btn_stop && gtk_widget_is_sensitive(btn_stop)) {
                g_signal_emit_by_name(btn_stop, "clicked");
                atomic_store_explicit(&seek_flag, false, memory_order_release);
                atomic_store_explicit(&get_transport_state()->playback_pos, 0,
                                      memory_order_release);
                atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);
                if (waveform_adj)
                    gtk_adjustment_set_value(waveform_adj, 0.0);
                refresh_track_time_display();
                if (waveform_area_bt)
                    gtk_widget_queue_draw(waveform_area_bt);
                if (waveform_area_input)
                    gtk_widget_queue_draw(waveform_area_input);
            }
        }
    }

    return G_SOURCE_CONTINUE;
}

static void start_recording_execution(void) {
    size_t current_bt_frames =
        atomic_load_explicit(&get_transport_state()->backing_track_frames, memory_order_acquire);
    size_t current_pos =
        atomic_load_explicit(&get_transport_state()->playback_pos, memory_order_acquire);

    int rate =
        atomic_load_explicit(&get_transport_state()->active_sample_rate, memory_order_acquire);
    if (rate <= 0)
        rate = 48000;

    // Rewind to 0 if at/past canvas end, or within the last 5 seconds of the canvas end
    if (current_bt_frames > 0 && (current_pos >= current_bt_frames ||
                                  current_pos + ((size_t)rate * 5) >= current_bt_frames)) {
        current_pos = 0;
        atomic_store_explicit(&seek_flag, false, memory_order_release);
        atomic_store_explicit(&get_transport_state()->playback_pos, 0, memory_order_release);
        atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);
    }

    if (btn_save_mux)
        gtk_widget_remove_css_class(btn_save_mux, "needs-save");
    if (btn_save_session)
        gtk_widget_remove_css_class(btn_save_session, "needs-save");

    mark_session_dirty();

    float old_speed =
        atomic_load_explicit(&get_transport_state()->playback_speed, memory_order_acquire);
    bool speed_was_reset = false;

    // Force playback speed back to 1.0x on record
    if (old_speed != 1.0f) {
        speed_was_reset = true;
    }

    if (speed_debounce_id != 0) {
        g_source_remove(speed_debounce_id);
        speed_debounce_id = 0;
    }
    pending_speed = 1.0f;
    set_playback_speed(1.0f);
    if (btn_speed)
        gtk_button_set_label(GTK_BUTTON(btn_speed), "Speed: 1.0x");

    if (current_bt_frames == 0) {
        int canvas_limit_min = ui_state.config.default_canvas_limit_min > 0
                                   ? ui_state.config.default_canvas_limit_min
                                   : 5;

        if (init_empty_loop_canvas(canvas_limit_min * 60) != 0) {
            log_and_notify(1, "Status: Insufficient RAM for blank canvas");
            return;
        }

        char track_lbl[128];
        snprintf(track_lbl, sizeof(track_lbl), "Track: Blank Loop Canvas [%02d:00 limit]",
                 canvas_limit_min);
        gtk_label_set_text(GTK_LABEL(lbl_track), track_lbl);
        update_multitrack_status_ui();
        update_zoom_button_label_to_length();
    }

    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);

    char base_name[256] = "Untracked";
    if (strlen(ui_state.selected_track_path) > 0) {
        char *tmp = g_path_get_basename(ui_state.selected_track_path);
        char *dot = strrchr(tmp, '.');
        if (dot)
            *dot = '\0';
        snprintf(base_name, sizeof(base_name), "%s", tmp);
        g_free(tmp);
    }

    bool is_ext_video = false;
    if (video_end_frame > video_start_frame && !has_active_live_video()) {
        is_ext_video = true;
    }

    if (atomic_load_explicit(&video_is_armed, memory_order_acquire)) {
        if (strlen(current_raw_path) > 0) {
            remove(current_raw_path);
        }

        clear_video_stash_async();

        if (!is_ext_video) {
            launch_mkv_video_preview("");
        }

        snprintf(current_raw_path, sizeof(current_raw_path),
                 "/tmp/qjams_raw_%04d%02d%02d_%02d%02d%02d.mkv", tm.tm_year + 1900, tm.tm_mon + 1,
                 tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    }

    int current_samplerate = client ? jack_get_sample_rate(client) : 48000;
    bool was_loop_wrap = atomic_exchange_explicit(&is_loop_wrapping, false, memory_order_acquire);

    if (await_rt_thread_detach()) {
        if (speed_was_reset && pristine_frames > 0) {
            extern _Atomic bool speed_change_flag;
            extern _Atomic bool stretcher_ratio_update;
            extern _Atomic bool stretcher_flush_request;

            atomic_store_explicit(&speed_change_flag, false, memory_order_release);
            atomic_store_explicit(&get_transport_state()->playback_speed, 1.0f,
                                  memory_order_release);
            atomic_store_explicit(&stretcher_ratio_update, true, memory_order_release);

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
            double v_start_frac =
                (bt_frames > 0) ? ((double)video_start_frame / (double)bt_frames) : 0.0;
            double v_end_frac =
                (bt_frames > 0) ? ((double)video_end_frame / (double)bt_frames) : 0.0;

            atomic_store_explicit(&get_transport_state()->backing_track_frames, pristine_frames,
                                  memory_order_release);
            current_pos = (size_t)(fraction * pristine_frames);
            if (current_pos >= pristine_frames)
                current_pos = 0;
            atomic_store_explicit(&get_transport_state()->playback_pos, current_pos,
                                  memory_order_release);

            if (atomic_load_explicit(&loop_active, memory_order_acquire)) {
                atomic_store_explicit(&loop_start_frame, (size_t)(loop_s_frac * pristine_frames),
                                      memory_order_relaxed);
                atomic_store_explicit(&loop_end_frame, (size_t)(loop_e_frac * pristine_frames),
                                      memory_order_relaxed);
            }

            video_start_frame = (size_t)(v_start_frac * pristine_frames);
            video_end_frame = (size_t)(v_end_frac * pristine_frames);

            atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);
        }
        int rec_track = atomic_load_explicit(&current_recording_track, memory_order_acquire);
        int current_tb = atomic_load_explicit(&timebase_track, memory_order_acquire);
        if ((rec_track == current_tb || was_loop_wrap) &&
            atomic_load_explicit(&master_tracks[rec_track].has_audio, memory_order_acquire)) {
            for (int t = 0; t < QJ_MAX_TRACKS; t++) {
                if (!atomic_load_explicit(&master_tracks[t].has_audio, memory_order_acquire)) {
                    rec_track = t;
                    atomic_store_explicit(&current_recording_track, rec_track,
                                          memory_order_release);
                    atomic_store_explicit(&selected_tracks_mask, (1U << rec_track),
                                          memory_order_release);
                    int active = atomic_load_explicit(&active_track_count, memory_order_acquire);
                    if (rec_track >= active) {
                        atomic_store_explicit(&active_track_count, rec_track + 1,
                                              memory_order_release);
                    }
                    break;
                }
            }
        }
        backup_track_to_ledger(rec_track);
        if (!atomic_load_explicit(&master_tracks[rec_track].has_audio, memory_order_acquire)) {
            atomic_store_explicit(&master_tracks[rec_track].gain, 1.0f, memory_order_relaxed);
        }
        if (atomic_load_explicit(&video_is_armed, memory_order_acquire)) {
            if (!is_ext_video) {
                video_start_frame = 0;
                video_end_frame = 0;
            }
        }
        resume_rt_thread();
    }

    if (atomic_load_explicit(&video_is_armed, memory_order_acquire)) {
        mark_current_transaction_as_video();

        if (init_and_start_encoder(current_raw_path, capture_width, capture_height,
                                   current_samplerate, &video_queue) != 0) {
            log_and_notify(1, "Status: Encoder Failed");
            return;
        }

        live_video_start_frame =
            atomic_load_explicit(&get_transport_state()->playback_pos, memory_order_acquire);
    }

    live_audio_start_frame =
        atomic_load_explicit(&get_transport_state()->playback_pos, memory_order_acquire);

    atomic_store_explicit(&get_transport_state()->is_armed, true, memory_order_release);
    atomic_store_explicit(&get_transport_state()->is_recording, true, memory_order_release);
    trigger_global_ui_refresh();
    if (is_ext_video || (!atomic_load_explicit(&video_is_armed, memory_order_acquire) &&
                         media_has_video(ui_state.selected_track_path) &&
                         access(ui_state.selected_track_path, F_OK) == 0)) {
        launch_mkv_video_preview(ui_state.selected_track_path);
    }
    if (!atomic_load_explicit(&get_transport_state()->is_playing, memory_order_acquire)) {
        atomic_store_explicit(&get_transport_state()->is_playing, true, memory_order_release);
    }

    if (speed_was_reset) {
        log_and_notify(1, "Status: Recording (Speed reset to 1.0x)");
    } else {
        log_and_notify(1, "Status: Recording");
    }

    bool is_video = atomic_load_explicit(&video_is_armed, memory_order_acquire);

    if (is_video) {
        gtk_button_set_label(GTK_BUTTON(btn_record), "🎜");
        gtk_widget_set_sensitive(btn_record, FALSE);
        if (btn_record_video) {
            gtk_button_set_label(GTK_BUTTON(btn_record_video), "⏸");
            gtk_widget_set_tooltip_text(btn_record_video, "Pause Video Recording");
            gtk_widget_set_sensitive(btn_record_video, TRUE);
        }
    } else {
        gtk_button_set_label(GTK_BUTTON(btn_record), "⏸");
        gtk_widget_set_tooltip_text(btn_record, "Pause Audio Recording");
        gtk_widget_set_sensitive(btn_record, TRUE);
        if (btn_record_video) {
            gtk_button_set_label(GTK_BUTTON(btn_record_video), "🖵");
            gtk_widget_set_sensitive(btn_record_video, FALSE);
        }
    }

    update_global_ui_locks(false, true);

    if (btn_play)
        gtk_widget_grab_focus(btn_play);
}

/**
 * @brief Returns true if the pre-record countdown modal is currently active.
 */
bool is_countdown_active(void) {
    return countdown_timer_id != 0;
}

static gboolean countdown_tick(gpointer user_data) {
    (void)user_data;
    EngineCommand cmd = {.type = CMD_PLAY_CLICK, .target_track = 0};

    if (countdown_val > 0) {
        char buf[128];
        snprintf(buf, sizeof(buf), "<span size='48000' weight='heavy'>%d</span>", countdown_val);
        gtk_label_set_markup(GTK_LABEL(countdown_label), buf);

        push_command(&cmd_queue, cmd);

        countdown_val--;
        return G_SOURCE_CONTINUE;
    } else if (countdown_val == 0) {
        gtk_label_set_markup(GTK_LABEL(countdown_label),
                             "<span size='48000' weight='heavy' foreground='#ff4444'>GO!</span>");

        push_command(&cmd_queue, cmd);

        countdown_val--;
        return G_SOURCE_CONTINUE;
    } else {
        start_recording_execution();
        countdown_timer_id = 0;

        if (countdown_window) {
            gtk_window_destroy(GTK_WINDOW(countdown_window));
            countdown_window = NULL;
        }

        return G_SOURCE_REMOVE;
    }
}

static void on_countdown_destroyed(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    (void)user_data;
    if (countdown_timer_id != 0) {
        g_source_remove(countdown_timer_id);
        countdown_timer_id = 0;
    }
    countdown_window = NULL;
    countdown_label = NULL;

    if (!atomic_load_explicit(&get_transport_state()->is_playing, memory_order_acquire) &&
        gtk_widget_is_sensitive(btn_stop)) {
        log_and_notify(1, "Status: Recording Aborted");
        update_global_ui_locks(false, false);
    }
}

static gboolean on_countdown_key_pressed(GtkEventControllerKey *controller, guint keyval,
                                         guint keycode, GdkModifierType state, gpointer user_data) {
    (void)controller;
    (void)keycode;
    (void)state;
    (void)user_data;
    if (keyval == GDK_KEY_Escape) {
        if (countdown_window) {
            gtk_window_destroy(GTK_WINDOW(countdown_window));
            countdown_window = NULL;
        }
        return TRUE;
    }
    return FALSE;
}

/**
 * @brief Initiates recording, either immediately (if playing) or after a 3-second countdown.
 */
void on_start_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    if (countdown_timer_id != 0)
        return;

    bool is_playing =
        atomic_load_explicit(&get_transport_state()->is_playing, memory_order_acquire);
    bool is_wrapping = atomic_load_explicit(&is_loop_wrapping, memory_order_acquire);
    bool bypass_countdown = is_wrapping && (ui_state.config.loop_countdown_active == 0);

    if (is_playing || bypass_countdown) {
        start_recording_execution();
        return;
    }
    gtk_widget_set_sensitive(btn_record, FALSE);
    if (btn_record_video)
        gtk_widget_set_sensitive(btn_record_video, FALSE);
    gtk_widget_set_sensitive(btn_play, FALSE);
    gtk_widget_set_sensitive(btn_prev, FALSE);
    gtk_widget_set_sensitive(btn_next, FALSE);
    gtk_widget_set_sensitive(btn_stop, TRUE);
    log_and_notify(1, "Status: Get Ready...");

    countdown_window = gtk_window_new();
    gtk_window_set_transient_for(GTK_WINDOW(countdown_window),
                                 GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(btn_record))));
    gtk_window_set_decorated(GTK_WINDOW(countdown_window), FALSE);
    gtk_window_set_modal(GTK_WINDOW(countdown_window), TRUE);

    GtkEventController *countdown_key_ctrl = gtk_event_controller_key_new();
    g_signal_connect(countdown_key_ctrl, "key-pressed", G_CALLBACK(on_countdown_key_pressed), NULL);
    gtk_widget_add_controller(countdown_window, countdown_key_ctrl);

    g_signal_connect(countdown_window, "destroy", G_CALLBACK(on_countdown_destroyed), NULL);

    countdown_label = gtk_label_new(NULL);
    gtk_widget_set_margin_start(countdown_label, 80);
    gtk_widget_set_margin_end(countdown_label, 80);
    gtk_widget_set_margin_top(countdown_label, 50);
    gtk_widget_set_margin_bottom(countdown_label, 50);
    gtk_window_set_child(GTK_WINDOW(countdown_window), countdown_label);

    countdown_val = 3;
    char buf[128];
    snprintf(buf, sizeof(buf), "<span size='48000' weight='heavy'>%d</span>", countdown_val);
    gtk_label_set_markup(GTK_LABEL(countdown_label), buf);
    countdown_val--;

    gtk_window_present(GTK_WINDOW(countdown_window));
    countdown_timer_id = g_timeout_add(1000, countdown_tick, NULL);
}

/**
 * @brief Stops playback/recording, commits recorded audio/parity state, and halts the encoder.
 */
void on_stop_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    bool was_recording =
        atomic_load_explicit(&get_transport_state()->is_recording, memory_order_acquire);
    bool wrapping = atomic_load_explicit(&is_loop_wrapping, memory_order_acquire);
    if (countdown_timer_id != 0) {
        if (countdown_window)
            gtk_window_destroy(GTK_WINDOW(countdown_window));
        gtk_label_set_text(GTK_LABEL(lbl_status), "Status: Recording Aborted");

        update_global_ui_locks(false, false);

        atomic_store_explicit(&get_transport_state()->is_playing, false, memory_order_release);
        atomic_store_explicit(&get_transport_state()->is_recording, false, memory_order_release);

        clear_loop_points();
        if (waveform_area_bt)
            gtk_widget_queue_draw(waveform_area_bt);
        if (waveform_area_input)
            gtk_widget_queue_draw(waveform_area_input);

        return;
    }

    if (!wrapping) {
        clear_loop_points();
    }
    bool detached = await_rt_thread_detach();

    atomic_store_explicit(&get_transport_state()->is_playing, false, memory_order_release);
    atomic_store_explicit(&get_transport_state()->is_recording, false, memory_order_release);
    atomic_store_explicit(&get_transport_state()->is_armed, false, memory_order_release);
    atomic_store_explicit(&get_transport_state()->is_paused, false, memory_order_release);

    size_t current_pos =
        atomic_load_explicit(&get_transport_state()->playback_pos, memory_order_acquire);

    if (pristine_frames > 0 && current_pos > pristine_frames) {
        current_pos = pristine_frames;
        atomic_store_explicit(&get_transport_state()->playback_pos, current_pos,
                              memory_order_release);
    }

    if (detached) {
        if (was_recording && current_pos > 0) {
            int current_rec = atomic_load_explicit(&current_recording_track, memory_order_acquire);

            if (atomic_load_explicit(&video_is_armed, memory_order_acquire)) {
                video_start_frame = live_video_start_frame;
                video_end_frame = current_pos;
                video_track_index = current_rec;
            }

            atomic_store_explicit(&master_tracks[current_rec].has_audio, true,
                                  memory_order_release);
            atomic_store_explicit(&selected_tracks_mask, (1 << current_rec), memory_order_release);
            int cur_active = atomic_load_explicit(&active_track_count, memory_order_acquire);
            if (current_rec >= cur_active) {
                atomic_store_explicit(&active_track_count, current_rec + 1, memory_order_release);
            }

            size_t rec_start = live_audio_start_frame;
            if (current_pos > rec_start && master_tracks[current_rec].parity_map) {
                size_t rec_len = current_pos - rec_start;
                uint8_t last_parity = 0;
                if (rec_start > 0) {
                    last_parity = master_tracks[current_rec].parity_map[rec_start - 1];
                }
                uint8_t next_parity = (last_parity == 0 || last_parity % 2 == 0) ? 1 : 2;
                memset(&master_tracks[current_rec].parity_map[rec_start], next_parity,
                       rec_len * sizeof(uint8_t));
            }

            atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);

            size_t fade_len = (current_pos < 256) ? current_pos : 256;
            if (fade_len > 1) {
                for (size_t f = 0; f < fade_len; f++) {
                    float multiplier = (float)(fade_len - 1 - f) / (float)(fade_len - 1);
                    size_t idx = current_pos - fade_len + f;

                    if (current_rec >= 0 && current_rec < QJ_MAX_TRACKS &&
                        master_tracks[current_rec].active_buffer && idx < pristine_frames) {
                        master_tracks[current_rec].active_buffer[idx * 2] *= multiplier;
                        master_tracks[current_rec].active_buffer[idx * 2 + 1] *= multiplier;
                    }
                }
            }

            update_zoom_button_label_to_length();
            invalidate_waveform_caches();
        }
        resume_rt_thread();
        if (was_recording) {
            trigger_audible_scan_async();
        }
    }

    update_multitrack_status_ui();
    invalidate_waveform_caches();
    if (waveform_area_bt)
        gtk_widget_queue_draw(waveform_area_bt);
    if (waveform_area_input)
        gtk_widget_queue_draw(waveform_area_input);

    stop_encoder();
    if (has_active_live_video()) {
        launch_mkv_video_preview(current_raw_path);
    } else if (media_has_video(ui_state.selected_track_path) &&
               access(ui_state.selected_track_path, F_OK) == 0) {
        launch_mkv_video_preview(ui_state.selected_track_path);
    } else {
        launch_mkv_video_preview("");
    }

    log_and_notify(1, "Status: Ready");
    gtk_button_set_icon_name(GTK_BUTTON(btn_play), "");
    gtk_button_set_label(GTK_BUTTON(btn_play), "▶");
    gtk_widget_set_tooltip_text(btn_play, "Play (Space)");

    gtk_button_set_label(GTK_BUTTON(btn_record), "🎜 ");
    gtk_widget_set_tooltip_text(btn_record, "Record Audio (R)");
    if (btn_record_video) {
        gtk_button_set_label(GTK_BUTTON(btn_record_video), "🖵");
        gtk_widget_set_tooltip_text(btn_record_video, "Record Audio + Video");
    }

    update_global_ui_locks(false, false);

    if (was_recording) {
        mark_session_dirty();

        if (btn_save_mux) {
            gtk_widget_remove_css_class(btn_save_mux, "needs-save");
            gtk_widget_add_css_class(btn_save_mux, "needs-save");
        }
        if (btn_save_session) {
            gtk_widget_remove_css_class(btn_save_session, "needs-save");
            gtk_widget_add_css_class(btn_save_session, "needs-save");
        }
    }

    if (btn_play)
        gtk_widget_grab_focus(btn_play);
}

typedef struct {
    int duration_sec;
    char next_track_path[1024];
} ResetCanvasPayload;

static void reset_canvas_thread(GTask *task, gpointer source_object, gpointer task_data,
                                GCancellable *cancellable) {
    (void)source_object;
    (void)cancellable;
    ResetCanvasPayload *rcp = (ResetCanvasPayload *)task_data;

    int status = init_empty_loop_canvas(rcp->duration_sec);

    g_task_return_int(task, status);
}

static void reset_canvas_ready(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    (void)source_object;
    (void)user_data;
    int status = g_task_propagate_int(G_TASK(res), NULL);
    ResetCanvasPayload *rcp = (ResetCanvasPayload *)g_task_get_task_data(G_TASK(res));

    reset_multitrack_ui_states();
    refresh_multitrack_tracks_ui();

    trigger_global_ui_refresh();

    if (btn_play)
        gtk_widget_set_sensitive(btn_play, FALSE);
    if (btn_record)
        gtk_widget_set_sensitive(btn_record, TRUE);

    GtkWindow *win =
        gtk_application_get_active_window(GTK_APPLICATION(g_application_get_default()));
    if (win) {
        GtkWidget *master_box = gtk_window_get_child(win);
        if (master_box)
            gtk_widget_set_sensitive(master_box, TRUE);
    }

    gtk_spinner_stop(GTK_SPINNER(main_spinner));
    set_app_waiting_state(false);

    if (status == 0) {
        mark_session_saved();
        ui_state.is_existing_session = false;

        if (strlen(rcp->next_track_path) > 0) {
            extern void command_post_load_session(const char *path);
            if (g_str_has_suffix(rcp->next_track_path, ".qjams")) {
                command_post_load_session(rcp->next_track_path);
            } else {
                int rec_track =
                    atomic_load_explicit(&current_recording_track, memory_order_acquire);
                multitrack_import_jukebox_async(rcp->next_track_path, rec_track);
            }
        } else {
            int limit = ui_state.config.default_canvas_limit_min > 0
                            ? ui_state.config.default_canvas_limit_min
                            : 5;
            char track_lbl[128];
            snprintf(track_lbl, sizeof(track_lbl), "Track: Blank Loop Canvas [%02d:00 limit]",
                     limit);
            if (lbl_track)
                gtk_label_set_text(GTK_LABEL(lbl_track), track_lbl);
            log_and_notify(1, "Status: Recording Canvas Reset");
        }
    } else if (status == -2) {
        log_and_notify(
            1,
            "<span foreground='#ff4444'><b>Status: Insufficient RAM for blank canvas</b></span>");
    } else {
        log_and_notify(1, "<span foreground='#ff4444'><b>Status: Audio Engine Timeout (Reset "
                          "Aborted)</b></span>");
    }
}

static void on_global_reset_confirm(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    (void)user_data;
    GtkAlertDialog *alert = GTK_ALERT_DIALOG(source_object);
    int response = gtk_alert_dialog_choose_finish(alert, res, NULL);

    if (response == 0) {
        execute_global_canvas_reset(NULL);
    }
}

void on_global_reset_clicked(GtkButton *button, gpointer user_data) {
    (void)user_data;
    if (atomic_load_explicit(&get_transport_state()->is_recording, memory_order_acquire))
        return;

    if (!is_session_dirty() || pristine_frames == 0) {
        execute_global_canvas_reset(NULL);
        return;
    }

    GtkAlertDialog *alert = gtk_alert_dialog_new("Reset Recording Canvas?");
    gtk_alert_dialog_set_detail(alert, "This will completely clear your recorded canvas, remove "
                                       "any loaded tracks, and start from scratch. Are you sure?");
    const char *buttons[] = {"Reset Canvas", "Cancel", NULL};
    gtk_alert_dialog_set_buttons(alert, buttons);
    gtk_alert_dialog_set_cancel_button(alert, 1);
    gtk_alert_dialog_set_default_button(alert, 0);

    GtkWindow *parent = GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(button)));
    gtk_alert_dialog_choose(alert, parent, NULL, on_global_reset_confirm, NULL);
    g_object_unref(alert);
}

/**
 * @brief Queues an asynchronous background wipe of the canvas, halting the GTK main loop safely.
 */
void execute_global_canvas_reset(const char *next_track_path) {
    purge_autosave_file();

    on_stop_clicked(NULL, NULL);
    clear_loop_points();
    zoom_multiplier = 1.0;
    if (waveform_adj)
        gtk_adjustment_set_value(waveform_adj, 0.0);
    abort_active_video_preview(false);
    force_clear_mkv_video_preview();

    video_start_frame = 0;
    video_end_frame = 0;
    video_track_index = -1;
    atomic_store_explicit(&timebase_track, 0, memory_order_release);
    atomic_store_explicit(&video_is_armed, false, memory_order_release);

    ui_state.selected_track_path[0] = '\0';
    update_playlist_toggle_state();

    if (speed_debounce_id != 0) {
        g_source_remove(speed_debounce_id);
        speed_debounce_id = 0;
    }
    pending_speed = 1.0f;
    set_playback_speed(1.0f);
    if (btn_speed)
        gtk_button_set_label(GTK_BUTTON(btn_speed), "Speed: 1.0x");

    for (int i = 0; i < QJ_MAX_TRACKS; i++) {
        set_multitrack_layer_gain(i, 1.0f);
    }

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

    gtk_spinner_start(GTK_SPINNER(main_spinner));
    set_app_waiting_state(true);
    log_and_notify(1, "Status: Wiping canvas memory...");

    ResetCanvasPayload *rcp = g_new0(ResetCanvasPayload, 1);
    rcp->duration_sec = ui_state.config.default_canvas_limit_min > 0
                            ? ui_state.config.default_canvas_limit_min * 60
                            : 300;
    if (next_track_path) {
        strncpy(rcp->next_track_path, next_track_path, sizeof(rcp->next_track_path) - 1);
    }

    GTask *task = g_task_new(NULL, NULL, reset_canvas_ready, NULL);
    g_task_set_task_data(task, rcp, g_free);
    g_task_run_in_thread(task, reset_canvas_thread);
    g_object_unref(task);
}

/**
 * @brief Starts or pauses/resumes multi-track playback.
 */
void on_play_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;

    bool is_playing =
        atomic_load_explicit(&get_transport_state()->is_playing, memory_order_acquire);
    bool is_paused = atomic_load_explicit(&get_transport_state()->is_paused, memory_order_acquire);
    bool is_recording =
        atomic_load_explicit(&get_transport_state()->is_recording, memory_order_acquire);

    // Trap: The play button does nothing if we are actively recording
    if (is_recording)
        return;

    bool any_audio = false;
    for (int i = 0; i < QJ_MAX_TRACKS; i++) {
        if (atomic_load_explicit(&master_tracks[i].has_audio, memory_order_acquire)) {
            any_audio = true;
            break;
        }
    }
    if (!any_audio && !is_playing && !is_paused)
        return;

    if (!is_playing && !is_paused) {
        size_t current_pos =
            atomic_load_explicit(&get_transport_state()->playback_pos, memory_order_acquire);
        size_t bt_frames = atomic_load_explicit(&get_transport_state()->backing_track_frames,
                                                memory_order_acquire);
        if (bt_frames > 0 && current_pos >= bt_frames)
            seek_backing_track(0.0);

        // Route either the active temp recording, or the imported MKV track
        char *video_to_play = NULL;

        if (access(current_raw_path, F_OK) == 0) {
            video_to_play = g_strdup(current_raw_path);
        } else if (media_has_video(ui_state.selected_track_path) &&
                   access(ui_state.selected_track_path, F_OK) == 0) {
            video_to_play = g_strdup(ui_state.selected_track_path);
        }

        if (video_to_play) {
            launch_mkv_video_preview(video_to_play);
            g_free(video_to_play);
        } else {
            launch_mkv_video_preview("");
        }

        atomic_store_explicit(&get_transport_state()->is_playing, true, memory_order_release);
        log_and_notify(1, "Status: PLAYING");
        gtk_button_set_icon_name(GTK_BUTTON(btn_play), "");
        gtk_button_set_label(GTK_BUTTON(btn_play), "⏸");
        gtk_widget_set_tooltip_text(btn_play, "Pause (Space)");

        update_global_ui_locks(true, false);
    } else if (is_playing) {
        bool new_pause_state = !is_paused;
        atomic_store_explicit(&get_transport_state()->is_paused, new_pause_state,
                              memory_order_release);

        if (new_pause_state) {
            log_and_notify(1, "Status: PAUSED");
            gtk_button_set_icon_name(GTK_BUTTON(btn_play), "");
            gtk_button_set_label(GTK_BUTTON(btn_play), "▶");
            gtk_widget_set_tooltip_text(btn_play, "Resume (Space)");
        } else {
            log_and_notify(1, "Status: PLAYING");
            gtk_button_set_icon_name(GTK_BUTTON(btn_play), "");
            gtk_button_set_label(GTK_BUTTON(btn_play), "⏸");
            gtk_widget_set_tooltip_text(btn_play, "Pause (Space)");
        }
    }
}

static void toggle_recording_pause(void) {
    bool is_paused = atomic_load_explicit(&get_transport_state()->is_paused, memory_order_acquire);
    bool new_pause_state = !is_paused;
    atomic_store_explicit(&get_transport_state()->is_paused, new_pause_state, memory_order_release);
    bool is_video = atomic_load_explicit(&video_is_armed, memory_order_acquire);

    if (new_pause_state) {
        log_and_notify(1, "Status: RECORDING PAUSED");
        if (is_video) {
            if (btn_record_video)
                gtk_button_set_label(GTK_BUTTON(btn_record_video), "🖵");
        } else {
            gtk_button_set_label(GTK_BUTTON(btn_record), "🎜");
        }
    } else {
        log_and_notify(1, "Status: Recording");
        if (is_video) {
            if (btn_record_video)
                gtk_button_set_label(GTK_BUTTON(btn_record_video), "⏸");
        } else {
            gtk_button_set_label(GTK_BUTTON(btn_record), "⏸");
        }
    }
}

/**
 * @brief Arms audio-only recording or toggles recording pause if already recording.
 */
void on_record_audio_clicked(GtkButton *button, gpointer user_data) {
    if (atomic_load_explicit(&get_transport_state()->is_recording, memory_order_acquire)) {
        toggle_recording_pause();
        return;
    }
    atomic_store_explicit(&video_is_armed, false, memory_order_release);
    on_start_clicked(button, user_data);
}

static void on_video_overwrite_response(GObject *source_object, GAsyncResult *res,
                                        gpointer user_data) {
    GtkAlertDialog *alert = GTK_ALERT_DIALOG(source_object);
    int response = gtk_alert_dialog_choose_finish(alert, res, NULL);

    if (response == 0) {
        atomic_store_explicit(&video_is_armed, true, memory_order_release);
        on_start_clicked(GTK_BUTTON(user_data), NULL);
    }
}

/**
 * @brief Arms audio+video recording (prompting on existing video overwrite) or toggles pause.
 */
void on_record_video_clicked(GtkButton *button, gpointer user_data) {
    if (atomic_load_explicit(&get_transport_state()->is_recording, memory_order_acquire)) {
        toggle_recording_pause();
        return;
    }

    if (!atomic_load_explicit(&suppress_video_overwrite, memory_order_acquire)) {
        if (has_active_live_video()) {
            GtkAlertDialog *alert = gtk_alert_dialog_new("Overwrite Existing Video Take?");
            gtk_alert_dialog_set_detail(
                alert, "A video take already exists in this session. Recording video again will "
                       "discard your previous video take.\n\nDo you want to overwrite it, or "
                       "cancel to Export/Mux your current mix first?");

            const char *buttons[] = {"Overwrite", "Cancel", NULL};
            gtk_alert_dialog_set_buttons(alert, buttons);
            gtk_alert_dialog_set_cancel_button(alert, 1);
            gtk_alert_dialog_set_default_button(alert, 1);

            GtkWindow *parent = GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(button)));
            gtk_alert_dialog_choose(alert, parent, NULL, on_video_overwrite_response, button);
            g_object_unref(alert);
            return;
        }
    }

    atomic_store_explicit(&video_is_armed, true, memory_order_release);
    atomic_store_explicit(&suppress_video_overwrite, false, memory_order_release);
    on_start_clicked(button, user_data);
}
