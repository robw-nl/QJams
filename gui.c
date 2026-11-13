#include "audio_core.h"
#include "audio_memory.h"
#include "audio_routing.h"
#include "config.h"
#include "scanner.h"
#include "ui_command_post.h"
#include "ui_dashboard.h"
#include "ui_globals.h"
#include "ui_lifecycle.h"
#include "ui_multitrack.h"
#include "ui_playlist.h"
#include "ui_shortcuts.h"
#include "ui_transport.h"
#include "ui_waveforms.h"
#include "video_engine.h"
#include <glib-unix.h> // For g_unix_fd_add
#include <gtk/gtk.h>
#include <jack/jack.h>
#include <libavutil/log.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <unistd.h>

extern _Atomic bool mkv_video_keep_running;

SPSC_Preview_Queue preview_queue;

QJamsUIState ui_state = {.selected_track_path = "", .is_existing_session = false};

GtkWidget *lbl_input_device;
GtkWidget *input_gain_spinner;
GtkWidget *lbl_track;
GtkWidget *lbl_status;
GtkWidget *lbl_hw_toast;
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

GtkWidget *lbl_multitrack_status;

GtkWidget *btn_load;
GtkWidget *btn_load_session;
GtkWidget *btn_load_playlist;
GtkWidget *btn_save_session;
GtkWidget *btn_save_playlist;
GtkWidget *btn_save_mux;
GtkWidget *btn_settings;
GtkWidget *btn_prev;
GtkWidget *btn_stop;
GtkWidget *btn_next;
GtkWidget *btn_speed;
GtkWidget *btn_reset;

static GtkWidget *video_preview;
GtkWidget *video_preview_frame = NULL;

void purge_autosave_file(void) {
    char autosave_path[1024];
    snprintf(autosave_path, sizeof(autosave_path), "%s/.autosave.qjams",
             ui_state.config.recordings_dir);
    remove(autosave_path);
}

void trigger_global_ui_refresh(void) {
    update_multitrack_status_ui();

    update_zoom_button_label_to_length();

    invalidate_waveform_caches();

    if (waveform_area_bt)
        gtk_widget_queue_draw(waveform_area_bt);
    if (waveform_area_input)
        gtk_widget_queue_draw(waveform_area_input);
}

void set_app_waiting_state(bool waiting) {
    GtkWindow *win =
        gtk_application_get_active_window(GTK_APPLICATION(g_application_get_default()));
    if (!win)
        return;

    GtkWidget *master_box = gtk_window_get_child(win);
    if (master_box) {
        gtk_widget_set_sensitive(master_box, !waiting);
    }

    if (main_spinner) {
        if (waiting) {
            gtk_spinner_start(GTK_SPINNER(main_spinner));
        } else {
            gtk_spinner_stop(GTK_SPINNER(main_spinner));
        }
    }

    if (waiting) {
        GdkCursor *cursor = gdk_cursor_new_from_name("wait", NULL);
        gtk_widget_set_cursor(GTK_WIDGET(win), cursor);
        g_object_unref(cursor);
    } else {
        gtk_widget_set_cursor(GTK_WIDGET(win), NULL);
    }
}

