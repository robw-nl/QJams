#include <gtk/gtk.h>
#include <jack/jack.h>
#include <math.h>
#include <stdbool.h>
#include "audio_engine.h"
#include "video_engine.h"
#include "scanner.h"
#include "config.h"
#include "ui_globals.h"
#include "ui_playlist.h"
#include "ui_waveforms.h"
#include "ui_looper.h"
#include "ui_dashboard.h"
#include "ui_command_post.h"
#include <unistd.h>

extern jack_client_t *client;
extern SPSC_Video_Queue video_queue;
SPSC_Preview_Queue preview_queue;

// --- INSTANTIATE UI GLOBALS ---
QJamsUIState ui_state = { .is_loading_track = false, .selected_track_path = "", .freestyle_duration_min = 15, .looper_duration_min = 5, .session_is_dirty = false };

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
GtkWidget *btn_looper_mode;
GtkWidget *lbl_looper_status;
GtkWidget *btn_looper_undo;
GtkWidget *btn_looper_next;
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

typedef struct {
    int audio_failed;
    int video_failed;
} AlertRetryData;

static void noop_free(gpointer data) { (void)data; }

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

static gboolean on_window_close(GtkWindow *window, gpointer user_data) {
    (void)user_data;
    if (active_muxer_pid != 0) {
        GtkAlertDialog *alert = gtk_alert_dialog_new("Muxing in Progress");
        gtk_alert_dialog_set_detail(alert, "Please wait for FFmpeg to finish processing the media file before closing QJams.");
        gtk_alert_dialog_show(alert, window);
        g_object_unref(alert);
        return TRUE;
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
        GBytes *bytes = g_bytes_new_with_free_func(last_payload.rgba_data, last_payload.width * last_payload.height * 4, noop_free, NULL);
        GdkTexture *texture = gdk_memory_texture_new(last_payload.width, last_payload.height, GDK_MEMORY_R8G8B8A8, bytes, last_payload.width * 4);
        gtk_picture_set_paintable(GTK_PICTURE(widget), GDK_PAINTABLE(texture));
        g_object_unref(texture);
        g_bytes_unref(bytes);
    }
    return G_SOURCE_CONTINUE;
}

static void on_hardware_alert_finished(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    GtkAlertDialog *alert = GTK_ALERT_DIALOG(source_object);
    AlertRetryData *ard = (AlertRetryData*)user_data;

    gtk_alert_dialog_choose_finish(alert, res, NULL);
    printf("\n=== Dialog Dismissed: Triggering Hardware Hot-Patch ===\n");

    if (ard->audio_failed) {
        patch_playback_ports(ui_state.config.playback_target);
        if (patch_audio_ports(ui_state.config.audio_device) == 0) {
            printf("-> Audio Hot-Patch successful.\n");
            ard->audio_failed = 0;
        } else {
            printf("-> Audio Hot-Patch failed.\n");
        }
    }

    if (ard->video_failed) {
        if (init_and_start_video_engine(ui_state.config.video_device, &video_queue) == 0) {
            printf("-> Video Hot-Patch successful.\n");
            ard->video_failed = 0;
        } else {
            printf("-> Video Hot-Patch failed.\n");
        }
    }

    if (ard->audio_failed || ard->video_failed) {
        gtk_label_set_text(GTK_LABEL(lbl_status), "Status: Running with missing hardware");
    } else {
        gtk_label_set_text(GTK_LABEL(lbl_status), "Status: Ready");
    }

    if (atomic_load_explicit(&is_looper_mode, memory_order_acquire)) {
        update_looper_status_ui();
    }

    free(ard);
}

