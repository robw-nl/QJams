#include <gtk/gtk.h>
#include <jack/jack.h>
#include <math.h>
#include <stdbool.h>
#include <signal.h>
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

// --- INSTANTIATE UI GLOBALS ---
QJamsUIState ui_state = { .is_loading_track = false, .selected_track_path = "", .freestyle_duration_min = 15, .multitrack_duration_min = 5, .session_is_dirty = false };

GtkWidget *lbl_input_device;
GtkWidget *input_gain_spinner;
GtkWidget *lbl_track;
GtkWidget *lbl_status;
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

GtkWidget *mode_stack;
GtkWidget *btn_multitrack_mode;
GtkWidget *lbl_multitrack_status;
GtkWidget *btn_multitrack_undo;
GtkWidget *btn_multitrack_next;
GtkWidget *sw_blank_canvas;

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

gboolean on_jack_shutdown_ui(gpointer data) {
    (void)data;
    on_stop_clicked(NULL, NULL);

    gtk_label_set_markup(GTK_LABEL(lbl_status), "<span foreground='#ff4444'><b>Status: Audio Engine Offline (Hardware Disconnected)</b></span>");
    gtk_widget_set_sensitive(btn_play, FALSE);
    gtk_widget_set_sensitive(btn_record, FALSE);

    GtkAlertDialog *alert = gtk_alert_dialog_new("Hardware Disconnected");
    gtk_alert_dialog_set_detail(alert, "The JACK audio server has forcefully disconnected. If a recording was active, the video file has been safely preserved on disk.\n\nPlease reconnect your hardware and restart QJams.");
    gtk_alert_dialog_show(alert, NULL);
    g_object_unref(alert);

    return G_SOURCE_REMOVE;
}

static void on_force_quit_response(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    GtkAlertDialog *alert = GTK_ALERT_DIALOG(source_object);
    int response = gtk_alert_dialog_choose_finish(alert, res, NULL);

    if (response == 0) { // Force Quit Confirmed
        if (active_muxer_pid != 0) {
            kill(active_muxer_pid, SIGKILL);
            active_muxer_pid = 0;
        }
        // Muxer is dead, safe to close window now
        GtkWidget *win = GTK_WIDGET(user_data);
        gtk_window_destroy(GTK_WINDOW(win));
    }
}

static gboolean on_window_close(GtkWindow *window, gpointer user_data) {
    (void)user_data;
    if (active_muxer_pid != 0) {
        GtkAlertDialog *alert = gtk_alert_dialog_new("Muxing in Progress");
        gtk_alert_dialog_set_detail(alert, "FFmpeg is currently processing the export in the background.\n\nDo you want to wait for it to finish, or Force Quit and abandon the export?");
        const char *buttons[] = { "Force Quit", "Wait", NULL };
        gtk_alert_dialog_set_buttons(alert, buttons);
        gtk_alert_dialog_set_cancel_button(alert, 1);
        gtk_alert_dialog_set_default_button(alert, 1);

        gtk_alert_dialog_choose(alert, GTK_WINDOW(window), NULL, on_force_quit_response, window);
        g_object_unref(alert);
        return TRUE; // Block immediate close
    }
    ui_state.config.window_width = gtk_widget_get_width(GTK_WIDGET(window));
    ui_state.config.window_height = gtk_widget_get_height(GTK_WIDGET(window));
    save_qjams_config(ui_state.config_path, &ui_state.config);
    return FALSE;
}