static gboolean update_video_preview(GtkWidget *widget, GdkFrameClock *frame_clock,
                                     gpointer user_data) {
    (void)widget;
    (void)frame_clock;
    (void)user_data;
    static int rendered_placeholder_w = 0;
    static int rendered_placeholder_h = 0;
    PreviewPayload payload;
    PreviewPayload last_payload;
    int frames_popped = 0;

    while (pop_preview_frame(&preview_queue, &payload)) {
        last_payload = payload;
        frames_popped++;
    }

    if (frames_popped > 0) {
        rendered_placeholder_w = 0;
        rendered_placeholder_h = 0;
        uint8_t *pixel_copy =
            g_memdup2(last_payload.rgba_data, last_payload.width * last_payload.height * 4);
        GBytes *bytes = g_bytes_new_take(pixel_copy, last_payload.width * last_payload.height * 4);
        GdkTexture *texture =
            gdk_memory_texture_new(last_payload.width, last_payload.height, GDK_MEMORY_R8G8B8A8,
                                   bytes, last_payload.width * 4);
        gtk_picture_set_paintable(GTK_PICTURE(widget), GDK_PAINTABLE(texture));
        g_object_unref(texture);
        g_bytes_unref(bytes);
    } else if (!is_camera_detected() && !is_mkv_preview_active()) {
        int w = atomic_load_explicit(&is_video_expanded, memory_order_acquire)
                    ? ui_state.config.video_width_expanded
                    : ui_state.config.video_width_default;
        if (w < 16)
            w = 480;
        int h = (int)(w * (9.0 / 16.0));
        if (w != rendered_placeholder_w || h != rendered_placeholder_h) {
            cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
            cairo_t *cr = cairo_create(surface);

            cairo_set_source_rgba(cr, 0.1, 0.1, 0.1, 1.0);
            cairo_paint(cr);

            cairo_set_source_rgba(cr, 0.5, 0.5, 0.5, 1.0);
            cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
            cairo_set_font_size(cr, 14.0);

            const char *text = "Camera not detected";
            cairo_text_extents_t extents;
            cairo_text_extents(cr, text, &extents);
            cairo_move_to(cr, (w - extents.width) / 2.0, (h + extents.height) / 2.0);
            cairo_show_text(cr, text);

            cairo_destroy(cr);

            int stride = cairo_image_surface_get_stride(surface);
            unsigned char *data = cairo_image_surface_get_data(surface);
            uint8_t *pixel_copy = g_memdup2(data, stride * h);
            GBytes *bytes = g_bytes_new_take(pixel_copy, stride * h);

            GdkTexture *texture =
                gdk_memory_texture_new(w, h, GDK_MEMORY_B8G8R8A8_PREMULTIPLIED, bytes, stride);
            gtk_picture_set_paintable(GTK_PICTURE(widget), GDK_PAINTABLE(texture));

            g_object_unref(texture);
            g_bytes_unref(bytes);
            cairo_surface_destroy(surface);

            rendered_placeholder_w = w;
            rendered_placeholder_h = h;
        }
    }
    return G_SOURCE_CONTINUE;
}

static void on_scrollbar_changed(GtkAdjustment *adjustment, gpointer user_data) {
    (void)adjustment;
    (void)user_data;
    invalidate_waveform_caches();
    if (waveform_area_bt)
        gtk_widget_queue_draw(waveform_area_bt);
    if (waveform_area_input)
        gtk_widget_queue_draw(waveform_area_input);
}