static gboolean on_window_key_pressed(GtkEventControllerKey *controller, guint keyval, guint keycode, GdkModifierType state, gpointer user_data) {
    GtkWidget *widget = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller));
    GtkWidget *focus_widget = gtk_root_get_focus(gtk_widget_get_root(widget));

    if (focus_widget && GTK_IS_EDITABLE(focus_widget)) {
        if (gtk_editable_get_editable(GTK_EDITABLE(focus_widget))) return FALSE;
    }

    (void)keycode; (void)state; (void)user_data;

    switch (keyval) {
        case GDK_KEY_space:
        case GDK_KEY_p: case GDK_KEY_P:
            if (gtk_widget_is_sensitive(btn_play)) on_play_clicked(GTK_BUTTON(btn_play), NULL);
            return TRUE;
        case GDK_KEY_r: case GDK_KEY_R:
            if (gtk_widget_is_sensitive(btn_record)) on_start_clicked(GTK_BUTTON(btn_record), NULL);
            return TRUE;
        case GDK_KEY_s: case GDK_KEY_S:
            if (gtk_widget_is_sensitive(btn_stop)) on_stop_clicked(GTK_BUTTON(btn_stop), NULL);
            return TRUE;
        case GDK_KEY_c: case GDK_KEY_C:
            if (gtk_widget_is_sensitive(btn_speed)) on_speed_clicked(GTK_BUTTON(btn_speed), NULL);
            return TRUE;
        case GDK_KEY_l: case GDK_KEY_L:
            if (gtk_widget_is_sensitive(btn_load)) on_select_track_clicked(GTK_BUTTON(btn_load), gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller)));
            return TRUE;
        case GDK_KEY_x: case GDK_KEY_X:
            if (gtk_widget_is_sensitive(btn_save_mux)) on_save_mux_clicked(GTK_BUTTON(btn_save_mux), NULL);
            return TRUE;
        case GDK_KEY_Escape:
            if (btn_reset && gtk_widget_is_sensitive(btn_reset)) {
                g_signal_emit_by_name(btn_reset, "clicked");
            }
            return TRUE;
        case GDK_KEY_Left:
            apply_keyboard_seek(-1.0);
            return TRUE;
        case GDK_KEY_Right:
            apply_keyboard_seek(1.0);
            return TRUE;
        case GDK_KEY_Home:
            if (!atomic_load_explicit(&engine_is_recording, memory_order_acquire)) seek_backing_track(0.0);
            return TRUE;
        case GDK_KEY_End:
            if (!atomic_load_explicit(&engine_is_recording, memory_order_acquire)) seek_backing_track(1.0);
            return TRUE;
        case GDK_KEY_F2:
            looper_start_rename();
            return TRUE;
    }
    return FALSE;
}

void apply_video_preview_size(int width, int height) {
    if (!left_column_box || !right_vbox || !command_post_box || !video_preview) return;
    gtk_widget_set_size_request(left_column_box, width, -1);
    gtk_widget_set_size_request(video_preview, width, height);
    if (mode_stack) gtk_widget_set_size_request(mode_stack, width, -1);

    int cmd_width = 290;
    int playlist_width = width - cmd_width - 10;
    gtk_widget_set_size_request(right_vbox, playlist_width, -1);
    gtk_widget_set_size_request(command_post_box, cmd_width, -1);
}