static void apply_keyboard_seek(double step_sign) {
    if (atomic_load_explicit(&engine_is_recording, memory_order_acquire)) return;

    size_t frames = atomic_load_explicit(&backing_track_frames, memory_order_acquire);
    if (frames > 0) {
        int current_rate = atomic_load_explicit(&active_sample_rate, memory_order_acquire);
        double step = (5.0 * current_rate) / (double)frames;
        double current;

        // STRICT: Eliminate race conditions during rapid key repeats.
        // If a seek is pending in the RT queue, build the next step on top of it.
        if (atomic_load_explicit(&seek_flag, memory_order_acquire)) {
            current = atomic_load_explicit(&seek_target, memory_order_relaxed);
        } else {
            current = (double)atomic_load_explicit(&playback_pos, memory_order_acquire) / (double)frames;
        }

        double target = current + (step * step_sign);

        if (step_sign < 0.0) {
            target = fmax(0.0, target);
            if (atomic_load_explicit(&loop_active, memory_order_acquire)) {
                double l_start = (double)atomic_load_explicit(&loop_start_frame, memory_order_relaxed) / (double)frames;
                target = fmax(l_start, target);
            }
        } else {
            target = fmin(1.0, target);
            if (atomic_load_explicit(&loop_active, memory_order_acquire)) {
                double l_end = (double)atomic_load_explicit(&loop_end_frame, memory_order_relaxed) / (double)frames;
                target = fmin(l_end, target);
            }
        }
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

gboolean on_window_key_pressed(GtkEventControllerKey *controller, guint keyval, guint keycode, GdkModifierType state, gpointer user_data) {
    (void)keycode; (void)user_data; (void)state;
    GtkWidget *window = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller));
    GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(window));

    // SMART FOCUS: Ignore global shortcuts if a true text entry (like Track Rename) is active.
    // SpinButtons (like Clip Gain) are strictly overridden so Space/Home always work.
    bool in_text_entry = false;
    if (focus && GTK_IS_EDITABLE(focus)) {
        in_text_entry = true;
        GtkWidget *parent = gtk_widget_get_parent(focus);
        if (GTK_IS_SPIN_BUTTON(focus) || (parent && GTK_IS_SPIN_BUTTON(parent))) {
            in_text_entry = false;
        }
    }
    if (in_text_entry) return FALSE;

    switch (keyval) {
        case GDK_KEY_p:
        case GDK_KEY_P:
        case GDK_KEY_space:
            if (btn_play && gtk_widget_is_sensitive(btn_play)) {
                g_signal_emit_by_name(btn_play, "clicked");
            }
            return TRUE;

        case GDK_KEY_r:
        case GDK_KEY_R:
            if (btn_record && gtk_widget_is_sensitive(btn_record)) {
                g_signal_emit_by_name(btn_record, "clicked");
            }
            return TRUE;

        case GDK_KEY_s:
        case GDK_KEY_S:
            if (btn_stop && gtk_widget_is_sensitive(btn_stop)) {
                g_signal_emit_by_name(btn_stop, "clicked");
            }
            return TRUE;

        case GDK_KEY_c:
        case GDK_KEY_C:
            if (btn_speed && gtk_widget_is_sensitive(btn_speed)) {
                g_signal_emit_by_name(btn_speed, "clicked");
            }
            return TRUE;

        case GDK_KEY_Escape:
            if (!atomic_load_explicit(&engine_is_recording, memory_order_acquire)) {
                extern void clear_loop_points(void);
                clear_loop_points();
                gtk_widget_queue_draw(waveform_area_bt);
                gtk_widget_queue_draw(waveform_area_input);
            }
            return TRUE;

        case GDK_KEY_Left:
            apply_keyboard_seek(-1.0);
            return TRUE;

        case GDK_KEY_Right:
            apply_keyboard_seek(1.0);
            return TRUE;

        case GDK_KEY_Home:
            if (!atomic_load_explicit(&engine_is_recording, memory_order_acquire)) {
                extern void clear_loop_points(void);
                clear_loop_points();
                seek_backing_track(0.0);
            }
            return TRUE;

        case GDK_KEY_End:
            if (!atomic_load_explicit(&engine_is_recording, memory_order_acquire)) seek_backing_track(1.0);
            return TRUE;

        case GDK_KEY_F2:
            multitrack_start_rename();
            return TRUE;
            // Playlist Preset Triggers
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

// PHASE 3 WAVEFORM DROP CALLBACK ---
static gboolean on_waveform_drop(GtkDropTarget *target, const GValue *value, double x, double y, gpointer user_data) {
    (void)target; (void)x; (void)y; (void)user_data;

    // 1. Handle external drops from OS file manager (Dolphin/Plasma)
    if (G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST)) {
        GdkFileList *file_list = g_value_get_boxed(value);
        if (file_list) {
            GSList *files = gdk_file_list_get_files(file_list);
            if (files) {
                if (atomic_load_explicit(&is_multitrack_mode, memory_order_acquire)) {
                    int rec_track = atomic_load_explicit(&current_recording_track, memory_order_acquire);
                    // Iterate through all dropped files and assign them to sequential layers
                    for (GSList *l = files; l != NULL; l = l->next) {
                        if (rec_track >= MAX_TRACKS) break; // Prevent out-of-bounds layer assignment
                        GFile *file = G_FILE(l->data);
                        char *path = g_file_get_path(file);
                        if (path) {
                            multitrack_import_file_async(path, rec_track);
                            g_free(path);
                            rec_track++; // Auto-increment to the next track for the next stem
                        }
                    }
                } else {
                    // Video Mode: Safely preserve legacy single-file load behavior
                    if (files->data) {
                        GFile *file = G_FILE(files->data);
                        char *path = g_file_get_path(file);
                        if (path) {
                            strncpy(ui_state.selected_track_path, path, sizeof(ui_state.selected_track_path) - 1);
                            ui_state.selected_track_path[sizeof(ui_state.selected_track_path) - 1] = '\0';
                            trigger_track_load();
                            g_free(path);
                        }
                    }
                }
                g_slist_free(files); // Safely release the GTK file list container
                return TRUE;
            }
        }
    }
    // 2. Handle internal drops directly from the QJams playlist
    else if (G_VALUE_HOLDS(value, QJ_TYPE_TRACK)) {
        QjTrack *track = g_value_get_object(value);
        if (track && track->filepath) {
            if (atomic_load_explicit(&is_multitrack_mode, memory_order_acquire)) {
                int rec_track = atomic_load_explicit(&current_recording_track, memory_order_acquire);
                multitrack_import_file_async(track->filepath, rec_track);
            } else {
                strncpy(ui_state.selected_track_path, track->filepath, sizeof(ui_state.selected_track_path) - 1);
                trigger_track_load();
            }
            return TRUE;
        }
    }
    return FALSE;
}

