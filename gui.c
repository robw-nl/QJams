#include <gtk/gtk.h>
#include <glib-unix.h>   // For g_unix_fd_add
#include <jack/jack.h>
#include <math.h>
#include <stdbool.h>
#include <signal.h>
#include <sys/stat.h>
#include "audio_engine.h"
#include "video_engine.h"
#include "scanner.h"
#include "config.h"
#include "ui_globals.h"
#include "ui_playlist.h"
#include "ui_waveforms.h"
#include "ui_multitrack.h"
#include "ui_dashboard.h"
#include "ui_command_post.h"
#include <unistd.h>

extern jack_client_t *client;
extern SPSC_Video_Queue video_queue;
SPSC_Preview_Queue preview_queue;

QJamsUIState ui_state = { .is_loading_track = false, .selected_track_path = "", .default_canvas_limit_min = 5, .is_existing_session = false };

GtkWidget *lbl_input_device;
GtkWidget *input_gain_spinner;
GtkWidget *lbl_track;
GtkWidget *lbl_status;
GtkWidget *lbl_hw_toast;
GtkWidget *lbl_roadmap;
GtkWidget *main_spinner;
GtkWidget *btn_play;
GtkWidget *btn_record;
GtkWidget *btn_playlist_toggle;
GtkWidget *waveform_area_bt;
GtkWidget *waveform_area_input;

GtkWidget *btn_zoom;
GtkWidget *waveform_scrollbar;
GtkAdjustment *waveform_adj;
double zoom_multiplier = 1.0;

GtkWidget *left_column_box;
GtkWidget *right_vbox;
GtkWidget *command_post_box;
GtkWidget *settings_dialog;

GtkWidget *lbl_multitrack_status;
GtkWidget *btn_multitrack_undo;
GtkWidget *btn_multitrack_next;

GtkWidget *btn_load;
GtkWidget *btn_save_mux;
GtkWidget *btn_save_session;
GtkWidget *btn_settings;
GtkWidget *btn_prev;
GtkWidget *btn_stop;
GtkWidget *btn_next;
GtkWidget *btn_speed;
GtkWidget *btn_reset;

static GtkWidget *video_preview;
GtkWidget *video_preview_frame = NULL;

static guint routing_timeout_id = 0;
static guint daemon_retry_id = 0;

static gboolean delayed_routing_cb(gpointer user_data) {
    (void)user_data;
    routing_timeout_id = 0;
    log_and_notify(0, "[GTK TIMER] Port debounce finished. Executing graph validation...\n");

    // 1. Recover Audio Graph
    extern void update_dashboard_pref_ui(void);
    update_dashboard_pref_ui();

    // 2. Recover Video Graph (V4L2 Zombie Node Pruning)
    VideoDevice v_devs[MAX_CAMERAS];
    int v_count = scan_video_devices(v_devs, MAX_CAMERAS);

    if (v_count > 0) {
        bool video_online = false;
        for (int i = 0; i < v_count; i++) {
            if (strcmp(ui_state.config.video_device, v_devs[i].device_path) == 0) {
                video_online = true;
                break;
            }
        }

        if (!video_online) {
            log_and_notify(0, "[HW-HEAL] Video capture lost. Auto-recovering to %s\n", v_devs[0].device_path);
            strncpy(ui_state.config.video_device, v_devs[0].device_path, sizeof(ui_state.config.video_device) - 1);
            ui_state.config.video_device[sizeof(ui_state.config.video_device) - 1] = '\0';

            stop_video_engine();
            init_and_start_video_engine(ui_state.config.video_device, &video_queue);
            save_qjams_config(ui_state.config_path, &ui_state.config);
        }
        if (video_preview_frame) gtk_widget_set_visible(video_preview_frame, TRUE);
        extern GtkWidget *btn_record_video;
        if (btn_record_video) gtk_widget_set_sensitive(btn_record_video, TRUE);
    } else {
        if (video_preview_frame) gtk_widget_set_visible(video_preview_frame, FALSE);
        extern GtkWidget *btn_record_video;
        if (btn_record_video) gtk_widget_set_sensitive(btn_record_video, FALSE);
        extern _Atomic bool video_is_armed;
        atomic_store_explicit(&video_is_armed, false, memory_order_release);
    }

    return G_SOURCE_REMOVE;
}