static void on_scrollbar_changed(GtkAdjustment *adjustment, gpointer user_data) {
    (void)adjustment; (void)user_data;
    invalidate_waveform_caches();
    if (waveform_area_bt) gtk_widget_queue_draw(waveform_area_bt);
    if (waveform_area_input) gtk_widget_queue_draw(waveform_area_input);
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
                                      ".looper-canvas { background-color: #ffffff; border-radius: 4px; }\n"
                                      ".looper-row { background-color: #ffffff; border-bottom: 1px solid #eeeeee; padding: 4px; }\n"
                                      ".looper-row.selected { background-color: #dbf0ff; }\n"
                                      "editablelabel.editing { background: #ffffff !important; border: 1px solid #3584e4 !important; border-radius: 4px; padding: 2px 6px; }\n"
                                      "editablelabel.editing text { color: #000000 !important; caret-color: #000000 !important; font-weight: 900 !important; }\n"
                                      "editablelabel.editing text selection { background: #3584e4 !important; color: #ffffff !important; }\n"
                                      "frame { border: 1px solid rgba(128, 128, 128, 0.4); border-radius: 6px; }\n"
                                      ".needs-save { background: #b3ffc6; color: black; font-weight: bold; transition: background 0.3s ease; }\n");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(css_provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    ui_state.config = load_qjams_config(ui_state.config_path);

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
    }
    printf("======================================\n\n");

    init_preview_queue(&preview_queue, 32);

    GtkBuilder *builder = gtk_builder_new_from_resource("/com/rob/qjams/qjams.ui");
    GtkWindow *window = GTK_WINDOW(gtk_builder_get_object(builder, "main_window"));
    gtk_window_set_application(window, app);
    gtk_window_set_default_size(window, ui_state.config.window_width, ui_state.config.window_height);
    g_signal_connect(window, "close-request", G_CALLBACK(on_window_close), NULL);

    GtkEventController *key_ctrl = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(key_ctrl, GTK_PHASE_CAPTURE);
    g_signal_connect(key_ctrl, "key-pressed", G_CALLBACK(on_window_key_pressed), NULL);
    gtk_widget_add_controller(GTK_WIDGET(window), key_ctrl);

    left_column_box = GTK_WIDGET(gtk_builder_get_object(builder, "left_column_box"));
    command_post_box = GTK_WIDGET(gtk_builder_get_object(builder, "command_post_box"));
    right_vbox = GTK_WIDGET(gtk_builder_get_object(builder, "right_vbox"));

    video_preview = GTK_WIDGET(gtk_builder_get_object(builder, "video_preview"));
    gtk_widget_add_tick_callback(video_preview, update_video_preview, NULL, NULL);

    ui_dashboard_init(builder);
    ui_command_post_init(builder, window);
    ui_looper_init(builder);

    GtkWidget *playlist_widget = create_playlist_widget();
    gtk_box_append(GTK_BOX(right_vbox), playlist_widget);

    apply_video_preview_size(ui_state.config.video_preview_width, ui_state.config.video_preview_height);

    GtkWidget *looper_scroll = GTK_WIDGET(gtk_builder_get_object(builder, "looper_layers_scroll"));
    GtkWidget *looper_list = create_looper_layers_widget();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(looper_scroll), looper_list);

    waveform_area_bt = GTK_WIDGET(gtk_builder_get_object(builder, "waveform_area_bt"));
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(waveform_area_bt), on_draw_bt_waveform, NULL, NULL);
    gtk_widget_add_tick_callback(waveform_area_bt, (GtkTickCallback)on_waveform_tick, NULL, NULL);

    GtkGesture *drag_gesture_bt = gtk_gesture_drag_new();
    g_signal_connect(drag_gesture_bt, "drag-begin", G_CALLBACK(on_drag_begin), NULL);
    g_signal_connect(drag_gesture_bt, "drag-update", G_CALLBACK(on_drag_update), NULL);
    g_signal_connect(drag_gesture_bt, "drag-end", G_CALLBACK(on_drag_end), NULL);
    gtk_widget_add_controller(waveform_area_bt, GTK_EVENT_CONTROLLER(drag_gesture_bt));

    waveform_area_input = GTK_WIDGET(gtk_builder_get_object(builder, "waveform_area_input"));
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(waveform_area_input), on_draw_input_waveform, NULL, NULL);
    gtk_widget_add_tick_callback(waveform_area_input, on_waveform_tick, NULL, NULL);

    GtkGesture *drag_gesture_in = gtk_gesture_drag_new();
    g_signal_connect(drag_gesture_in, "drag-begin", G_CALLBACK(on_drag_begin), NULL);
    g_signal_connect(drag_gesture_in, "drag-update", G_CALLBACK(on_drag_update), NULL);
    g_signal_connect(drag_gesture_in, "drag-end", G_CALLBACK(on_drag_end), NULL);
    gtk_widget_add_controller(waveform_area_input, GTK_EVENT_CONTROLLER(drag_gesture_in));

    waveform_scrollbar = GTK_WIDGET(gtk_builder_get_object(builder, "waveform_scrollbar"));
    waveform_adj = gtk_adjustment_new(0.0, 0.0, 1.0, 0.01, 0.1, 1.0);
    gtk_scrollbar_set_adjustment(GTK_SCROLLBAR(waveform_scrollbar), waveform_adj);

    g_signal_connect(waveform_adj, "value-changed", G_CALLBACK(on_scrollbar_changed), NULL);

    g_object_unref(builder);
    gtk_window_present(GTK_WINDOW(window));

    int audio_status = 0;
    if (client) {
        audio_status = init_audio_engine(131072, ui_state.config.input_gain_multiplier, client, ui_state.config.audio_device, ui_state.config.playback_target);
        set_bt_gain(ui_state.config.bt_gain_multiplier);
    }

    init_video_queue(&video_queue, 64);
    int video_status = init_and_start_video_engine(ui_state.config.video_device, &video_queue);

    if (audio_status == 2 || video_status != 0) {
        char warn_msg[512] = "";
        if (audio_status == 2) snprintf(warn_msg + strlen(warn_msg), sizeof(warn_msg) - strlen(warn_msg), "• Configured Audio Device (%s) not detected.\n", ui_state.config.audio_device);
        if (video_status != 0) snprintf(warn_msg + strlen(warn_msg), sizeof(warn_msg) - strlen(warn_msg), "• Configured Camera (%s) not detected.\n", ui_state.config.video_device);
        strcat(warn_msg, "\nPlease connect your hardware NOW.\n\nClose this message AFTER you have reconnected the device(s) to trigger an automatic live hot-patch.");

        GtkAlertDialog *alert = gtk_alert_dialog_new("Missing Hardware Detected");
        gtk_alert_dialog_set_detail(alert, warn_msg);

        AlertRetryData *ard = malloc(sizeof(AlertRetryData));
        ard->audio_failed = (audio_status == 2);
        ard->video_failed = (video_status != 0);

        gtk_label_set_text(GTK_LABEL(lbl_status), "Status: Awaiting hardware reconnection...");
        gtk_alert_dialog_choose(alert, GTK_WINDOW(window), NULL, on_hardware_alert_finished, ard);
        g_object_unref(alert);
    } else {
        gtk_label_set_text(GTK_LABEL(lbl_status), "Status: Ready");
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

    GtkApplication *app = gtk_application_new("com.rob.qjams", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);

    shutdown_audio_engine();
    return status;
}