static void on_open(GtkApplication *app, GFile **files, gint n_files, const gchar *hint, gpointer user_data) {
    (void)hint; (void)user_data;
    if (n_files > 0) {
        char *path = g_file_get_path(files[0]);
        if (path) {
            if (g_str_has_suffix(path, ".m3u")) {
                strncpy(ui_state.config.last_playlist_path, path, sizeof(ui_state.config.last_playlist_path) - 1);
            } else {
                strncpy(ui_state.selected_track_path, path, sizeof(ui_state.selected_track_path) - 1);
            }
            g_free(path);
        }
    }
    g_application_activate(G_APPLICATION(app));
}

static void on_activate(GtkApplication *app, gpointer user_data) {
    (void)user_data;

    GtkCssProvider *css_provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css_provider,
                                      "scrollbar slider { min-width: 8px; min-height: 8px; }\n"
                                      ".btn-record { background: #ffb3b3; color: black; }\n"
                                      ".btn-record:disabled { background: #5e272a; color: rgba(0,0,0,0.4); }\n"
                                      ".btn-play { background: #b3ffc6; color: black; }\n"
                                      ".btn-play:disabled { background: #265740; color: rgba(0,0,0,0.4); }\n"
                                      ".btn-reset { color: #ff4444; font-weight: bold; }\n"
                                      ".multitrack-canvas { background-color: #ffffff; border-radius: 4px; }\n"
                                      ".multitrack-row { background-color: #ffffff; border-bottom: 1px solid #eeeeee; padding: 4px; }\n"
                                      ".multitrack-row.selected { background-color: #dbf0ff; }\n"
                                      "editablelabel.editing { background: #ffffff; border: 1px solid #3584e4; border-radius: 4px; padding: 2px 6px; }\n"
                                      "editablelabel.editing text { color: #000000; caret-color: #000000; font-weight: 900; }\n"
                                      "editablelabel.editing text selection { background: #3584e4; color: #ffffff; }\n"
                                      "frame { border: 1px solid rgba(128, 128, 128, 0.4); border-radius: 6px; }\n"
                                      ".needs-save { background: #b3ffc6; color: black; font-weight: bold; transition: background 0.3s ease; }\n"
                                      ".active-preset { background: #b3ffc6; color: black; font-weight: bold; border-color: #265740; }\n"
                                      "spinbutton.compact-spin { min-width: 40px; }\n"
                                      "spinbutton.compact-spin text { min-width: 25px; padding-left: 2px; padding-right: 2px; }\n");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(css_provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    ui_state.config = load_qjams_config(ui_state.config_path);

    bool expected_is_offline = false;
    char expected_audio[128] = "";

    if (strlen(ui_state.config.last_playlist_path) > 0) {
        ui_state.playlist_path[sizeof(ui_state.playlist_path) - 1] = '\0';
    }

    jack_status_t status;
    client = jack_client_open("QJams_Engine", JackNullOption, &status);

    printf("\n=== QJAMS HARDWARE DISCOVERY TEST ===\n");
    VideoDevice v_devices[MAX_CAMERAS];
    int v_count = scan_video_devices(v_devices, MAX_CAMERAS);
    printf("[Video] Found %d capture devices:\n", v_count);
    for (int i = 0; i < v_count; i++) printf("  %d: %s (%s)\n", i+1, v_devices[i].device_name, v_devices[i].device_path);

    if (client) {
        AudioDevice a_devices[MAX_AUDIO_DEVICES];
        int a_count = scan_audio_devices(client, a_devices, MAX_AUDIO_DEVICES);
        printf("\n[Audio] Found %d physical capture devices:\n", a_count);
        for (int i = 0; i < a_count; i++) printf("  %d: %s [%d channels]\n", i+1, a_devices[i].display_name, a_devices[i].channel_count);

        char target_audio[128] = "";

        // 1. Enforce Settings Order: Find the designated Primary device
        for (int p = 0; p < ui_state.config.num_audio_profiles; p++) {
            if (ui_state.config.audio_profiles[p].is_primary) {
                strncpy(expected_audio, ui_state.config.audio_profiles[p].device_name, 127);
                break;
            }
        }

        // If no Primary is configured, the "expected" device is whatever the cycler was last left on
        if (strlen(expected_audio) == 0) {
            strncpy(expected_audio, ui_state.config.audio_device, 127);
        }

        // 2. Check if the expected device is online
        if (strlen(expected_audio) > 0) {
            for (int i = 0; i < a_count; i++) {
                if (strcmp(expected_audio, a_devices[i].display_name) == 0) {
                    strncpy(target_audio, expected_audio, 127);
                    break;
                }
            }
        }

        // 3. If offline, hunt for the designated Fallback
        if (strlen(target_audio) == 0 && a_count > 0 && strlen(expected_audio) > 0) {
            expected_is_offline = true;
            for (int p = 0; p < ui_state.config.num_audio_profiles; p++) {
                if (ui_state.config.audio_profiles[p].is_fallback) {
                    for (int i = 0; i < a_count; i++) {
                        if (strcmp(ui_state.config.audio_profiles[p].device_name, a_devices[i].display_name) == 0) {
                            strncpy(target_audio, a_devices[i].display_name, 127);
                            break;
                        }
                    }
                    break;
                }
            }
        }

        // 4. Overwrite the config memory BEFORE the engine initializes to guarantee successful connection
        if (strlen(target_audio) > 0) {
            strncpy(ui_state.config.audio_device, target_audio, 127);
            ui_state.config.audio_device[127] = '\0';
            for (int p = 0; p < ui_state.config.num_audio_profiles; p++) {
                if (strcmp(ui_state.config.audio_profiles[p].device_name, target_audio) == 0) {
                    ui_state.config.input_gain_multiplier = ui_state.config.audio_profiles[p].gain_multiplier;
                    break;
                }
            }
            printf("-> Pre-Boot Routing: Target explicitly forced to %s\n", target_audio);
        }
    }
    printf("======================================\n\n");

    init_preview_queue(&preview_queue, 32);

    // 1. Load the 7 isolated UI Builders natively from the compiled GResource
    GtkBuilder *b_main  = gtk_builder_new_from_resource("/com/rob/qjams/ui/main_window.ui");
    GtkBuilder *b_stack = gtk_builder_new_from_resource("/com/rob/qjams/ui/mode_stack.ui");
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

    // 2. Extract host containers from the main skeleton
    GtkBox *host_stack = GTK_BOX(gtk_builder_get_object(b_main, "mode_stack_host"));
    GtkBox *host_play  = GTK_BOX(gtk_builder_get_object(b_main, "playlist_host"));
    GtkBox *host_cmd   = GTK_BOX(gtk_builder_get_object(b_main, "command_post_host"));
    GtkBox *host_dash  = GTK_BOX(gtk_builder_get_object(b_main, "dashboard_host"));
    GtkBox *host_wave  = GTK_BOX(gtk_builder_get_object(b_main, "waveforms_host"));

    // 3. Extract module roots
    GtkStack  *mode_stack_obj = GTK_STACK(gtk_builder_get_object(b_stack, "mode_stack"));
    GtkWidget *multi_page     = GTK_WIDGET(gtk_builder_get_object(b_multi, "multitrack_page_root"));
    GtkWidget *play_root      = GTK_WIDGET(gtk_builder_get_object(b_play, "playlist_root"));
    GtkWidget *cmd_root       = GTK_WIDGET(gtk_builder_get_object(b_cmd, "command_post_root"));
    GtkWidget *dash_root      = GTK_WIDGET(gtk_builder_get_object(b_dash, "dashboard_root"));
    GtkWidget *wave_root      = GTK_WIDGET(gtk_builder_get_object(b_wave, "waveforms_root"));

    // 4. Assemble the UI mechanically
    gtk_box_append(host_stack, GTK_WIDGET(mode_stack_obj));
    gtk_stack_add_named(mode_stack_obj, multi_page, "multitrack_page");

    gtk_box_append(host_play, play_root);
    gtk_box_append(host_cmd, cmd_root);
    gtk_box_append(host_dash, dash_root);
    gtk_box_append(host_wave, wave_root);

    // Map legacy UI globals to the new layout blocks
    left_column_box = GTK_WIDGET(gtk_builder_get_object(b_main, "left_column_box"));
    command_post_box = cmd_root;
    right_vbox = play_root;

    video_preview = GTK_WIDGET(gtk_builder_get_object(b_stack, "video_preview"));
    gtk_widget_add_tick_callback(video_preview, update_video_preview, NULL, NULL);

    // Provide specific builders to subsystem initializers
    ui_dashboard_init(b_dash, b_wave);
    ui_command_post_init(b_cmd, b_stack, window);
    ui_multitrack_init(b_multi, b_stack);

    // Map dynamic playlist components into the XML playlist root
    GtkWidget *playlist_widget = create_playlist_widget();
    gtk_box_append(GTK_BOX(play_root), playlist_widget);

    // Map looper logic to the XML multitrack scroll window
    GtkWidget *looper_scroll = GTK_WIDGET(gtk_builder_get_object(b_multi, "multitrack_tracks_scroll"));
    GtkWidget *looper_list = create_multitrack_tracks_widget();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(looper_scroll), looper_list);

    // Setup waveform drawing handlers through the extracted b_wave pointers
    waveform_area_bt = GTK_WIDGET(gtk_builder_get_object(b_wave, "waveform_area_bt"));
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(waveform_area_bt), on_draw_bt_waveform, NULL, NULL);
    gtk_widget_add_tick_callback(waveform_area_bt, (GtkTickCallback)on_waveform_tick, NULL, NULL);

    GtkGesture *drag_gesture_bt = gtk_gesture_drag_new();
    g_signal_connect(drag_gesture_bt, "drag-begin", G_CALLBACK(on_drag_begin), NULL);
    g_signal_connect(drag_gesture_bt, "drag-update", G_CALLBACK(on_drag_update), NULL);
    g_signal_connect(drag_gesture_bt, "drag-end", G_CALLBACK(on_drag_end), NULL);
    gtk_widget_add_controller(waveform_area_bt, GTK_EVENT_CONTROLLER(drag_gesture_bt));

    // NEW: Anchor Selection Click Gesture
    GtkGesture *click_gesture_bt = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click_gesture_bt), GDK_BUTTON_PRIMARY);
    g_signal_connect(click_gesture_bt, "pressed", G_CALLBACK(on_waveform_click_pressed), NULL);
    gtk_widget_add_controller(waveform_area_bt, GTK_EVENT_CONTROLLER(click_gesture_bt));

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

    waveform_scrollbar = GTK_WIDGET(gtk_builder_get_object(b_wave, "waveform_scrollbar"));
    waveform_adj = gtk_adjustment_new(0.0, 0.0, 1.0, 0.01, 0.1, 1.0);
    gtk_scrollbar_set_adjustment(GTK_SCROLLBAR(waveform_scrollbar), waveform_adj);
    g_signal_connect(waveform_adj, "value-changed", G_CALLBACK(on_scrollbar_changed), NULL);

    // Unref modular builders
    g_object_unref(b_main);
    g_object_unref(b_stack);
    g_object_unref(b_multi);
    g_object_unref(b_play);
    g_object_unref(b_cmd);
    g_object_unref(b_dash);
    g_object_unref(b_wave);

    gtk_window_present(GTK_WINDOW(window));

    int audio_status = 0;
    if (client) {
        audio_status = init_audio_engine(1048576, ui_state.config.input_gain_multiplier, client, ui_state.config.audio_device, ui_state.config.playback_target);
        set_bt_gain(ui_state.config.bt_gain_multiplier);
    }

    init_video_queue(&video_queue, 64);
    int video_status = init_and_start_video_engine(ui_state.config.video_device, &video_queue);

    adjust_blank_canvas_for_mode(ui_state.config.multitrack_mode_active != 0);

    if (!client) {
        gtk_label_set_markup(GTK_LABEL(lbl_status), "<span foreground='#ff4444'><b>FATAL ERROR: JACK Audio Server is not running.</b></span>");
        gtk_widget_set_sensitive(btn_play, FALSE);
        gtk_widget_set_sensitive(btn_record, FALSE);
    } else if (audio_status == 2 || video_status != 0 || expected_is_offline) {
        if (audio_status == 2 || video_status != 0) {
            gtk_label_set_text(GTK_LABEL(lbl_status), "Status: Running with missing hardware");
        } else {
            gtk_label_set_text(GTK_LABEL(lbl_status), "Status: Ready");
        }
    } else {
        gtk_label_set_text(GTK_LABEL(lbl_status), "Status: Ready");
    }

    // Trigger CLI injected files
    if (strlen(ui_state.selected_track_path) > 0) {
        if (g_str_has_suffix(ui_state.selected_track_path, ".qjams")) {
            extern void command_post_load_session(const char *path);
            command_post_load_session(ui_state.selected_track_path);
        } else {
            trigger_track_load();
        }
    }
}

int main(int argc, char **argv) {
    char exe_path[1024];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len != -1) {
        exe_path[len] = '\0';
        char *last_slash = strrchr(exe_path, '/');
        if (last_slash) {
            *last_slash = '\0';
            snprintf(ui_state.config_path, sizeof(ui_state.config_path), "%s/qjams.config", exe_path);
            snprintf(ui_state.playlist_path, sizeof(ui_state.playlist_path), "%s/qjams.m3u", exe_path);
        }
    }

    GtkApplication *app = gtk_application_new("com.rob.qjams", G_APPLICATION_HANDLES_OPEN);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    g_signal_connect(app, "open", G_CALLBACK(on_open), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);

    // --- STRICT SHUTDOWN SEQUENCE ---
    stop_video_engine();     // Explicitly release the V4L2 camera lock
    shutdown_audio_engine(); // Free the JACK audio buffers

    // Free the UI preview queue memory leak
    if (preview_queue.buffer) {
        free(preview_queue.buffer);
        preview_queue.buffer = NULL;
    }

    // Wipe the final raw MKV file from the /tmp partition
    extern char current_raw_path[1024];
    if (strlen(current_raw_path) > 0) {
        remove(current_raw_path);
    }

    return status;
}