static gboolean daemon_retry_cb(gpointer user_data) {
    (void)user_data;
    extern int reboot_audio_engine(const char*, const char*, float);
    int status = reboot_audio_engine(ui_state.config.audio_device, ui_state.config.playback_target, ui_state.config.input_gain_multiplier);
    if (status != -1) {
        log_and_notify(0, "[DAEMON] Audio Daemon Restored.\n");
        daemon_retry_id = 0;
        extern void update_dashboard_pref_ui(void);
        update_dashboard_pref_ui();
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

gboolean on_jack_pipe_event(gint fd, GIOCondition condition, gpointer user_data) {
    (void)condition; (void)user_data;
    uint8_t event;
    while (read(fd, &event, sizeof(event)) > 0) {
        if (event == 1) {
            log_and_notify(0, "[IPC] Daemon crash detected.\n");
            if (routing_timeout_id > 0) g_source_remove(routing_timeout_id);
            routing_timeout_id = 0;
            on_stop_clicked(NULL, NULL);
            log_and_notify(1, "<span foreground='#ff4444'><b>Status: FATAL - Audio Daemon Offline. Retrying...</b></span>");
            if (daemon_retry_id == 0) daemon_retry_id = g_timeout_add(1000, daemon_retry_cb, NULL);
        } else if (event == 2 || event == 3) {
            log_and_notify(0, "[IPC] Topology change detected (%s).\n", event == 2 ? "ADDED" : "REMOVED");
            if (daemon_retry_id == 0) {
                if (routing_timeout_id > 0) g_source_remove(routing_timeout_id);

                bool device_missing = false;
                if (event == 3) {
                    AudioDevice active_devs[MAX_AUDIO_DEVICES];
                    int online_count = scan_audio_devices(client, active_devs, MAX_AUDIO_DEVICES);
                    bool found = false;
                    for (int i = 0; i < online_count; i++) {
                        if (strcmp(ui_state.config.audio_device, active_devs[i].display_name) == 0) {
                            found = true; break;
                        }
                    }
                    if (!found) device_missing = true;
                }

                if (event == 3 && device_missing) {
                    // SAFETY [Critical]: Drop connections immediately ONLY if our active hardware vanished
                    extern void qjams_drop_connections(void);
                    qjams_drop_connections();
                }
                routing_timeout_id = g_timeout_add(300, delayed_routing_cb, NULL);
            }
        }
    }
    return G_SOURCE_CONTINUE;
}

static void on_force_quit_response(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    GtkAlertDialog *alert = GTK_ALERT_DIALOG(source_object);
    int response = gtk_alert_dialog_choose_finish(alert, res, NULL);

    if (response == 0) {
        if (active_muxer_pid != 0) {
            kill(active_muxer_pid, SIGKILL);
            active_muxer_pid = 0;
        }
        GtkWidget *win = GTK_WIDGET(user_data);
        gtk_window_destroy(GTK_WINDOW(win));
    }
}

static bool force_quit_approved = false;
bool pending_quit_after_save = false;

static void on_unsaved_quit_response(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    GtkAlertDialog *alert = GTK_ALERT_DIALOG(source_object);
    int response = gtk_alert_dialog_choose_finish(alert, res, NULL);
    GtkWidget *win = GTK_WIDGET(user_data);

    if (response == 0) {
        pending_quit_after_save = true;
        extern GtkWidget *btn_save_session;
        if (btn_save_session) g_signal_emit_by_name(btn_save_session, "clicked");
    } else if (response == 1) {
        char autosave_path[1024];
        snprintf(autosave_path, sizeof(autosave_path), "%s/.qjams_autosave.qjams", ui_state.config.recordings_dir);
        remove(autosave_path);
        mark_session_saved();

        // Explicitly wipe un-exported video since the session is abandoned
        extern void abort_active_video_preview(bool stash);
        abort_active_video_preview(false);

        force_quit_approved = true;
        gtk_window_destroy(GTK_WINDOW(win));
    }
}

static void on_recording_quit_response(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    GtkAlertDialog *alert = GTK_ALERT_DIALOG(source_object);
    int response = gtk_alert_dialog_choose_finish(alert, res, NULL);
    GtkWidget *win = GTK_WIDGET(user_data);

    if (response == 0) {
        on_stop_clicked(NULL, NULL);
        force_quit_approved = true;
        gtk_window_destroy(GTK_WINDOW(win));
    }
}

void purge_autosave_file(void) {
    char autosave_path[1024];
    snprintf(autosave_path, sizeof(autosave_path), "%s/.qjams_autosave.qjams", ui_state.config.recordings_dir);
    remove(autosave_path);
}

void trigger_global_ui_refresh(void) {
    extern void update_multitrack_status_ui(void);
    update_multitrack_status_ui();

    extern void update_zoom_button_label_to_length(void);
    update_zoom_button_label_to_length();

    extern void invalidate_waveform_caches(void);
    invalidate_waveform_caches();

    extern GtkWidget *waveform_area_bt;
    extern GtkWidget *waveform_area_input;
    if (waveform_area_bt) gtk_widget_queue_draw(waveform_area_bt);
    if (waveform_area_input) gtk_widget_queue_draw(waveform_area_input);
}

void set_app_waiting_state(bool waiting) {
    GtkWindow *win = gtk_application_get_active_window(GTK_APPLICATION(g_application_get_default()));
    if (!win) return;

    if (waiting) {
        GdkCursor *cursor = gdk_cursor_new_from_name("wait", NULL);
        gtk_widget_set_cursor(GTK_WIDGET(win), cursor);
        g_object_unref(cursor);
    } else {
        gtk_widget_set_cursor(GTK_WIDGET(win), NULL);
    }
}

static gboolean on_window_close(GtkWindow *window, gpointer user_data) {
    (void)user_data;

    if (force_quit_approved) {
        ui_state.config.window_width = gtk_widget_get_width(GTK_WIDGET(window));
        ui_state.config.window_height = gtk_widget_get_height(GTK_WIDGET(window));
        save_qjams_config(ui_state.config_path, &ui_state.config);
        return FALSE;
    }

    if (atomic_load_explicit(&engine_is_recording, memory_order_acquire)) {
        GtkAlertDialog *alert = gtk_alert_dialog_new("Recording in Progress");
        gtk_alert_dialog_set_detail(alert, "A recording is currently active. Do you want to stop recording and quit, or cancel?");
        const char *buttons[] = { "Stop & Quit", "Cancel", NULL };
        gtk_alert_dialog_set_buttons(alert, buttons);
        gtk_alert_dialog_set_cancel_button(alert, 1);
        gtk_alert_dialog_set_default_button(alert, 1);

        gtk_alert_dialog_choose(alert, GTK_WINDOW(window), NULL, on_recording_quit_response, window);
        g_object_unref(alert);
        return TRUE;
    }

    if (active_muxer_pid != 0) {
        GtkAlertDialog *alert = gtk_alert_dialog_new("Muxing in Progress");
        gtk_alert_dialog_set_detail(alert, "FFmpeg is currently processing the export in the background.\n\nDo you want to wait for it to finish, or Force Quit and abandon the export?");
        const char *buttons[] = { "Force Quit", "Wait", NULL };
        gtk_alert_dialog_set_buttons(alert, buttons);
        gtk_alert_dialog_set_cancel_button(alert, 1);
        gtk_alert_dialog_set_default_button(alert, 1);

        gtk_alert_dialog_choose(alert, GTK_WINDOW(window), NULL, on_force_quit_response, window);
        g_object_unref(alert);
        return TRUE;
    }

    // Safely blocks the UI thread until the encoder pipeline flushes remaining chunks to disk
    if (atomic_load_explicit(&engine_is_recording, memory_order_acquire)) {
        on_stop_clicked(NULL, NULL);
        mark_session_dirty();
    }

    if (is_session_dirty() && pristine_frames > 0) {
        GtkAlertDialog *alert = gtk_alert_dialog_new("Unsaved Changes");
        gtk_alert_dialog_set_detail(alert, "You have unsaved audio overdubs on the canvas. If you quit now, your progress will be permanently lost.");

        const char *buttons[] = { "Save Session", "Discard & Quit", "Cancel", NULL };
        gtk_alert_dialog_set_buttons(alert, buttons);
        gtk_alert_dialog_set_cancel_button(alert, 2);
        gtk_alert_dialog_set_default_button(alert, 0);

        gtk_alert_dialog_choose(alert, GTK_WINDOW(window), NULL, on_unsaved_quit_response, window);
        g_object_unref(alert);
        return TRUE;
    }

    ui_state.config.window_width = gtk_widget_get_width(GTK_WIDGET(window));
    ui_state.config.window_height = gtk_widget_get_height(GTK_WIDGET(window));
    save_qjams_config(ui_state.config_path, &ui_state.config);
    return FALSE;
}

static void apply_keyboard_seek(double step_sign, GdkModifierType state) {
    if (atomic_load_explicit(&engine_is_recording, memory_order_acquire)) return;

    size_t frames = atomic_load_explicit(&backing_track_frames, memory_order_acquire);
    if (frames > 0) {
        extern double zoom_multiplier;

        // DYNAMIC ZOOM PROPORTIONALITY
        // Jumps exactly 1% of the currently visible timeline width (100 steps per screen)
        double step_fraction = 0.01 / zoom_multiplier;

        double current;
        if (atomic_load_explicit(&seek_flag, memory_order_acquire)) {
            current = atomic_load_explicit(&seek_target, memory_order_relaxed);
        } else {
            current = (double)atomic_load_explicit(&playback_pos, memory_order_acquire) / (double)frames;
        }

        double target = current + (step_fraction * step_sign);

        if (state & GDK_SHIFT_MASK) {
            // --- REGION SELECTION MODE ---
            double anchor;
            extern _Atomic bool loop_active;
            if (atomic_load_explicit(&loop_active, memory_order_acquire)) {
                extern _Atomic size_t loop_start_frame;
                extern _Atomic size_t loop_end_frame;
                double l_start = (double)atomic_load_explicit(&loop_start_frame, memory_order_relaxed) / (double)frames;
                double l_end = (double)atomic_load_explicit(&loop_end_frame, memory_order_relaxed) / (double)frames;

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

            extern void set_loop_points(double, double);
            set_loop_points(anchor, target);

            // Force redraw to visually update the blue loop region
            extern GtkWidget *waveform_area_bt;
            extern GtkWidget *waveform_area_input;
            if (waveform_area_bt) gtk_widget_queue_draw(waveform_area_bt);
            if (waveform_area_input) gtk_widget_queue_draw(waveform_area_input);
        } else {
            // --- STANDARD SEEK MODE ---
            if (step_sign < 0.0) {
                target = fmax(0.0, target);
                extern _Atomic bool loop_active;
                if (atomic_load_explicit(&loop_active, memory_order_acquire)) {
                    extern _Atomic size_t loop_start_frame;
                    double l_start = (double)atomic_load_explicit(&loop_start_frame, memory_order_relaxed) / (double)frames;
                    target = fmax(l_start, target);
                }
            } else {
                target = fmin(1.0, target);
                extern _Atomic bool loop_active;
                if (atomic_load_explicit(&loop_active, memory_order_acquire)) {
                    extern _Atomic size_t loop_end_frame;
                    double l_end = (double)atomic_load_explicit(&loop_end_frame, memory_order_relaxed) / (double)frames;
                    target = fmin(l_end, target);
                }
            }
        }

        extern void seek_backing_track(double);
        seek_backing_track(target);
    }
}

static gboolean update_video_preview(GtkWidget *widget, GdkFrameClock *frame_clock, gpointer user_data) {
    (void)widget; (void)frame_clock; (void)user_data;
    PreviewPayload payload;
    PreviewPayload last_payload;
    int frames_popped = 0;

    while (pop_preview_frame(&preview_queue, &payload)) {
        last_payload = payload;
        frames_popped++;
    }

    if (frames_popped > 0) {
        uint8_t *pixel_copy = g_memdup2(last_payload.rgba_data, last_payload.width * last_payload.height * 4);
        GBytes *bytes = g_bytes_new_take(pixel_copy, last_payload.width * last_payload.height * 4);
        GdkTexture *texture = gdk_memory_texture_new(last_payload.width, last_payload.height, GDK_MEMORY_R8G8B8A8, bytes, last_payload.width * 4);
        gtk_picture_set_paintable(GTK_PICTURE(widget), GDK_PAINTABLE(texture));
        g_object_unref(texture);
        g_bytes_unref(bytes);
    }
    return G_SOURCE_CONTINUE;
}

static bool is_time_shift_safe(uint32_t mask, bool is_global_edit) {
    extern char current_raw_path[1024];
    bool has_video = false;
    if (strlen(current_raw_path) > 0 && access(current_raw_path, F_OK) == 0) has_video = true;
    if (g_str_has_suffix(ui_state.selected_track_path, ".mkv") || g_str_has_suffix(ui_state.selected_track_path, ".MKV")) has_video = true;

    if (!has_video) return true;
    if (is_global_edit) return false;

    int total_audio_tracks = 0;
    int selected_audio_tracks = 0;

    for (int i = 0; i < MAX_TRACKS; i++) {
        if (atomic_load_explicit(&master_tracks[i].has_audio, memory_order_acquire)) {
            total_audio_tracks++;
            if (mask & (1 << i)) {
                selected_audio_tracks++;
            }
        }
    }

    if (total_audio_tracks == 0) return true;
    return (selected_audio_tracks < total_audio_tracks);
}

typedef void (*NLE_Func)(void);

static void async_nle_thread(GTask *task, gpointer source, gpointer task_data, GCancellable *c) {
    (void)source; (void)c;
    NLE_Func func = (NLE_Func)task_data;
    if (func) func();
    g_task_return_boolean(task, TRUE);
}

static void async_nle_ready(GObject *source, GAsyncResult *res, gpointer data) {
    (void)source; (void)data;
    g_task_propagate_boolean(G_TASK(res), NULL);

    extern void trigger_global_ui_refresh(void);
    trigger_global_ui_refresh();

    extern GtkWidget *main_spinner;
    if (main_spinner) gtk_spinner_stop(GTK_SPINNER(main_spinner));

    extern void set_app_waiting_state(bool);
    set_app_waiting_state(false);

    GtkWindow *win = gtk_application_get_active_window(GTK_APPLICATION(g_application_get_default()));
    if (win) {
        GtkWidget *master_box = gtk_window_get_child(win);
        if (master_box) gtk_widget_set_sensitive(master_box, TRUE);
    }
}

static void dispatch_nle_operation(NLE_Func op) {
    GtkWindow *win = gtk_application_get_active_window(GTK_APPLICATION(g_application_get_default()));
    if (win) {
        GtkWidget *master_box = gtk_window_get_child(win);
        if (master_box) gtk_widget_set_sensitive(master_box, FALSE);
    }

    extern GtkWidget *main_spinner;
    if (main_spinner) gtk_spinner_start(GTK_SPINNER(main_spinner));

    extern void set_app_waiting_state(bool);
    set_app_waiting_state(true);

    GTask *task = g_task_new(NULL, NULL, async_nle_ready, NULL);
    g_task_set_task_data(task, op, NULL);
    g_task_run_in_thread(task, async_nle_thread);
    g_object_unref(task);
}

static void execute_delete_active_layer(void) {
    extern void delete_multitrack_layer(int);
    extern _Atomic int current_recording_track;
    int trk = atomic_load_explicit(&current_recording_track, memory_order_acquire);
    delete_multitrack_layer(trk);
}

gboolean on_window_key_pressed(GtkEventControllerKey *controller, guint keyval, guint keycode, GdkModifierType state, gpointer user_data) {
    (void)keycode; (void)user_data;

    extern GtkWidget *main_spinner;
    if (main_spinner && gtk_spinner_get_spinning(GTK_SPINNER(main_spinner))) {
        return TRUE;
    }

    GtkWidget *window = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller));
    GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(window));

    bool in_text_entry = false;
    if (focus && GTK_IS_EDITABLE(focus)) {
        in_text_entry = true;
        GtkWidget *parent = gtk_widget_get_parent(focus);
        if (GTK_IS_SPIN_BUTTON(focus) || (parent && GTK_IS_SPIN_BUTTON(parent))) {
            in_text_entry = false;
        }
    }
    if (in_text_entry) return FALSE;

    bool has_ctrl = (state & GDK_CONTROL_MASK) != 0;
    bool has_shift = (state & GDK_SHIFT_MASK) != 0;

    switch (keyval) {
        case GDK_KEY_space:
            if (atomic_load_explicit(&engine_is_recording, memory_order_acquire)) {
                extern _Atomic bool video_is_armed;
                extern GtkWidget *btn_record_video;
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

        case GDK_KEY_p:
        case GDK_KEY_P:
        case GDK_KEY_Page_Up:
            if (!has_ctrl && btn_prev && gtk_widget_is_sensitive(btn_prev)) {
                g_signal_emit_by_name(btn_prev, "clicked");
            }
            return TRUE;

        case GDK_KEY_n:
        case GDK_KEY_N:
        case GDK_KEY_Page_Down:
            if (!has_ctrl && btn_next && gtk_widget_is_sensitive(btn_next)) {
                g_signal_emit_by_name(btn_next, "clicked");
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

        case GDK_KEY_y:
        case GDK_KEY_Y:
            if (has_ctrl && !has_shift) {
                if (global_undo_ledger.head >= global_undo_ledger.count) return TRUE;

                static gint64 last_redo_time_y = 0;
                gint64 now = g_get_monotonic_time();
                if (now - last_redo_time_y < 250000) return TRUE;
                last_redo_time_y = now;

                extern void redo_selection(void);
                dispatch_nle_operation(redo_selection);
                return TRUE;
            }
            return FALSE;

        case GDK_KEY_s:
        case GDK_KEY_S:
            if (!has_ctrl && !has_shift && btn_stop && gtk_widget_is_sensitive(btn_stop)) {
                g_signal_emit_by_name(btn_stop, "clicked");
            }
            return TRUE;

        case GDK_KEY_a:
        case GDK_KEY_A:
            if (has_ctrl && !has_shift) {
                GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(window));
                extern GtkWidget *right_vbox;
                extern GtkWidget *command_post_box;
                extern GtkWidget *left_column_box;

                bool in_playlist = focus && (focus == right_vbox || gtk_widget_is_ancestor(focus, right_vbox));
                bool in_cmd = focus && (focus == command_post_box || gtk_widget_is_ancestor(focus, command_post_box));
                bool in_left = focus && (focus == left_column_box || gtk_widget_is_ancestor(focus, left_column_box));
                bool in_multi = in_left && !in_playlist && !in_cmd;

                if (in_playlist) {
                    extern bool playlist_handle_ctrl_a(void);
                    return playlist_handle_ctrl_a();
                } else if (in_multi) {
                    extern void multitrack_select_all_active(void);
                    multitrack_select_all_active();
                    return TRUE;
                } else {
                    extern size_t get_last_audible_frame(void);
                    size_t last_f = get_last_audible_frame();
                    size_t total = atomic_load_explicit(&backing_track_frames, memory_order_acquire);
                    if (last_f > 0 && total > 0) {
                        extern void set_loop_points(double, double);
                        set_loop_points(0.0, (double)last_f / (double)total);
                        extern void seek_backing_track(double);
                        seek_backing_track(0.0);
                        extern GtkWidget *waveform_area_bt;
                        extern GtkWidget *waveform_area_input;
                        if (waveform_area_bt) gtk_widget_queue_draw(waveform_area_bt);
                        if (waveform_area_input) gtk_widget_queue_draw(waveform_area_input);
                    }
                    return TRUE;
                }
            }
            return FALSE;

        case GDK_KEY_c:
        case GDK_KEY_C:
            if (has_ctrl && !has_shift) {
                extern void copy_selection(void);
                dispatch_nle_operation(copy_selection);
                return TRUE;
            } else if (!has_ctrl && !has_shift) {
                if (btn_speed && gtk_widget_is_sensitive(btn_speed)) {
                    g_signal_emit_by_name(btn_speed, "clicked");
                }
                return TRUE;
            }
            return FALSE;

        case GDK_KEY_x:
        case GDK_KEY_X:
        {
            extern _Atomic uint32_t selected_tracks_mask;
            uint32_t mask = atomic_load_explicit(&selected_tracks_mask, memory_order_acquire);

            if (has_ctrl && !has_shift) {
                if (!is_time_shift_safe(mask, false)) {
                    log_and_notify(1, "Status: Time shifting edits disabled to protect video sync");
                    return TRUE;
                }
                extern void cut_selection(void);
                dispatch_nle_operation(cut_selection);
                return TRUE;
            } else if (!has_ctrl && !has_shift) {
                if (!is_time_shift_safe(mask, false)) {
                    log_and_notify(1, "Status: Time shifting edits disabled to protect video sync");
                    return TRUE;
                }
                extern void apply_blend_fade_selection(void);
                dispatch_nle_operation(apply_blend_fade_selection);
                return TRUE;
            }
            return FALSE;
        }

        case GDK_KEY_v:
        case GDK_KEY_V:
            if (has_ctrl) {
                extern _Atomic uint32_t selected_tracks_mask;
                uint32_t mask = atomic_load_explicit(&selected_tracks_mask, memory_order_acquire);

                if (!is_time_shift_safe(mask, has_shift)) {
                    log_and_notify(1, "Status: Time shifting edits disabled to protect video sync");
                    return TRUE;
                }

                if (has_shift) {
                    extern void global_paste_selection(void);
                    dispatch_nle_operation(global_paste_selection);
                } else {
                    extern void paste_selection(void);
                    dispatch_nle_operation(paste_selection);
                }
                return TRUE;
            } else if (!has_ctrl && !has_shift) {
                extern GtkWidget *btn_record_video;
                if (btn_record_video && gtk_widget_is_sensitive(btn_record_video)) {
                    g_signal_emit_by_name(btn_record_video, "clicked");
                }
                return TRUE;
            }
            return FALSE;

        case GDK_KEY_z:
        case GDK_KEY_Z:
            if (has_ctrl && !has_shift) {
                if (global_undo_ledger.head <= 0 || global_undo_ledger.count <= 0) return TRUE;

                static gint64 last_undo_time = 0;
                gint64 now = g_get_monotonic_time();
                if (now - last_undo_time < 250000) return TRUE;
                last_undo_time = now;

                extern void undo_selection(void);
                dispatch_nle_operation(undo_selection);
                return TRUE;
            } else if (has_ctrl && has_shift) {
                if (global_undo_ledger.head >= global_undo_ledger.count) return TRUE;

                static gint64 last_redo_time_z = 0;
                gint64 now = g_get_monotonic_time();
                if (now - last_redo_time_z < 250000) return TRUE;
                last_redo_time_z = now;

                extern void redo_selection(void);
                dispatch_nle_operation(redo_selection);
                return TRUE;
            }
            return FALSE;

        case GDK_KEY_f:
        case GDK_KEY_F:
            if (!has_ctrl && !has_shift) {
                extern void apply_smart_fade_selection(void);
                dispatch_nle_operation(apply_smart_fade_selection);
                return TRUE;
            }
            return FALSE;

        case GDK_KEY_Delete:
        {
            if (has_ctrl && !has_shift) {
                static gint64 last_ctrl_del_time = 0;
                gint64 now = g_get_monotonic_time();
                if (now - last_ctrl_del_time < 250000) return TRUE;
                last_ctrl_del_time = now;

                dispatch_nle_operation(execute_delete_active_layer);
                return TRUE;
            } else if (!has_ctrl && has_shift) {
                if (!is_time_shift_safe(0xFFFFFFFF, true)) {
                    log_and_notify(1, "Status: Time shifting edits disabled to protect video sync");
                    return TRUE;
                }
                extern void clear_video_stash_async(void);
                clear_video_stash_async();

                extern void global_ripple_delete_selection(void);
                dispatch_nle_operation(global_ripple_delete_selection);
                return TRUE;
            } else if (!has_ctrl && !has_shift) {
                extern void delete_selection(void);
                dispatch_nle_operation(delete_selection);
                return TRUE;
            }
            return FALSE;
        }
        case GDK_KEY_Escape:
            extern bool is_countdown_active(void);
            if (atomic_load_explicit(&engine_is_recording, memory_order_acquire) || is_countdown_active()) {
                extern GtkWidget *btn_stop;
                if (btn_stop && gtk_widget_is_sensitive(btn_stop)) {
                    g_signal_emit_by_name(btn_stop, "clicked");
                }
                return TRUE;
            }

            if (!atomic_load_explicit(&engine_is_recording, memory_order_acquire)) {
                extern _Atomic bool loop_active;
                if (atomic_load_explicit(&loop_active, memory_order_acquire)) {
                    extern void clear_loop_points(void);
                    clear_loop_points();
                    gtk_widget_queue_draw(waveform_area_bt);
                    gtk_widget_queue_draw(waveform_area_input);
                } else {
                    extern GtkWidget *btn_reset;
                    if (btn_reset && gtk_widget_is_sensitive(btn_reset)) {
                        g_signal_emit_by_name(btn_reset, "clicked");
                    }
                }
            }
            return TRUE;

        case GDK_KEY_Left:
            apply_keyboard_seek(-1.0, state);
            return TRUE;

        case GDK_KEY_Right:
            apply_keyboard_seek(1.0, state);
            return TRUE;

        case GDK_KEY_Home:
            if (!atomic_load_explicit(&engine_is_recording, memory_order_acquire)) {
                if (has_shift) {
                    size_t total = atomic_load_explicit(&backing_track_frames, memory_order_acquire);
                    if (total > 0) {
                        double end_frac = 1.0;
                        if (atomic_load_explicit(&loop_active, memory_order_acquire)) {
                            end_frac = (double)atomic_load_explicit(&loop_end_frame, memory_order_acquire) / (double)total;
                        } else {
                            end_frac = (double)atomic_load_explicit(&playback_pos, memory_order_acquire) / (double)total;
                        }
                        set_loop_points(0.0, end_frac);
                        seek_backing_track(0.0);
                        gtk_widget_queue_draw(waveform_area_bt);
                        gtk_widget_queue_draw(waveform_area_input);
                    }
                } else {
                    extern void clear_loop_points(void);
                    clear_loop_points();
                    seek_backing_track(0.0);
                }
            }
            return TRUE;

        case GDK_KEY_End:
            if (!atomic_load_explicit(&engine_is_recording, memory_order_acquire)) {
                size_t total = atomic_load_explicit(&backing_track_frames, memory_order_acquire);
                if (total > 0) {
                    extern size_t get_last_audible_frame(void);
                    size_t last_f = get_last_audible_frame();
                    double target_frac = 1.0;
                    if (!has_ctrl && last_f > 0) {
                        target_frac = (double)last_f / (double)total;
                    }
                    if (has_shift) {
                        double start_frac = 0.0;
                        if (atomic_load_explicit(&loop_active, memory_order_acquire)) {
                            start_frac = (double)atomic_load_explicit(&loop_start_frame, memory_order_acquire) / (double)total;
                        } else {
                            start_frac = (double)atomic_load_explicit(&playback_pos, memory_order_acquire) / (double)total;
                        }
                        set_loop_points(start_frac, target_frac);
                        seek_backing_track(start_frac);
                        gtk_widget_queue_draw(waveform_area_bt);
                        gtk_widget_queue_draw(waveform_area_input);
                    } else {
                        extern void clear_loop_points(void);
                        clear_loop_points();
                        seek_backing_track(target_frac);
                    }
                }
            }
            return TRUE;

        case GDK_KEY_F2:
            multitrack_start_rename();
            return TRUE;

        case GDK_KEY_1: case GDK_KEY_2: case GDK_KEY_3: case GDK_KEY_4: case GDK_KEY_5:
        case GDK_KEY_6: case GDK_KEY_7: case GDK_KEY_8: case GDK_KEY_9: case GDK_KEY_0:
        {
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

static void on_scrollbar_changed(GtkAdjustment *adjustment, gpointer user_data) {
    (void)adjustment; (void)user_data;
    invalidate_waveform_caches();
    if (waveform_area_bt) gtk_widget_queue_draw(waveform_area_bt);
    if (waveform_area_input) gtk_widget_queue_draw(waveform_area_input);
}

static gboolean on_waveform_drop(GtkDropTarget *target, const GValue *value, double x, double y, gpointer user_data) {
    (void)target; (void)x; (void)y; (void)user_data;

    extern _Atomic int current_recording_track;
    int rec_track = atomic_load_explicit(&current_recording_track, memory_order_acquire);

    return handle_track_drop_payload(value, rec_track);
}

static void on_open(GtkApplication *app, GFile **files, gint n_files, const gchar *hint, gpointer user_data) {
    (void)hint; (void)user_data;
    GList *windows = gtk_application_get_windows(app);
    bool is_running = (windows != NULL);

    if (n_files > 0) {
        char *path = g_file_get_path(files[0]);
        if (path) {
            if (g_str_has_suffix(path, ".m3u")) {
                strncpy(ui_state.config.last_playlist_path, path, sizeof(ui_state.config.last_playlist_path) - 1);
                if (is_running) load_playlist_from_file(path);
            } else {
                strncpy(ui_state.selected_track_path, path, sizeof(ui_state.selected_track_path) - 1);
                if (is_running) {
                    extern atomic_int current_recording_track;
                    int rec_track = atomic_load_explicit(&current_recording_track, memory_order_acquire);
                    multitrack_import_file_async(path, rec_track);
                }
            }
            g_free(path);
        }
    }

    if (!is_running) {
        g_application_activate(G_APPLICATION(app));
    } else {
        gtk_window_present(GTK_WINDOW(windows->data));
    }
}

static void on_audio_recovery_response(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    GtkAlertDialog *alert = GTK_ALERT_DIALOG(source_object);
    int response = gtk_alert_dialog_choose_finish(alert, res, NULL);
    char *autosave_path = (char *)user_data;

    if (response == 0) {
        extern void command_post_load_session(const char *path);
        command_post_load_session(autosave_path);
    } else {
        remove(autosave_path);
    }
    g_free(autosave_path);
}

static void on_video_recovery_response(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    GtkAlertDialog *alert = GTK_ALERT_DIALOG(source_object);
    int response = gtk_alert_dialog_choose_finish(alert, res, NULL);
    char *raw_path = (char *)user_data;

    if (response == 0) {
        strncpy(current_raw_path, raw_path, sizeof(current_raw_path) - 1);
        strncpy(ui_state.selected_track_path, raw_path, sizeof(ui_state.selected_track_path) - 1);

        int rec_track = atomic_load_explicit(&current_recording_track, memory_order_acquire);
        multitrack_import_file_async(raw_path, rec_track);

        extern GtkWidget *btn_save_mux;
        if (btn_save_mux) {
            gtk_widget_remove_css_class(btn_save_mux, "needs-save");
            gtk_widget_add_css_class(btn_save_mux, "needs-save");
        }
    } else {
        remove(raw_path);
    }
    g_free(raw_path);
}

static gboolean check_for_crash_recovery(gpointer user_data) {
    GtkWindow *window = GTK_WINDOW(user_data);

    char autosave_path[1024];
    snprintf(autosave_path, sizeof(autosave_path), "%s/.qjams_autosave.qjams", ui_state.config.recordings_dir);

    if (access(autosave_path, F_OK) == 0) {
        log_and_notify(0, "[RECOVERY] Orphaned autosave detected. Prompting user for session restore.\n");
        GtkAlertDialog *alert = gtk_alert_dialog_new("Recover Unsaved Session?");
        gtk_alert_dialog_set_detail(alert, "QJams recovered an unsaved multitrack session from a previous crash. Would you like to restore it?");
        const char *buttons[] = { "Restore Session", "Discard", NULL };
        gtk_alert_dialog_set_buttons(alert, buttons);
        gtk_alert_dialog_set_cancel_button(alert, 1);
        gtk_alert_dialog_set_default_button(alert, 0);

        gtk_alert_dialog_choose(alert, window, NULL, on_audio_recovery_response, g_strdup(autosave_path));
        return G_SOURCE_REMOVE;
    }

    GDir *dir = g_dir_open("/tmp", 0, NULL);
    if (dir) {
        const char *filename;
        char latest_raw[1024] = "";
        time_t latest_time = 0;

        while ((filename = g_dir_read_name(dir)) != NULL) {
            if (g_str_has_prefix(filename, "qjams_raw_") && g_str_has_suffix(filename, ".mkv")) {
                char full_path[1024];
                snprintf(full_path, sizeof(full_path), "/tmp/%s", filename);

                struct stat st;
                if (stat(full_path, &st) == 0) {
                    if (st.st_mtime > latest_time) {
                        if (strlen(latest_raw) > 0) remove(latest_raw);
                        latest_time = st.st_mtime;
                        strncpy(latest_raw, full_path, sizeof(latest_raw) - 1);
                    } else {
                        remove(full_path);
                    }
                }
            }
        }
        g_dir_close(dir);

        if (strlen(latest_raw) > 0) {
            log_and_notify(0, "[RECOVERY] Orphaned un-exported video detected. Prompting user for recovery.\n");
            GtkAlertDialog *alert = gtk_alert_dialog_new("Recover Un-Exported Video?");
            gtk_alert_dialog_set_detail(alert, "QJams recovered an un-exported video recording. Would you like to load it for export?");

            const char *buttons[] = { "Restore Recording", "Discard", NULL };

            gtk_alert_dialog_set_buttons(alert, buttons);
            gtk_alert_dialog_set_cancel_button(alert, 1);
            gtk_alert_dialog_set_default_button(alert, 0);

            gtk_alert_dialog_choose(alert, window, NULL, on_video_recovery_response, g_strdup(latest_raw));
        }
    }

    return G_SOURCE_REMOVE;
}

static void autosave_thread(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable) {
    (void)source_object; (void)cancellable;
    char *path = (char *)task_data;

    extern int save_qjams_session(const char* filepath);
    int result = save_qjams_session(path);

    g_task_return_int(task, result);
}

static void autosave_ready(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    (void)source_object; (void)user_data;
    GError *error = NULL;
    int status = g_task_propagate_int(G_TASK(res), &error);

    extern GtkWidget *main_spinner;
    if (main_spinner) gtk_spinner_stop(GTK_SPINNER(main_spinner));

    // Unlock the UI's master container, leaving the window itself untouched
    GtkWindow *win = gtk_application_get_active_window(GTK_APPLICATION(g_application_get_default()));
    if (win) {
        GtkWidget *master_box = gtk_window_get_child(win);
        if (master_box) gtk_widget_set_sensitive(master_box, TRUE);
    }

    set_app_waiting_state(false);

    // Communicate status without stealing focus
    if (status == 0) {
        log_and_notify(1, "Status: Autosave Complete");
    } else {
        log_and_notify(1, "Status: Autosave Failed");
    }
}

static gboolean idle_autosave_tick(gpointer user_data) {
    (void)user_data;
    static int last_autosave_head = -1;

    bool is_playing = atomic_load_explicit(&engine_is_playing, memory_order_acquire);
    bool is_recording = atomic_load_explicit(&engine_is_recording, memory_order_acquire);

    if (is_recording || is_playing) {
        return G_SOURCE_CONTINUE;
    }

    if (!is_session_dirty() || pristine_frames == 0) {
        return G_SOURCE_CONTINUE;
    }

    extern UndoLedger global_undo_ledger;
    if (global_undo_ledger.head == last_autosave_head) {
        return G_SOURCE_CONTINUE;
    }
    last_autosave_head = global_undo_ledger.head;

    char autosave_path[1024];
    snprintf(autosave_path, sizeof(autosave_path), "%s/.qjams_autosave.qjams", ui_state.config.recordings_dir);

    // Lock the master container so the frame clock stays alive for the spinner
    GtkWindow *win = gtk_application_get_active_window(GTK_APPLICATION(g_application_get_default()));
    if (win) {
        GtkWidget *master_box = gtk_window_get_child(win);
        if (master_box) gtk_widget_set_sensitive(master_box, FALSE);
    }

    extern GtkWidget *main_spinner;
    if (main_spinner) gtk_spinner_start(GTK_SPINNER(main_spinner));
    set_app_waiting_state(true);
    log_and_notify(1, "Status: Autosaving in background...");

    GTask *task = g_task_new(NULL, NULL, autosave_ready, NULL);
    g_task_set_task_data(task, g_strdup(autosave_path), g_free);
    g_task_run_in_thread(task, autosave_thread);
    g_object_unref(task);

    return G_SOURCE_CONTINUE;
}

static void on_activate(GtkApplication *app, gpointer user_data) {
    (void)user_data;

    GtkCssProvider *css_provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css_provider,
                                      "scrollbar slider { min-width: 8px; min-height: 8px; }\n"
                                      ".subpixel-label { font-size: 10.5pt; }\n"
                                      ".btn-record { background: #ffb3b3; color: black; }\n"
                                      ".btn-play { background: #b3ffc6; color: black; }\n"
                                      ".btn-play:disabled { background: #265740; color: rgba(0,0,0,0.4); }\n"
                                      ".btn-reset { color: #ff4444; font-weight: 600; }\n"
                                      ".btn-reset label { margin-top: 4px; }\n"
                                      ".btn-eject label { font-weight: 200; }\n"
                                      ".symbol-btn { padding: 0px 0px; min-width: 32px; min-height: 32px; }\n"
                                      ".symbol-btn label { font-size: 14px; }\n"
                                      ".multitrack-canvas { background-color: #ffffff; border-radius: 4px; }\n"
                                      ".multitrack-row { background-color: #ffffff; border-bottom: 1px solid #eeeeee; padding: 4px; }\n"
                                      ".multitrack-row.active-row { background-color: #dbf0ff; }\n"
                                      "editablelabel.editing { background: #ffffff; border: 1px solid #3584e4; border-radius: 4px; padding: 2px 6px; }\n"
                                      "editablelabel.editing text { color: #000000; caret-color: #000000; font-weight: 600; }\n"
                                      "editablelabel.editing text selection { background: #3584e4; color: #ffffff; }\n"
                                      "frame { border: 1px solid rgba(128, 128, 128, 0.4); border-radius: 6px; }\n"
                                      ".header-title { font-weight: 800; padding: 2px 2px; }\n"
                                      ".needs-save { background: #b3ffc6; color: black; font-weight: bold; transition: background 0.3s ease; }\n"
                                      ".active-preset { background: #b3ffc6; color: black; font-weight: bold; border-color: #265740; }\n"
                                      "spinbutton.compact-spin { min-width: 40px; }\n"
                                      "spinbutton.compact-spin text { min-width: 25px; padding-left: 2px; padding-right: 2px; }\n");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(css_provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    ui_state.config = load_qjams_config(ui_state.config_path);

    // UNIFIED CANVAS LIMIT
    ui_state.default_canvas_limit_min = ui_state.config.default_canvas_limit_min;

    bool expected_is_offline = false;
    char expected_audio[128] = "";

    if (strlen(ui_state.config.last_playlist_path) > 0) {
        strncpy(ui_state.playlist_path, ui_state.config.last_playlist_path, sizeof(ui_state.playlist_path) - 1);
        ui_state.playlist_path[sizeof(ui_state.playlist_path) - 1] = '\0';
    }

    jack_status_t status;
    client = jack_client_open("QJams_Engine", JackNullOption, &status);

    log_and_notify(0, "\n=== QJAMS HARDWARE DISCOVERY TEST ===\n");

    const char *session_type = getenv("XDG_SESSION_TYPE");
    log_and_notify(0, "[SYSTEM] Display Server: %s\n", session_type ? session_type : "Unknown");
    if (client) {
        log_and_notify(0, "[JACK DSP] Daemon Version: %s\n", jack_get_version_string());
        log_and_notify(0, "[JACK DSP] Buffer Size: %d frames\n", jack_get_buffer_size(client));
    }

    VideoDevice v_devices[MAX_CAMERAS];
    int v_count = scan_video_devices(v_devices, MAX_CAMERAS);
    log_and_notify(0, "[Video] Found %d capture devices:\n", v_count);
    for (int i = 0; i < v_count; i++) log_and_notify(0, "  %d: %s (%s)\n", i+1, v_devices[i].device_name, v_devices[i].device_path);

    if (client) {
        AudioDevice a_devices[MAX_AUDIO_DEVICES];
        int a_count = scan_audio_devices(client, a_devices, MAX_AUDIO_DEVICES);
        log_and_notify(0, "\n[Audio] Found %d physical capture devices:\n", a_count);
        for (int i = 0; i < a_count; i++) log_and_notify(0, "  %d: %s [%d channels]\n", i+1, a_devices[i].display_name, a_devices[i].channel_count);

        char usb_hardware[32][128] = {0};
        int usb_count = get_physical_usb_hardware(usb_hardware, 32);
        bool config_healed = false;

        for (int p = 0; p < ui_state.config.num_audio_profiles; p++) {
            char *stale_name = ui_state.config.audio_profiles[p].device_name;
            bool exact_match = false;

            for (int i = 0; i < a_count; i++) {
                if (strcmp(stale_name, a_devices[i].display_name) == 0) {
                    exact_match = true;
                    break;
                }
            }
            if (exact_match) continue;

            for (int k = 0; k < usb_count; k++) {
                if ((strstr(stale_name, "Cortex") && strstr(usb_hardware[k], "Cortex")) ||
                    (strstr(stale_name, "SA9226") && strstr(usb_hardware[k], "SA9226")) ||
                    (strstr(stale_name, "Logitech") && strstr(usb_hardware[k], "Logitech"))) {

                    for (int i = 0; i < a_count; i++) {
                        if ((strstr(a_devices[i].display_name, "Cortex") && strstr(usb_hardware[k], "Cortex")) ||
                            (strstr(a_devices[i].display_name, "SA9226") && strstr(usb_hardware[k], "SA9226")) ||
                            (strstr(a_devices[i].display_name, "Logitech") && strstr(usb_hardware[k], "Logitech"))) {

                            log_and_notify(0, "[HW-HEAL] USB topology changed. Re-mapping stale profile '%s' to active port '%s'\n", stale_name, a_devices[i].display_name);

                        if (strcmp(ui_state.config.audio_device, stale_name) == 0) {
                            strncpy(ui_state.config.audio_device, a_devices[i].display_name, 127);
                            ui_state.config.audio_device[127] = '\0';
                        }

                        strncpy(ui_state.config.audio_profiles[p].device_name, a_devices[i].display_name, 127);
                        ui_state.config.audio_profiles[p].device_name[127] = '\0';
                        config_healed = true;
                        break;
                            }
                    }
                    }
            }
        }

        if (config_healed) {
            save_qjams_config(ui_state.config_path, &ui_state.config);
        }

        char target_audio[128] = "";

        for (int p = 0; p < ui_state.config.num_audio_profiles; p++) {
            if (ui_state.config.audio_profiles[p].is_primary) {
                strncpy(expected_audio, ui_state.config.audio_profiles[p].device_name, 127);
                expected_audio[127] = '\0';
                break;
            }
        }

        if (strlen(expected_audio) == 0) {
            strncpy(expected_audio, ui_state.config.audio_device, 127);
            expected_audio[127] = '\0';
        }

        if (strlen(expected_audio) > 0) {
            for (int i = 0; i < a_count; i++) {
                if (strcmp(expected_audio, a_devices[i].display_name) == 0) {
                    strncpy(target_audio, expected_audio, 127);
                    target_audio[127] = '\0';
                    break;
                }
            }
        }

        if (strlen(target_audio) == 0 && a_count > 0 && strlen(expected_audio) > 0) {
            expected_is_offline = true;
            for (int p = 0; p < ui_state.config.num_audio_profiles; p++) {
                if (ui_state.config.audio_profiles[p].is_fallback) {
                    for (int i = 0; i < a_count; i++) {
                        if (strcmp(ui_state.config.audio_profiles[p].device_name, a_devices[i].display_name) == 0) {
                            strncpy(target_audio, a_devices[i].display_name, 127);
                            target_audio[127] = '\0';
                            break;
                        }
                    }
                    break;
                }
            }

            if (strlen(target_audio) == 0) {
                strncpy(target_audio, a_devices[0].display_name, 127);
                target_audio[127] = '\0';
                log_and_notify(0, "-> Pre-Boot Routing: Expected devices missing. Ultimate fallback to %s\n", target_audio);
            }
        }

        if (strlen(target_audio) > 0) {
            strncpy(ui_state.config.audio_device, target_audio, 127);
            ui_state.config.audio_device[127] = '\0';
            for (int p = 0; p < ui_state.config.num_audio_profiles; p++) {
                if (strcmp(ui_state.config.audio_profiles[p].device_name, target_audio) == 0) {
                    ui_state.config.input_gain_multiplier = ui_state.config.audio_profiles[p].gain_multiplier;
                    break;
                }
            }
            log_and_notify(0, "-> Pre-Boot Routing: Target explicitly forced to %s\n", target_audio);
        }
    }
    log_and_notify(0, "======================================\n\n");

    init_preview_queue(&preview_queue, 32);

    GtkBuilder *b_main  = gtk_builder_new_from_resource("/com/rob/qjams/ui/main_window.ui");
    GtkBuilder *b_multi = gtk_builder_new_from_resource("/com/rob/qjams/ui/multitrack.ui");
    GtkBuilder *b_play  = gtk_builder_new_from_resource("/com/rob/qjams/ui/playlist.ui");
    GtkBuilder *b_cmd   = gtk_builder_new_from_resource("/com/rob/qjams/ui/command_post.ui");
    GtkBuilder *b_dash  = gtk_builder_new_from_resource("/com/rob/qjams/ui/dashboard.ui");
    GtkBuilder *b_wave  = gtk_builder_new_from_resource("/com/rob/qjams/ui/waveforms.ui");

    GtkWindow *window = GTK_WINDOW(gtk_builder_get_object(b_main, "main_window"));
    gtk_window_set_application(window, app);
    gtk_window_set_default_size(window, ui_state.config.window_width, ui_state.config.window_height);
    g_signal_connect(window, "close-request", G_CALLBACK(on_window_close), NULL);

    GtkEventController *key_ctrl = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(key_ctrl, GTK_PHASE_CAPTURE);
    g_signal_connect(key_ctrl, "key-pressed", G_CALLBACK(on_window_key_pressed), NULL);
    gtk_widget_add_controller(GTK_WIDGET(window), key_ctrl);

    GtkBox *host_multi = GTK_BOX(gtk_builder_get_object(b_main, "multitrack_host"));
    GtkBox *host_play  = GTK_BOX(gtk_builder_get_object(b_main, "playlist_host"));
    GtkBox *host_cmd   = GTK_BOX(gtk_builder_get_object(b_main, "command_post_host"));
    GtkBox *host_dash  = GTK_BOX(gtk_builder_get_object(b_main, "dashboard_host"));
    GtkBox *host_wave  = GTK_BOX(gtk_builder_get_object(b_main, "waveforms_host"));

    GtkWidget *multi_page     = GTK_WIDGET(gtk_builder_get_object(b_multi, "multitrack_page_root"));
    GtkWidget *play_root      = GTK_WIDGET(gtk_builder_get_object(b_play, "playlist_root"));
    GtkWidget *cmd_root       = GTK_WIDGET(gtk_builder_get_object(b_cmd, "command_post_root"));
    GtkWidget *dash_root      = GTK_WIDGET(gtk_builder_get_object(b_dash, "dashboard_root"));
    GtkWidget *wave_root      = GTK_WIDGET(gtk_builder_get_object(b_wave, "waveforms_root"));

    gtk_box_append(host_multi, multi_page);
    gtk_box_append(host_play, play_root);
    gtk_box_append(host_cmd, cmd_root);
    gtk_box_append(host_dash, dash_root);
    gtk_box_append(host_wave, wave_root);

    left_column_box = GTK_WIDGET(gtk_builder_get_object(b_main, "left_column_box"));
    command_post_box = cmd_root;
    right_vbox = play_root;

    video_preview_frame = GTK_WIDGET(gtk_builder_get_object(b_dash, "video_preview_frame"));
    video_preview = GTK_WIDGET(gtk_builder_get_object(b_dash, "video_preview"));
    gtk_widget_add_tick_callback(video_preview, update_video_preview, NULL, NULL);

    ui_dashboard_init(b_dash, b_wave);
    ui_command_post_init(b_cmd, window);
    ui_multitrack_init(b_multi);

    GtkWidget *playlist_widget = create_playlist_widget();
    gtk_box_append(GTK_BOX(play_root), playlist_widget);

    GtkWidget *multitrack_scroll = GTK_WIDGET(gtk_builder_get_object(b_multi, "multitrack_tracks_scroll"));
    GtkWidget *multitrack_list = create_multitrack_tracks_widget();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(multitrack_scroll), multitrack_list);

    waveform_area_bt = GTK_WIDGET(gtk_builder_get_object(b_wave, "waveform_area_bt"));
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(waveform_area_bt), on_draw_bt_waveform, NULL, NULL);
    gtk_widget_add_tick_callback(waveform_area_bt, (GtkTickCallback)on_waveform_tick, NULL, NULL);

    GtkGesture *drag_gesture_bt = gtk_gesture_drag_new();
    g_signal_connect(drag_gesture_bt, "drag-begin", G_CALLBACK(on_drag_begin), NULL);
    g_signal_connect(drag_gesture_bt, "drag-update", G_CALLBACK(on_drag_update), NULL);
    g_signal_connect(drag_gesture_bt, "drag-end", G_CALLBACK(on_drag_end), NULL);
    gtk_widget_add_controller(waveform_area_bt, GTK_EVENT_CONTROLLER(drag_gesture_bt));

    GtkGesture *click_gesture_bt = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click_gesture_bt), GDK_BUTTON_PRIMARY);
    g_signal_connect(click_gesture_bt, "pressed", G_CALLBACK(on_waveform_click_pressed), NULL);
    gtk_widget_add_controller(waveform_area_bt, GTK_EVENT_CONTROLLER(click_gesture_bt));

    GtkGesture *right_click_bt = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(right_click_bt), GDK_BUTTON_SECONDARY);
    g_signal_connect(right_click_bt, "pressed", G_CALLBACK(on_waveform_right_click), NULL);
    gtk_widget_add_controller(waveform_area_bt, GTK_EVENT_CONTROLLER(right_click_bt));

    gtk_widget_set_has_tooltip(waveform_area_bt, TRUE);
    g_signal_connect(waveform_area_bt, "query-tooltip", G_CALLBACK(on_waveform_query_tooltip), NULL);

    GType drop_types[] = { GDK_TYPE_FILE_LIST, QJ_TYPE_TRACK };
    GtkDropTarget *drop_target = gtk_drop_target_new(G_TYPE_INVALID, GDK_ACTION_COPY | GDK_ACTION_MOVE);
    gtk_drop_target_set_gtypes(drop_target, drop_types, 2);
    g_signal_connect(drop_target, "drop", G_CALLBACK(on_waveform_drop), NULL);
    gtk_widget_add_controller(waveform_area_bt, GTK_EVENT_CONTROLLER(drop_target));

    waveform_area_input = GTK_WIDGET(gtk_builder_get_object(b_wave, "waveform_area_input"));
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(waveform_area_input), on_draw_input_waveform, NULL, NULL);
    gtk_widget_add_tick_callback(waveform_area_input, on_waveform_tick, NULL, NULL);

    GtkGesture *drag_gesture_in = gtk_gesture_drag_new();
    g_signal_connect(drag_gesture_in, "drag-begin", G_CALLBACK(on_drag_begin), NULL);
    g_signal_connect(drag_gesture_in, "drag-update", G_CALLBACK(on_drag_update), NULL);
    g_signal_connect(drag_gesture_in, "drag-end", G_CALLBACK(on_drag_end), NULL);
    gtk_widget_add_controller(waveform_area_input, GTK_EVENT_CONTROLLER(drag_gesture_in));

    GtkGesture *click_gesture_in = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click_gesture_in), GDK_BUTTON_PRIMARY);
    g_signal_connect(click_gesture_in, "pressed", G_CALLBACK(on_waveform_click_pressed), NULL);
    gtk_widget_add_controller(waveform_area_input, GTK_EVENT_CONTROLLER(click_gesture_in));

    GtkGesture *right_click_in = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(right_click_in), GDK_BUTTON_SECONDARY);
    g_signal_connect(right_click_in, "pressed", G_CALLBACK(on_waveform_right_click), NULL);
    gtk_widget_add_controller(waveform_area_input, GTK_EVENT_CONTROLLER(right_click_in));

    gtk_widget_set_has_tooltip(waveform_area_input, TRUE);
    g_signal_connect(waveform_area_input, "query-tooltip", G_CALLBACK(on_waveform_query_tooltip), NULL);

    waveform_scrollbar = GTK_WIDGET(gtk_builder_get_object(b_wave, "waveform_scrollbar"));
    waveform_adj = gtk_adjustment_new(0.0, 0.0, 1.0, 0.01, 0.1, 1.0);
    gtk_scrollbar_set_adjustment(GTK_SCROLLBAR(waveform_scrollbar), waveform_adj);
    g_signal_connect(waveform_adj, "value-changed", G_CALLBACK(on_scrollbar_changed), NULL);

    g_object_unref(b_main);
    g_object_unref(b_multi);
    g_object_unref(b_play);
    g_object_unref(b_cmd);
    g_object_unref(b_dash);
    g_object_unref(b_wave);

    gtk_window_present(GTK_WINDOW(window));

    int audio_status = 0;
    if (client) {
        audio_status = init_audio_engine(16384, ui_state.config.input_gain_multiplier, client, ui_state.config.audio_device, ui_state.config.playback_target);
        set_bt_gain(ui_state.config.bt_gain_multiplier);
    }

    extern int get_jack_event_fd(void);
    extern gboolean on_jack_pipe_event(gint fd, GIOCondition condition, gpointer user_data);
    g_unix_fd_add(get_jack_event_fd(), G_IO_IN, on_jack_pipe_event, NULL);

    init_video_queue(&video_queue, 64);
    int video_status = init_and_start_video_engine(ui_state.config.video_device, &video_queue);

    if (v_count == 0) {
        if (video_preview_frame) gtk_widget_set_visible(video_preview_frame, FALSE);
        extern GtkWidget *btn_record_video;
        if (btn_record_video) gtk_widget_set_sensitive(btn_record_video, FALSE);
        extern _Atomic bool video_is_armed;
        atomic_store_explicit(&video_is_armed, false, memory_order_release);
    }

    adjust_blank_canvas();

    if (!client) {
        log_and_notify(1, "<span foreground='#ff4444'><b>FATAL ERROR: JACK Audio Server is not running.</b></span>");
        gtk_widget_set_sensitive(btn_play, FALSE);
        gtk_widget_set_sensitive(btn_record, FALSE);
    } else if (audio_status == 2 || video_status != 0 || expected_is_offline) {
        if (audio_status == 2 || video_status != 0) {
            log_and_notify(1, "Status: Running with missing hardware");
        } else {
            log_and_notify(1, "Status: Ready");
        }
    } else {
        log_and_notify(1, "Status: Ready");
    }

    if (strlen(ui_state.selected_track_path) > 0) {
        if (g_str_has_suffix(ui_state.selected_track_path, ".qjams")) {
            extern void command_post_load_session(const char *path);
            command_post_load_session(ui_state.selected_track_path);
        } else {
            int rec_track = atomic_load_explicit(&current_recording_track, memory_order_acquire);
            multitrack_import_file_async(ui_state.selected_track_path, rec_track);
        }
    }

    g_idle_add(check_for_crash_recovery, window);
    g_timeout_add(300000, idle_autosave_tick, NULL); //300000 = 5 minutes autosave interval
}