static gboolean on_waveform_drop(GtkDropTarget *target, const GValue *value, double x, double y,
                                 gpointer user_data) {
    (void)y;
    (void)user_data;
    int rec_track = atomic_load_explicit(&current_recording_track, memory_order_acquire);
    return handle_track_drop_payload(target, value, rec_track, x);
}
static bool cli_file_loaded = false;
static void on_open(GtkApplication *app, GFile **files, gint n_files, const gchar *hint,
                    gpointer user_data) {
    (void)hint;
    (void)user_data;
    GList *windows = gtk_application_get_windows(app);
    bool is_running = (windows != NULL);
    if (n_files > 0) {
        char *path = g_file_get_path(files[0]);
        if (path) {
            cli_file_loaded = true;
            if (g_str_has_suffix(path, ".m3u")) {
                strncpy(ui_state.config.last_playlist_path, path,
                        sizeof(ui_state.config.last_playlist_path) - 1);
                ui_state.config.last_playlist_path[sizeof(ui_state.config.last_playlist_path) - 1] =
                    '\0';
                strncpy(ui_state.playlist_path, path, sizeof(ui_state.playlist_path) - 1);
                ui_state.playlist_path[sizeof(ui_state.playlist_path) - 1] = '\0';
                if (is_running)
                    load_playlist_from_file(path);
            } else {
                strncpy(ui_state.selected_track_path, path,
                        sizeof(ui_state.selected_track_path) - 1);
                ui_state.selected_track_path[sizeof(ui_state.selected_track_path) - 1] = '\0';
                if (is_running) {
                    int rec_track =
                        atomic_load_explicit(&current_recording_track, memory_order_acquire);
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
static void on_activate(GtkApplication *app, gpointer user_data) {
    (void)user_data;
    GtkCssProvider *css_provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(
        css_provider,
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
        ".multitrack-canvas { border-radius: 4px; }\n"
        ".multitrack-row { border-bottom: 1px solid alpha(currentColor, 0.15); padding: 4px; }\n"
        ".multitrack-row.active-row { background-color: #3584e4; color: #ffffff; }\n"
        "editablelabel.editing { background: #ffffff; border: 1px solid #3584e4; border-radius: "
        "4px; padding: 2px 6px; }\n"
        "editablelabel.editing text { color: #000000; caret-color: #000000; font-weight: 600; }\n"
        "editablelabel.editing text selection { background: #3584e4; color: #ffffff; }\n"
        "frame { border: 1px solid rgba(128, 128, 128, 0.4); border-radius: 6px; }\n"
        ".header-title { font-weight: 800; padding: 2px 2px; }\n"
        ".needs-save { background: #b3ffc6; color: black; font-weight: bold; transition: "
        "background 0.3s ease; }\n"
        ".active-preset { background: #b3ffc6; color: black; font-weight: bold; border-color: "
        "#265740; }\n"
        "spinbutton.compact-spin { min-width: 40px; }\n"
        "spinbutton.compact-spin text { min-width: 25px; padding-left: 2px; padding-right: 2px; }\n"
        ".audio-dashboard { background-color: #f0f0f0; border-bottom: 1px solid #d4d4d4; padding: "
        "6px 10px; }\n"
        ".audio-dashboard label { color: #2e2e2e; }\n"
        ".transport-pane { background-color: #ffffff; border: 1px solid #d4d4d4; border-radius: "
        "4px; padding: 16px 20px; margin-top: 5px; }\n"
        ".transport-pane button { border-radius: 3px; }\n"
        "#lbl_status, #lbl_hw_toast { color: #4a4a4a; font-weight: bold; padding-top: 4px; }\n");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                               GTK_STYLE_PROVIDER(css_provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css_provider);
    char cli_saved_track[512] = "";
    char cli_saved_playlist[1024] = "";
    if (cli_file_loaded) {
        strncpy(cli_saved_track, ui_state.selected_track_path, sizeof(cli_saved_track) - 1);
        strncpy(cli_saved_playlist, ui_state.playlist_path, sizeof(cli_saved_playlist) - 1);
    }
    ui_state.config = load_qjams_config(ui_state.config_path);
    if (cli_file_loaded) {
        if (strlen(cli_saved_track) > 0) {
            strncpy(ui_state.selected_track_path, cli_saved_track,
                    sizeof(ui_state.selected_track_path) - 1);
        }
        if (strlen(cli_saved_playlist) > 0) {
            strncpy(ui_state.playlist_path, cli_saved_playlist, sizeof(ui_state.playlist_path) - 1);
            strncpy(ui_state.config.last_playlist_path, cli_saved_playlist,
                    sizeof(ui_state.config.last_playlist_path) - 1);
        }
    }
    bool expected_is_offline = false;
    char expected_audio[128] = "";
    if (strlen(ui_state.config.last_playlist_path) > 0) {
        strncpy(ui_state.playlist_path, ui_state.config.last_playlist_path,
                sizeof(ui_state.playlist_path) - 1);
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

    QJVideoDevice v_devices[QJ_MAX_CAMERAS];
    int v_count = scan_video_devices(v_devices, QJ_MAX_CAMERAS);
    log_and_notify(0, "[Video] Found %d capture devices:\n", v_count);
    for (int i = 0; i < v_count; i++)
        log_and_notify(0, "  %d: %s (%s)\n", i + 1, v_devices[i].device_name,
                       v_devices[i].device_path);

    if (client) {
        QJAudioDevice a_devices[QJ_MAX_AUDIO_DEVICES];
        int a_count = scan_audio_devices(client, a_devices, QJ_MAX_AUDIO_DEVICES);
        log_and_notify(0, "\n[Audio] Found %d physical capture devices:\n", a_count);
        for (int i = 0; i < a_count; i++)
            log_and_notify(0, "  %d: %s [%d channels]\n", i + 1, a_devices[i].display_name,
                           a_devices[i].channel_count);

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
            if (exact_match)
                continue;

            for (int k = 0; k < usb_count; k++) {
                if ((strstr(stale_name, "Cortex") && strstr(usb_hardware[k], "Cortex")) ||
                    (strstr(stale_name, "SA9226") && strstr(usb_hardware[k], "SA9226")) ||
                    (strstr(stale_name, "Logitech") && strstr(usb_hardware[k], "Logitech"))) {

                    for (int i = 0; i < a_count; i++) {
                        if ((strstr(a_devices[i].display_name, "Cortex") &&
                             strstr(usb_hardware[k], "Cortex")) ||
                            (strstr(a_devices[i].display_name, "SA9226") &&
                             strstr(usb_hardware[k], "SA9226")) ||
                            (strstr(a_devices[i].display_name, "Logitech") &&
                             strstr(usb_hardware[k], "Logitech"))) {

                            log_and_notify(0,
                                           "[HW-HEAL] USB topology changed. Re-mapping stale "
                                           "profile '%s' to active port '%s'\n",
                                           stale_name, a_devices[i].display_name);

                            if (strcmp(ui_state.config.audio_device, stale_name) == 0) {
                                strncpy(ui_state.config.audio_device, a_devices[i].display_name,
                                        127);
                                ui_state.config.audio_device[127] = '\0';
                            }

                            strncpy(ui_state.config.audio_profiles[p].device_name,
                                    a_devices[i].display_name, 127);
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
                        if (strcmp(ui_state.config.audio_profiles[p].device_name,
                                   a_devices[i].display_name) == 0) {
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
                log_and_notify(
                    0, "-> Pre-Boot Routing: Expected devices missing. Ultimate fallback to %s\n",
                    target_audio);
            }
        }

        if (strlen(target_audio) > 0) {
            strncpy(ui_state.config.audio_device, target_audio, 127);
            ui_state.config.audio_device[127] = '\0';
            for (int p = 0; p < ui_state.config.num_audio_profiles; p++) {
                if (strcmp(ui_state.config.audio_profiles[p].device_name, target_audio) == 0) {
                    ui_state.config.input_gain_multiplier =
                        ui_state.config.audio_profiles[p].gain_multiplier;
                    break;
                }
            }
            log_and_notify(0, "-> Pre-Boot Routing: Target explicitly forced to %s\n",
                           target_audio);
        }
    }
    log_and_notify(0, "======================================\n\n");

    init_preview_queue(&preview_queue, 32);

    GtkBuilder *b_main = gtk_builder_new_from_resource("/com/rob/qjams/ui/main_window.ui");
    GtkBuilder *b_multi = gtk_builder_new_from_resource("/com/rob/qjams/ui/multitrack.ui");
    GtkBuilder *b_play = gtk_builder_new_from_resource("/com/rob/qjams/ui/playlist.ui");
    GtkBuilder *b_cmd = gtk_builder_new_from_resource("/com/rob/qjams/ui/command_post.ui");
    GtkBuilder *b_dash = gtk_builder_new_from_resource("/com/rob/qjams/ui/dashboard.ui");
    GtkBuilder *b_wave = gtk_builder_new_from_resource("/com/rob/qjams/ui/waveforms.ui");

    GtkWindow *window = GTK_WINDOW(gtk_builder_get_object(b_main, "main_window"));
    gtk_window_set_application(window, app);
    gtk_window_set_default_size(window, ui_state.config.window_width,
                                ui_state.config.window_height);
    g_signal_connect(window, "close-request", G_CALLBACK(on_window_close), NULL);

    GtkEventController *key_ctrl = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(key_ctrl, GTK_PHASE_CAPTURE);
    g_signal_connect(key_ctrl, "key-pressed", G_CALLBACK(on_window_key_pressed), NULL);
    gtk_widget_add_controller(GTK_WIDGET(window), key_ctrl);

    GtkBox *host_multi = GTK_BOX(gtk_builder_get_object(b_main, "multitrack_host"));
    GtkBox *host_play = GTK_BOX(gtk_builder_get_object(b_main, "playlist_host"));
    GtkBox *host_cmd = GTK_BOX(gtk_builder_get_object(b_main, "command_post_host"));
    GtkBox *host_dash = GTK_BOX(gtk_builder_get_object(b_main, "dashboard_host"));
    GtkBox *host_wave = GTK_BOX(gtk_builder_get_object(b_main, "waveforms_host"));

    GtkWidget *multi_page = GTK_WIDGET(gtk_builder_get_object(b_multi, "multitrack_page_root"));
    GtkWidget *play_root = GTK_WIDGET(gtk_builder_get_object(b_play, "playlist_root"));
    GtkWidget *cmd_root = GTK_WIDGET(gtk_builder_get_object(b_cmd, "command_post_root"));
    GtkWidget *dash_root = GTK_WIDGET(gtk_builder_get_object(b_dash, "dashboard_root"));
    GtkWidget *wave_root = GTK_WIDGET(gtk_builder_get_object(b_wave, "waveforms_root"));

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

    int init_video_w = ui_state.config.video_width_default;
    int init_video_h = (int)(init_video_w * (9.0 / 16.0));
    gtk_widget_set_size_request(video_preview_frame, init_video_w, init_video_h);
    gtk_widget_set_size_request(video_preview, init_video_w, init_video_h);

    ui_dashboard_init(b_dash, b_wave);

    // Dynamically override the text emoji with the colorable native symbolic icon
    if (btn_reset)
        gtk_button_set_icon_name(GTK_BUTTON(btn_reset), "user-trash-symbolic");

    ui_command_post_init(b_cmd, window);
    ui_multitrack_init(b_multi);

    GtkWidget *playlist_widget = create_playlist_widget();
    gtk_box_append(GTK_BOX(play_root), playlist_widget);

    GtkWidget *multitrack_scroll =
        GTK_WIDGET(gtk_builder_get_object(b_multi, "multitrack_tracks_scroll"));
    GtkWidget *multitrack_list = create_multitrack_tracks_widget();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(multitrack_scroll), multitrack_list);

    waveform_area_bt = GTK_WIDGET(gtk_builder_get_object(b_wave, "waveform_area_bt"));
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(waveform_area_bt), on_draw_bt_waveform, NULL,
                                   NULL);
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
    g_signal_connect(waveform_area_bt, "query-tooltip", G_CALLBACK(on_waveform_query_tooltip),
                     NULL);

    GType drop_types[] = {GDK_TYPE_FILE_LIST, QJ_TYPE_TRACK};
    GtkDropTarget *drop_target = gtk_drop_target_new(G_TYPE_INVALID, GDK_ACTION_COPY);
    gtk_drop_target_set_gtypes(drop_target, drop_types, 2);
    g_signal_connect(drop_target, "drop", G_CALLBACK(on_waveform_drop), NULL);
    gtk_widget_add_controller(waveform_area_bt, GTK_EVENT_CONTROLLER(drop_target));

    waveform_area_input = GTK_WIDGET(gtk_builder_get_object(b_wave, "waveform_area_input"));
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(waveform_area_input), on_draw_input_waveform,
                                   NULL, NULL);
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
    g_signal_connect(waveform_area_input, "query-tooltip", G_CALLBACK(on_waveform_query_tooltip),
                     NULL);

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

    int audio_status = 0;
    if (client) {
        audio_status =
            init_audio_engine(16384, ui_state.config.input_gain_multiplier, client,
                              ui_state.config.audio_device, ui_state.config.playback_target);
        set_bt_gain(ui_state.config.bt_gain_multiplier);
    }

    g_unix_fd_add(get_jack_event_fd(), G_IO_IN, on_jack_pipe_event, NULL);

    // BOOT-TIME VIDEO HEALING LOGIC
    if (v_count > 0) {
        bool video_online = false;
        for (int i = 0; i < v_count; i++) {
            if (strcmp(ui_state.config.video_device, v_devices[i].device_path) == 0) {
                video_online = true;
                break;
            }
        }

        if (!video_online) {
            log_and_notify(0, "[HW-HEAL] Saved video device stale. Auto-recovering to %s\n",
                           v_devices[0].device_path);
            strncpy(ui_state.config.video_device, v_devices[0].device_path,
                    sizeof(ui_state.config.video_device) - 1);
            ui_state.config.video_device[sizeof(ui_state.config.video_device) - 1] = '\0';
            save_qjams_config(ui_state.config_path, &ui_state.config);
        }
    }


    init_video_queue(&video_queue, 64);
    int video_status = init_and_start_video_engine(ui_state.config.video_device, &video_queue);
    atomic_store_explicit(&rt_loop_countdown_enabled, ui_state.config.loop_countdown_active != 0,
                          memory_order_release);

    if (!is_camera_detected()) {
        if (btn_record_video)
            gtk_widget_set_sensitive(btn_record_video, FALSE);
        atomic_store_explicit(&video_is_armed, false, memory_order_release);
    }
    bool loading_cli_session = (strlen(ui_state.selected_track_path) > 0 &&
                                g_str_has_suffix(ui_state.selected_track_path, ".qjams"));
    if (!loading_cli_session) {
        adjust_blank_canvas();
    }
    if (!client) {
        log_and_notify(1, "<span foreground='#ff4444'><b>FATAL ERROR: JACK Audio Server is not "
                          "running.</b></span>");
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
            command_post_load_session(ui_state.selected_track_path);
        } else {
            int rec_track = atomic_load_explicit(&current_recording_track, memory_order_acquire);
            multitrack_import_file_async(ui_state.selected_track_path, rec_track);
        }
    }

    g_idle_add(check_for_crash_recovery, window);
    g_timeout_add(300000, idle_autosave_tick, NULL); // 300000 = 5 minutes autosave interval

    g_timeout_add(100, transport_monitor_tick, NULL);
}

static gboolean on_posix_signal(gpointer user_data) {
    GtkApplication *app = GTK_APPLICATION(user_data);
    log_and_notify(0, "\n[SYSTEM] Intercepted POSIX kill signal. Executing clean teardown...\n");

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
                    snprintf(stash_path, sizeof(stash_path), "%s/%s", boot_config.recordings_dir,
                             filename);
                    remove(stash_path);
                }
            }
            g_dir_close(dir);
        }
    }

    remove("/tmp/qjams_session_video.mkv");

    init_qjams_logger(qjams_dir, boot_config.enable_logging);
    av_log_set_level(AV_LOG_FATAL);

    const char *music_dir = g_get_user_special_dir(G_USER_DIRECTORY_MUSIC);
    if (!music_dir)
        music_dir = g_get_home_dir();
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

    atomic_store_explicit(&engine_is_shutting_down, true, memory_order_release);
    stop_video_engine();
    shutdown_audio_engine();

    if (preview_queue.buffer) {
        free(preview_queue.buffer);
        preview_queue.buffer = NULL;
    }

    if (strlen(current_raw_path) > 0) {
        remove(current_raw_path);
    }

    char autosave_path[1024];
    snprintf(autosave_path, sizeof(autosave_path), "%s/.autosave.qjams",
             ui_state.config.recordings_dir);
    remove(autosave_path);

    close_qjams_logger();
    return status;
}