static gboolean on_posix_signal(gpointer user_data) {
    GtkApplication *app = GTK_APPLICATION(user_data);
    log_and_notify(0, "\n[SYSTEM] Intercepted POSIX kill signal. Executing clean teardown...\n");

    extern bool is_session_dirty(void);
    if (!is_session_dirty()) {
        purge_autosave_file();
    }

    g_application_quit(G_APPLICATION(app));
    return G_SOURCE_REMOVE;
}

int main(int argc, char **argv) {
    const char *config_dir = g_get_user_config_dir();
    char qjams_dir[1024];
    snprintf(qjams_dir, sizeof(qjams_dir), "%s/qjams", config_dir);
    g_mkdir_with_parents(qjams_dir, 0755);

    snprintf(ui_state.config_path, sizeof(ui_state.config_path), "%s/qjams.config", qjams_dir);
    snprintf(ui_state.playlist_path, sizeof(ui_state.playlist_path), "%s/qjams.m3u", qjams_dir);

    // Bootstrap config early to read logging preferences before GTK/JACK boot
    QJamsConfig boot_config = load_qjams_config(ui_state.config_path);

    // Boot-Time Janitor: Sweep for orphaned .stash files using standardized GDir
    if (strlen(boot_config.recordings_dir) > 0) {
        GDir *dir = g_dir_open(boot_config.recordings_dir, 0, NULL);
        if (dir) {
            const char *filename;
            while ((filename = g_dir_read_name(dir)) != NULL) {
                if (g_str_has_suffix(filename, ".stash")) {
                    char stash_path[1024];
                    snprintf(stash_path, sizeof(stash_path), "%s/%s", boot_config.recordings_dir, filename);
                    remove(stash_path);
                }
            }
            g_dir_close(dir);
        }
    }

    remove("/tmp/qjams_session_video.mkv");

    init_qjams_logger(qjams_dir, boot_config.enable_logging);

    const char *music_dir = g_get_user_special_dir(G_USER_DIRECTORY_MUSIC);
    if (!music_dir) music_dir = g_get_home_dir();
    char default_rec_dir[1024];
    snprintf(default_rec_dir, sizeof(default_rec_dir), "%s/QJams", music_dir);
    g_mkdir_with_parents(default_rec_dir, 0755);

    snprintf(ui_state.config_path, sizeof(ui_state.config_path), "%s/qjams.config", qjams_dir);
    snprintf(ui_state.playlist_path, sizeof(ui_state.playlist_path), "%s/qjams.m3u", qjams_dir);

    GtkApplication *app = gtk_application_new("com.rob.qjams", G_APPLICATION_HANDLES_OPEN);

    g_unix_signal_add(SIGINT, on_posix_signal, app);
    g_unix_signal_add(SIGTERM, on_posix_signal, app);

    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    g_signal_connect(app, "open", G_CALLBACK(on_open), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);

    stop_video_engine();
    shutdown_audio_engine();

    if (preview_queue.buffer) {
        free(preview_queue.buffer);
        preview_queue.buffer = NULL;
    }

    ui_dashboard_shutdown();

    if (strlen(current_raw_path) > 0) {
        remove(current_raw_path);
    }

    char autosave_path[1024];
    snprintf(autosave_path, sizeof(autosave_path), "%s/.qjams_autosave.qjams", ui_state.config.recordings_dir);
    remove(autosave_path);

    close_qjams_logger();
    return status;
}
