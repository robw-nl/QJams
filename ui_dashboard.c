#include "ui_dashboard.h"
#include "audio_core.h"
#include "audio_routing.h"
#include "scanner.h"
#include "ui_globals.h"
#include "ui_multitrack.h"
#include "ui_playlist.h"
#include "ui_transport.h"
#include "ui_waveforms.h"
#include "video_engine.h"
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <math.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>
#include <unistd.h>


_Atomic bool suppress_video_overwrite = false;
_Atomic bool is_loop_wrapping = false;

char current_raw_path[1024] = "";
char stashed_raw_path[1024] = "";

atomic_size_t video_start_frame = 0;
atomic_size_t video_end_frame = 0;
int video_track_index = -1;

static guint routing_timeout_id = 0;
static guint daemon_retry_id = 0;

static GtkWidget *telemetry_popover = NULL;
GtkWidget *btn_record_video = NULL;
GtkWidget *btn_loop_countdown = NULL;
GtkWidget *btn_overlay_clip = NULL;

_Atomic bool is_video_expanded;
GtkWidget *btn_video_size = NULL;

static GtkWidget *input_pref_drop = NULL;
static GtkStringList *pref_model = NULL;
static bool pref_is_updating = false;

static gboolean deferred_model_unref(gpointer data) {
    if (data)
        g_object_unref(data);
    return G_SOURCE_REMOVE;
}

static void on_input_gain_changed(GtkSpinButton *spin_button, gpointer user_data);

static void update_hardware_status_label(void) {
    QJAudioDevice active_devs[QJ_MAX_AUDIO_DEVICES];
    int online_count = scan_audio_devices(client, active_devs, QJ_MAX_AUDIO_DEVICES);

    char toast_msg[1024] = "";
    const char *primary_dev = NULL;
    const char *fallback_dev = NULL;

    for (int i = 0; i < ui_state.config.num_audio_profiles; i++) {
        if (ui_state.config.audio_profiles[i].is_primary) {
            primary_dev = ui_state.config.audio_profiles[i].device_name;
        }
        if (ui_state.config.audio_profiles[i].is_fallback) {
            fallback_dev = ui_state.config.audio_profiles[i].device_name;
        }
    }

    const char *devices_to_check[2] = {primary_dev, fallback_dev};

    for (int i = 0; i < 2; i++) {
        const char *dev_name = devices_to_check[i];
        if (!dev_name || strlen(dev_name) == 0)
            continue;

        if (i == 1 && primary_dev && strcmp(dev_name, primary_dev) == 0)
            continue;

        int is_online = 0;
        for (int j = 0; j < online_count; j++) {
            if (strcmp(dev_name, active_devs[j].display_name) == 0) {
                is_online = 1;
                break;
            }
        }

        if (!is_online) {
            char line[256];
            snprintf(line, sizeof(line), "%s: not detected\n", dev_name);
            strncat(toast_msg, line, sizeof(toast_msg) - strlen(toast_msg) - 1);
        }
    }

    size_t len = strlen(toast_msg);
    if (len > 0) {
        if (toast_msg[len - 1] == '\n')
            toast_msg[len - 1] = '\0';
        log_and_notify(2, "%s", toast_msg);
    } else {
        gtk_widget_set_visible(lbl_hw_toast, FALSE);
    }
}

static gboolean delayed_routing_cb(gpointer user_data) {
    (void)user_data;
    routing_timeout_id = 0;
    log_and_notify(0, "[GTK TIMER] Port debounce finished. Executing graph validation...\n");

    // 1. Recover Audio Graph
    update_dashboard_pref_ui();

    // 2. Recover Video Graph (V4L2 Zombie Node Pruning)
    QJVideoDevice v_devs[QJ_MAX_CAMERAS];
    int v_count = scan_video_devices(v_devs, QJ_MAX_CAMERAS);

    if (v_count > 0) {
        bool video_online = false;
        for (int i = 0; i < v_count; i++) {
            if (strcmp(ui_state.config.video_device, v_devs[i].device_path) == 0) {
                video_online = true;
                break;
            }
        }

        if (!video_online) {
            log_and_notify(0, "[HW-HEAL] Video capture lost. Auto-recovering to %s\n",
                           v_devs[0].device_path);
            strncpy(ui_state.config.video_device, v_devs[0].device_path,
                    sizeof(ui_state.config.video_device) - 1);
            ui_state.config.video_device[sizeof(ui_state.config.video_device) - 1] = '\0';

            stop_video_engine();
            init_and_start_video_engine(ui_state.config.video_device, &video_queue);
            save_qjams_config(ui_state.config_path, &ui_state.config);
        } else if (!is_video_engine_running()) {
            log_and_notify(0, "[HW-HEAL] Video device reconnected. Restarting engine.\n");
            init_and_start_video_engine(ui_state.config.video_device, &video_queue);
        }
        if (video_preview_frame)
            gtk_widget_set_visible(video_preview_frame, TRUE);
        if (btn_record_video)
            gtk_widget_set_sensitive(btn_record_video, TRUE);
    } else {
        log_and_notify(0, "[HW-HEAL] All video devices lost. Shutting down video engine.\n");
        stop_video_engine();
        if (video_preview_frame)
            gtk_widget_set_visible(video_preview_frame, FALSE);
        if (btn_record_video)
            gtk_widget_set_sensitive(btn_record_video, FALSE);
        atomic_store_explicit(&video_is_armed, false, memory_order_release);
    }

    return G_SOURCE_REMOVE;
}

static gboolean daemon_retry_cb(gpointer user_data) {
    (void)user_data;
    int status = reboot_audio_engine(ui_state.config.audio_device, ui_state.config.playback_target,
                                     ui_state.config.input_gain_multiplier);
    if (status != -1) {
        log_and_notify(0, "[DAEMON] Audio Daemon Restored.\n");
        daemon_retry_id = 0;
        update_dashboard_pref_ui();
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

gboolean on_jack_pipe_event(gint fd, GIOCondition condition, gpointer user_data) {
    (void)condition;
    (void)user_data;
    uint8_t event;
    while (read(fd, &event, sizeof(event)) > 0) {
        if (event == 1) {
            log_and_notify(0, "[IPC] Daemon crash detected.\n");
            if (routing_timeout_id > 0)
                g_source_remove(routing_timeout_id);
            routing_timeout_id = 0;
            on_stop_clicked(NULL, NULL);
            log_and_notify(1, "<span foreground='#ff4444'><b>Status: FATAL - Audio Daemon Offline. "
                              "Retrying...</b></span>");
            if (daemon_retry_id == 0)
                daemon_retry_id = g_timeout_add(1000, daemon_retry_cb, NULL);
        } else if (event == 2 || event == 3) {
            log_and_notify(0, "[IPC] Topology change detected (%s).\n",
                           event == 2 ? "ADDED" : "REMOVED");
            if (daemon_retry_id == 0) {
                if (routing_timeout_id > 0)
                    g_source_remove(routing_timeout_id);

                bool device_missing = false;
                if (event == 3) {
                    bool capture_connected =
                        (input_port_1 && jack_port_connected(input_port_1) > 0) ||
                        (input_port_2 && jack_port_connected(input_port_2) > 0);
                    if (!capture_connected) {
                        QJAudioDevice active_devs[QJ_MAX_AUDIO_DEVICES];
                        int online_count =
                            scan_audio_devices(client, active_devs, QJ_MAX_AUDIO_DEVICES);
                        bool found = false;
                        for (int i = 0; i < online_count; i++) {
                            if (strcmp(ui_state.config.audio_device, active_devs[i].display_name) ==
                                0) {
                                found = true;
                                break;
                            }
                        }
                        if (!found)
                            device_missing = true;
                    }
                }

                if (event == 3 && device_missing) {
                    // SAFETY [Critical]: Drop connections immediately ONLY if our active hardware
                    // vanished
                    qjams_drop_connections();
                }
                routing_timeout_id = g_timeout_add(300, delayed_routing_cb, NULL);
            }
        }
    }
    return G_SOURCE_CONTINUE;
}

/**
 * @brief Rebuilds the audio input device preference dropdown and performs autonomous failover
 * routing.
 */
void update_dashboard_pref_ui(void) {
    if (!input_pref_drop)
        return;
    pref_is_updating = true;

    GtkStringList *new_model = gtk_string_list_new(NULL);

    int active_idx = -1;
    int count = 0;

    QJAudioDevice active_devs[QJ_MAX_AUDIO_DEVICES];
    int online_count = scan_audio_devices(client, active_devs, QJ_MAX_AUDIO_DEVICES);

    update_hardware_status_label();

    // 1. Identify key profiles
    const char *primary_dev = NULL;
    const char *fallback_dev = NULL;
    for (int i = 0; i < ui_state.config.num_audio_profiles; i++) {
        if (ui_state.config.audio_profiles[i].is_primary)
            primary_dev = ui_state.config.audio_profiles[i].device_name;
        if (ui_state.config.audio_profiles[i].is_fallback)
            fallback_dev = ui_state.config.audio_profiles[i].device_name;
    }

    // 2. Assess online state
    bool current_is_online = false;
    bool primary_is_online = false;
    bool fallback_is_online = false;

    for (int j = 0; j < online_count; j++) {
        if (strcmp(ui_state.config.audio_device, active_devs[j].display_name) == 0)
            current_is_online = true;
        if (primary_dev && strcmp(primary_dev, active_devs[j].display_name) == 0)
            primary_is_online = true;
        if (fallback_dev && strcmp(fallback_dev, active_devs[j].display_name) == 0)
            fallback_is_online = true;
    }

    if (!current_is_online && client &&
        ((input_port_1 && jack_port_connected(input_port_1) > 0) ||
         (output_port_1 && jack_port_connected(output_port_1) > 0))) {
        current_is_online = true;
    }

    // 3. Autonomous Routing Decisions
    const char *target_device = ui_state.config.audio_device;
    bool did_dropout = false;

    if (primary_is_online && strcmp(target_device, primary_dev) != 0) {
        // Recovered: Cable moved to new port or device powered back on
        target_device = primary_dev;
    } else if (!current_is_online) {
        did_dropout = true;
        if (fallback_is_online) {
            target_device = fallback_dev;
        } else if (online_count > 0) {
            target_device = active_devs[0].display_name;
        } else {
            target_device = ""; // Absolute failure, nothing online
        }
    }

    // 4. Handle Catastrophic Failure (No devices available to switch to)
    if (strlen(target_device) == 0) {
        if (atomic_load_explicit(&get_transport_state()->is_recording, memory_order_acquire) ||
            atomic_load_explicit(&get_transport_state()->is_playing, memory_order_acquire)) {
            on_stop_clicked(NULL, NULL);
        }
        if (lbl_status) {
            log_and_notify(1, "<span foreground='#ff4444'><b>Device cannot be recovered at the "
                              "moment. Advise: Boot your PC and/or device.</b></span>");
        }

        if (pref_model) {
            g_idle_add(deferred_model_unref, pref_model);
        }
        pref_model = new_model;

        gtk_drop_down_set_model(GTK_DROP_DOWN(input_pref_drop), G_LIST_MODEL(new_model));
        pref_is_updating = false;
        return;
    }

    // 5. Handle Valid Switch (Dropout or Recovery)
    if (strcmp(target_device, ui_state.config.audio_device) != 0 ||
        !atomic_load_explicit(&audio_available, memory_order_acquire)) {

        // If the old device dropped offline mid-recording, halt transport safely to protect the
        // take
        if (did_dropout &&
            atomic_load_explicit(&get_transport_state()->is_recording, memory_order_acquire)) {
            on_stop_clicked(NULL, NULL);
        }

        patch_audio_ports(target_device);
        patch_playback_ports(ui_state.config.playback_target);
        strncpy(ui_state.config.audio_device, target_device,
                sizeof(ui_state.config.audio_device) - 1);
        ui_state.config.audio_device[sizeof(ui_state.config.audio_device) - 1] = '\0';

        float target_multiplier = 1.0f;
        for (int i = 0; i < ui_state.config.num_audio_profiles; i++) {
            if (strcmp(ui_state.config.audio_profiles[i].device_name, target_device) == 0) {
                target_multiplier = ui_state.config.audio_profiles[i].gain_multiplier;
                break;
            }
        }

        ui_state.config.input_gain_multiplier = target_multiplier;
        set_input_gain(target_multiplier);

        float loaded_db =
            (target_multiplier <= 0.001f) ? -24.0f : 20.0f * log10f(target_multiplier);
        g_signal_handlers_block_by_func(input_gain_spinner, G_CALLBACK(on_input_gain_changed),
                                        NULL);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(input_gain_spinner), loaded_db);
        g_signal_handlers_unblock_by_func(input_gain_spinner, G_CALLBACK(on_input_gain_changed),
                                          NULL);

        if (current_is_online) {
            // session
            log_and_notify(2, "Auto-Switched to: %s", target_device);
        }

        save_qjams_config(ui_state.config_path, &ui_state.config);
    }

    // 6. Build Dropdown
    for (int i = 0; i < ui_state.config.num_audio_profiles; i++) {
        if (ui_state.config.audio_profiles[i].in_pref) {
            bool is_online = false;
            for (int j = 0; j < online_count; j++) {
                if (strcmp(ui_state.config.audio_profiles[i].device_name,
                           active_devs[j].display_name) == 0) {
                    is_online = true;
                    break;
                }
            }

            if (is_online) {
                gtk_string_list_append(new_model, ui_state.config.audio_profiles[i].device_name);
                if (strcmp(ui_state.config.audio_profiles[i].device_name, target_device) == 0) {
                    active_idx = count;
                }
                count++;
            }
        }
    }

    if (count == 0 || active_idx == -1) {
        gtk_string_list_append(new_model, target_device);
        active_idx = count;
    }

    if (pref_model) {
        g_idle_add(deferred_model_unref, pref_model);
    }
    pref_model = new_model;

    gtk_drop_down_set_model(GTK_DROP_DOWN(input_pref_drop), G_LIST_MODEL(new_model));
    gtk_drop_down_set_selected(GTK_DROP_DOWN(input_pref_drop), active_idx);

    pref_is_updating = false;
}

static void on_pref_selection_changed(GObject *gobject, GParamSpec *pspec, gpointer user_data) {
    (void)pspec;
    (void)user_data;
    if (pref_is_updating)
        return;

    guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(gobject));
    if (selected == GTK_INVALID_LIST_POSITION || !pref_model)
        return;

    const char *new_device = gtk_string_list_get_string(pref_model, selected);
    if (!new_device)
        return;

    patch_audio_ports(new_device);
    strncpy(ui_state.config.audio_device, new_device, sizeof(ui_state.config.audio_device) - 1);

    float target_multiplier = 1.0f;
    for (int i = 0; i < ui_state.config.num_audio_profiles; i++) {
        if (strcmp(ui_state.config.audio_profiles[i].device_name, new_device) == 0) {
            target_multiplier = ui_state.config.audio_profiles[i].gain_multiplier;
            break;
        }
    }

    ui_state.config.input_gain_multiplier = target_multiplier;
    set_input_gain(target_multiplier);

    float loaded_db = (target_multiplier <= 0.001f) ? -24.0f : 20.0f * log10f(target_multiplier);

    g_signal_handlers_block_by_func(input_gain_spinner, G_CALLBACK(on_input_gain_changed), NULL);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(input_gain_spinner), loaded_db);
    g_signal_handlers_unblock_by_func(input_gain_spinner, G_CALLBACK(on_input_gain_changed), NULL);

    save_qjams_config(ui_state.config_path, &ui_state.config);
    update_hardware_status_label();
}

static void bind_meter(GtkWidget *meter, gpointer peak_var_ptr) {
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(meter), on_draw_vu_meter, peak_var_ptr, NULL);
    gtk_widget_add_tick_callback(meter, (GtkTickCallback)on_waveform_tick, NULL, NULL);
}

static void on_input_gain_changed(GtkSpinButton *spin_button, gpointer user_data) {
    (void)user_data;
    double db = gtk_spin_button_get_value(spin_button);
    float multiplier = (db <= -24.0) ? 0.0f : powf(10.0f, (float)(db / 20.0));
    set_input_gain(multiplier);
    ui_state.config.input_gain_multiplier = multiplier;

    int found = 0;
    for (int i = 0; i < ui_state.config.num_audio_profiles; i++) {
        if (g_strcmp0(ui_state.config.audio_profiles[i].device_name,
                      ui_state.config.audio_device) == 0) {
            ui_state.config.audio_profiles[i].gain_multiplier = multiplier;
            found = 1;
            break;
        }
    }
    if (!found && ui_state.config.num_audio_profiles < QJ_MAX_SAVED_DEVICES) {
        strncpy(ui_state.config.audio_profiles[ui_state.config.num_audio_profiles].device_name,
                ui_state.config.audio_device, 127);
        ui_state.config.audio_profiles[ui_state.config.num_audio_profiles].gain_multiplier =
            multiplier;
        ui_state.config.audio_profiles[ui_state.config.num_audio_profiles].is_primary = 0;
        ui_state.config.audio_profiles[ui_state.config.num_audio_profiles].is_fallback = 0;
        ui_state.config.audio_profiles[ui_state.config.num_audio_profiles].in_pref = 0;
        ui_state.config.num_audio_profiles++;
    }

    save_qjams_config(ui_state.config_path, &ui_state.config);
}

static void on_bt_gain_changed(GtkSpinButton *spin_button, gpointer user_data) {
    (void)user_data;
    double db = gtk_spin_button_get_value(spin_button);
    float multiplier = (db <= -24.0) ? 0.0f : powf(10.0f, (float)(db / 20.0));
    set_bt_gain(multiplier);
    ui_state.config.bt_gain_multiplier = multiplier;

    save_qjams_config(ui_state.config_path, &ui_state.config);
}

void refresh_track_time_display(void) {
    if (!lbl_track)
        return;

    size_t frames =
        atomic_load_explicit(&get_transport_state()->backing_track_frames, memory_order_acquire);
    size_t pos = atomic_load_explicit(&get_transport_state()->playback_pos, memory_order_acquire);
    int rate =
        atomic_load_explicit(&get_transport_state()->active_sample_rate, memory_order_acquire);
    if (rate <= 0)
        rate = 48000;

    int cur_secs = pos / rate;

    size_t audible_1x = atomic_load_explicit(&cached_audible_frames, memory_order_acquire);
    float speed =
        atomic_load_explicit(&get_transport_state()->playback_speed, memory_order_relaxed);
    if (speed <= 0.05f)
        speed = 1.0f;
    size_t audible_scaled = (size_t)((double)audible_1x / (double)speed);
    int cached_total_secs =
        (audible_scaled > 0) ? (int)(audible_scaled / rate) : (int)(frames / rate);

    int total_secs = (frames > 0) ? cached_total_secs : 0;
    if (cur_secs > total_secs)
        total_secs = cur_secs; // Prevent time-travel text while actively recording

    char base_name[128] = "None Selected";

    int rec_track = atomic_load_explicit(&current_recording_track, memory_order_acquire);
    get_multitrack_track_name(rec_track, base_name);

    char lbl_buf[256];
    if (frames > 0) {
        snprintf(lbl_buf, sizeof(lbl_buf), "%s [%02d:%02d / %02d:%02d]", base_name, cur_secs / 60,
                 cur_secs % 60, total_secs / 60, total_secs % 60);
    } else {
        int limit = ui_state.config.default_canvas_limit_min > 0
                        ? ui_state.config.default_canvas_limit_min
                        : 5;
        snprintf(lbl_buf, sizeof(lbl_buf), "Blank Loop Canvas [%02d:00 limit]", limit);
    }

    const char *current_text = gtk_label_get_text(GTK_LABEL(lbl_track));
    if (g_strcmp0(current_text, lbl_buf) != 0) {
        gtk_label_set_text(GTK_LABEL(lbl_track), lbl_buf);
    }
}

static GtkWidget *help_window = NULL;

static gboolean on_help_close(GtkWindow *window, gpointer user_data) {
    (void)user_data;
    gtk_widget_set_visible(GTK_WIDGET(window), FALSE);
    return TRUE;
}

static void on_shortcuts_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    if (help_window) {
        gtk_window_present(GTK_WINDOW(help_window));
        return;
    }

    help_window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(help_window), "Keyboard Shortcuts");
    gtk_window_set_default_size(GTK_WINDOW(help_window), 320, 750);
    g_signal_connect(help_window, "close-request", G_CALLBACK(on_help_close), NULL);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(vbox, 15);
    gtk_widget_set_margin_end(vbox, 15);
    gtk_widget_set_margin_top(vbox, 5);
    gtk_widget_set_margin_bottom(vbox, 5);

    GtkWidget *header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_append(GTK_BOX(vbox), header_box);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);

    GtkWidget *lbl_text = gtk_label_new(NULL);
    gtk_label_set_use_markup(GTK_LABEL(lbl_text), TRUE);
    gtk_widget_set_halign(lbl_text, GTK_ALIGN_START);
    gtk_label_set_markup(
        GTK_LABEL(lbl_text),
        "<span foreground='#888888' size='small' weight='bold'>ADDING TRACKS FROM SESSION "
        "MANAGER</span>\n"
        "<b>Double Click</b> : To Add Track to Timeline\n"
        "<b>Drag</b> : To Compose a Track on Timeline\n\n"
        "<span foreground='#888888' size='small' weight='bold'>PLAYBACK &amp; RECORDING</span>\n"
        "<b>Space</b> : Play / Pause\n"
        "<b>R / S</b> : Record / Stop\n"
        "<b>V</b> : Record Video\n"
        "<b>P / N</b> (or <b>PgUp/PgDn</b>) : Prev / Next Track\n"
        "<b>Left / Right</b> : Seek Timeline\n"
        "<b>Home / End</b> : Seek Start / End\n"
        "<b>C</b> : Toggle Speed\n"
        "<b>[ &amp; ]</b> : Reduce &amp; Increase Speed\n"
        "<b>t</b> : Toggle Video View Size\n\n"
        "<span foreground='#888888' size='small' weight='bold'>NAVIGATION &amp; LOOPING</span>\n"
        "<b>Ctrl+Drag</b> : Pan Timeline\n"
        "<b>Scroll</b> : Zoom Timeline\n"
        "<b>Shift+Scroll</b> : Adjust Track Volume\n"
        "<b>Shift+Click</b> : Loop Selection\n"
        "<b>Esc</b> : Clear Selection\n\n"
        "<span foreground='#888888' size='small' weight='bold'>SELECTING TRACKS</span>\n"
        "<b>Click</b> : Select Single Track (Target)\n"
        "<b>Ctrl+Click</b> : Toggle Track in Selection\n"
        "<b>Shift+Click</b> : Select Range of Tracks\n"
        "<b>F2</b> : Rename Focus Track\n\n"
        "<span foreground='#888888' size='small' weight='bold'>EDITING SELECTED TRACKS</span>\n"
        "<b>Ctrl+C</b> : Copy Selection\n"
        "<b>Ctrl+X</b> : Cut Selection (5ms V-Fade)\n"
        "<b>Ctrl+V</b> : Paste (Selected Track)\n"
        "<b>Ctrl+Shift+V</b> : Global Paste (All Tracks)\n"
        "<b>Del</b> : Delete Selection (Clear)\n"
        "<b>Shift+Del</b> : Global Ripple Delete\n"
        "<b>F</b> : Smart V-Fade Selection\n"
        "<b>X</b> : Ripple Crossfade (Blend)\n"
        "<b>Ctrl+Z</b> : Undo Edit\n"
        "<b>Ctrl+Shift+Z / Ctrl+Y</b> : Redo Edit\n"
        "<b>Right-Click Red Dot</b> : Remove Video Take\n");

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), lbl_text);
    gtk_box_append(GTK_BOX(vbox), scroll);
    gtk_window_set_child(GTK_WINDOW(help_window), vbox);

    gtk_window_present(GTK_WINDOW(help_window));
}

static void on_telemetry_clicked(GtkButton *button, gpointer user_data) {
    (void)user_data;

    QJEngineTelemetry stats = {0};
    get_engine_telemetry(&stats);

    size_t v_write = atomic_load_explicit(&video_queue.write_index, memory_order_relaxed);
    size_t v_read = atomic_load_explicit(&video_queue.read_index, memory_order_acquire);
    size_t v_delta = v_write - v_read;

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 15);
    gtk_widget_set_margin_start(grid, 15);
    gtk_widget_set_margin_end(grid, 15);
    gtk_widget_set_margin_top(grid, 15);
    gtk_widget_set_margin_bottom(grid, 15);

    int row = 0;

    GtkWidget *lbl_header1 = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lbl_header1),
                         "<span foreground='#888888' size='small' weight='bold'>AUDIO DSP</span>");
    gtk_widget_set_halign(lbl_header1, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), lbl_header1, 0, row++, 2, 1);

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Hardware Rate:"), 0, row, 1, 1);
    char sr_buf[32];
    snprintf(sr_buf, sizeof(sr_buf), "<b>%d Hz</b>", stats.sample_rate);
    GtkWidget *lbl_sr = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lbl_sr), sr_buf);
    gtk_widget_set_halign(lbl_sr, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), lbl_sr, 1, row++, 1, 1);

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("RT Dropouts:"), 0, row, 1, 1);
    char drop_buf[32];
    snprintf(drop_buf, sizeof(drop_buf), "<b>%u</b>", stats.dropped_frames);
    GtkWidget *lbl_drop = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lbl_drop), drop_buf);
    gtk_widget_set_halign(lbl_drop, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), lbl_drop, 1, row++, 1, 1);

    GtkWidget *lbl_header2 = gtk_label_new(NULL);
    gtk_label_set_markup(
        GTK_LABEL(lbl_header2),
        "\n<span foreground='#888888' size='small' weight='bold'>MEMORY &amp; VIDEO</span>");
    gtk_widget_set_halign(lbl_header2, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), lbl_header2, 0, row++, 2, 1);

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Undo Ledger:"), 0, row, 1, 1);
    char ledger_buf[64];
    snprintf(ledger_buf, sizeof(ledger_buf), "<b>%d undos, %d redos</b> (Max: %d)",
             stats.ledger_undos, stats.ledger_redos, stats.ledger_capacity);
    GtkWidget *lbl_ledger = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lbl_ledger), ledger_buf);
    gtk_widget_set_halign(lbl_ledger, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), lbl_ledger, 1, row++, 1, 1);

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("System RAM:"), 0, row, 1, 1);
    char total_buf[32];
    snprintf(total_buf, sizeof(total_buf), "<b>%.1f GB</b>",
             (double)stats.total_ram_bytes / (1024.0 * 1024.0 * 1024.0));
    GtkWidget *lbl_total = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lbl_total), total_buf);
    gtk_widget_set_halign(lbl_total, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), lbl_total, 1, row++, 1, 1);

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("QJams Allocated:"), 0, row, 1, 1);
    char qj_buf[32];
    snprintf(qj_buf, sizeof(qj_buf), "<b>%.1f GB</b>",
             (double)stats.qjams_rss_bytes / (1024.0 * 1024.0 * 1024.0));
    GtkWidget *lbl_qj = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lbl_qj), qj_buf);
    gtk_widget_set_halign(lbl_qj, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), lbl_qj, 1, row++, 1, 1);

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Available RAM:"), 0, row, 1, 1);
    char avail_buf[32];
    snprintf(avail_buf, sizeof(avail_buf), "<b>%.1f GB</b>",
             (double)stats.available_ram_bytes / (1024.0 * 1024.0 * 1024.0));
    GtkWidget *lbl_avail = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lbl_avail), avail_buf);
    gtk_widget_set_halign(lbl_avail, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), lbl_avail, 1, row++, 1, 1);

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Video Queue Delta:"), 0, row, 1, 1);
    char vq_buf[32];
    snprintf(vq_buf, sizeof(vq_buf), "<b>%zu frames</b>", v_delta);
    GtkWidget *lbl_vq = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lbl_vq), vq_buf);
    gtk_widget_set_halign(lbl_vq, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), lbl_vq, 1, row++, 1, 1);

    telemetry_popover = gtk_popover_new();
    gtk_popover_set_child(GTK_POPOVER(telemetry_popover), grid);
    gtk_widget_set_parent(telemetry_popover, GTK_WIDGET(button));

    // Safety [Moderate]: Destroys the popover entirely when closed to prevent Wayland surface
    // memory leaks
    g_signal_connect(telemetry_popover, "closed", G_CALLBACK(gtk_widget_unparent), NULL);

    gtk_popover_popup(GTK_POPOVER(telemetry_popover));
}

static void on_pref_drop_setup(GtkSignalListItemFactory *factory, GtkListItem *list_item,
                               gpointer user_data) {
    (void)factory;
    (void)user_data;
    GtkWidget *label = gtk_label_new(NULL);

    // Force strict dimensions and truncation on the dropdown label
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_label_set_width_chars(GTK_LABEL(label), 31);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 31);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);

    gtk_list_item_set_child(list_item, label);
}

static void on_pref_drop_bind(GtkSignalListItemFactory *factory, GtkListItem *list_item,
                              gpointer user_data) {
    (void)factory;
    (void)user_data;
    GtkWidget *label = gtk_list_item_get_child(list_item);
    gpointer item = gtk_list_item_get_item(list_item);

    if (item && GTK_IS_STRING_OBJECT(item)) {
        gtk_label_set_text(GTK_LABEL(label), gtk_string_object_get_string(GTK_STRING_OBJECT(item)));
    }
}

/**
 * @brief Toggles the video preview frame between the configured Default and Expanded/Toggled
 * dimensions.
 */
void on_video_size_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    bool current = atomic_load_explicit(&is_video_expanded, memory_order_acquire);
    bool new_state = !current;
    atomic_store_explicit(&is_video_expanded, new_state, memory_order_release);

    if (ui_state.config.video_width_default == ui_state.config.video_width_expanded) {
        return;
    }

    int target_w =
        new_state ? ui_state.config.video_width_expanded : ui_state.config.video_width_default;
    int target_h = (int)(target_w * (9.0 / 16.0));

    if (video_preview_frame) {
        gtk_widget_set_size_request(video_preview_frame, target_w, target_h);

        GtkWidget *picture = gtk_widget_get_first_child(video_preview_frame);
        if (picture) {
            gtk_widget_set_size_request(picture, target_w, target_h);
            gtk_widget_queue_resize(picture);
            gtk_widget_queue_draw(picture);
        }

        gtk_widget_queue_resize(video_preview_frame);

        GtkRoot *root = gtk_widget_get_root(video_preview_frame);
        if (root)
            gtk_widget_queue_draw(GTK_WIDGET(root));
    }
}

void ui_dashboard_init(GtkBuilder *b_dash, GtkBuilder *b_wave) {
    // SAFETY [Critical]: Initialize status widgets first so the hardware routing logic can safely
    // emit toasts
    lbl_status = GTK_WIDGET(gtk_builder_get_object(b_dash, "lbl_status"));
    lbl_hw_toast = GTK_WIDGET(gtk_builder_get_object(b_dash, "lbl_hw_toast"));
    main_spinner = GTK_WIDGET(gtk_builder_get_object(b_dash, "main_spinner"));

    lbl_input_device = GTK_WIDGET(gtk_builder_get_object(b_dash, "lbl_input_device"));

    input_gain_spinner = GTK_WIDGET(gtk_builder_get_object(b_dash, "input_gain_spinner"));
    float loaded_db = (ui_state.config.input_gain_multiplier <= 0.001f)
                          ? -24.0f
                          : 20.0f * log10f(ui_state.config.input_gain_multiplier);
    GtkAdjustment *input_adj = gtk_adjustment_new(loaded_db, -24.0, 24.0, 1.0, 5.0, 0.0);
    gtk_spin_button_set_adjustment(GTK_SPIN_BUTTON(input_gain_spinner), input_adj);
    gtk_spin_button_set_climb_rate(GTK_SPIN_BUTTON(input_gain_spinner), 1.0);
    g_signal_connect(input_gain_spinner, "value-changed", G_CALLBACK(on_input_gain_changed), NULL);

    GtkWidget *bt_gain_spinner = GTK_WIDGET(gtk_builder_get_object(b_dash, "bt_gain_spinner"));
    float loaded_bt_db = (ui_state.config.bt_gain_multiplier <= 0.001f)
                             ? -24.0f
                             : 20.0f * log10f(ui_state.config.bt_gain_multiplier);
    GtkAdjustment *bt_adj = gtk_adjustment_new(loaded_bt_db, -24.0, 24.0, 1.0, 5.0, 0.0);
    gtk_spin_button_set_adjustment(GTK_SPIN_BUTTON(bt_gain_spinner), bt_adj);
    gtk_spin_button_set_climb_rate(GTK_SPIN_BUTTON(bt_gain_spinner), 1.0);
    g_signal_connect(bt_gain_spinner, "value-changed", G_CALLBACK(on_bt_gain_changed), NULL);

    GtkWidget *vu_input_l = GTK_WIDGET(gtk_builder_get_object(b_dash, "vu_input_l"));
    GtkWidget *vu_input_r = GTK_WIDGET(gtk_builder_get_object(b_dash, "vu_input_r"));
    GtkWidget *vu_bt_l = GTK_WIDGET(gtk_builder_get_object(b_dash, "vu_bt_l"));
    GtkWidget *vu_bt_r = GTK_WIDGET(gtk_builder_get_object(b_dash, "vu_bt_r"));
    bind_meter(vu_input_l, &vu_peak_input_l);
    bind_meter(vu_input_r, &vu_peak_input_r);
    bind_meter(vu_bt_l, &vu_peak_bt_l);
    bind_meter(vu_bt_r, &vu_peak_bt_r);

    lbl_track = GTK_WIDGET(gtk_builder_get_object(b_dash, "lbl_track"));
    gtk_label_set_ellipsize(GTK_LABEL(lbl_track), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(lbl_track, TRUE);
    gtk_widget_set_halign(lbl_track, GTK_ALIGN_START);

    btn_play = GTK_WIDGET(gtk_builder_get_object(b_dash, "btn_play"));
    g_signal_connect(btn_play, "clicked", G_CALLBACK(on_play_clicked), NULL);

    btn_record = GTK_WIDGET(gtk_builder_get_object(b_dash, "btn_record"));
    g_signal_connect(btn_record, "clicked", G_CALLBACK(on_record_audio_clicked), NULL);

    btn_record_video = GTK_WIDGET(gtk_builder_get_object(b_dash, "btn_record_video"));
    g_signal_connect(btn_record_video, "clicked", G_CALLBACK(on_record_video_clicked), NULL);

    btn_stop = GTK_WIDGET(gtk_builder_get_object(b_dash, "btn_stop"));
    g_signal_connect(btn_stop, "clicked", G_CALLBACK(on_stop_clicked), NULL);
    gtk_widget_set_sensitive(btn_stop, FALSE);

    btn_video_size = GTK_WIDGET(gtk_builder_get_object(b_dash, "btn_video_size"));
    if (btn_video_size) {
        gtk_widget_set_tooltip_text(btn_video_size, "Toggle Video Size");
        g_signal_connect(btn_video_size, "clicked", G_CALLBACK(on_video_size_clicked), NULL);
    }

    btn_reset = GTK_WIDGET(gtk_builder_get_object(b_dash, "btn_reset"));
    g_signal_connect(btn_reset, "clicked", G_CALLBACK(on_global_reset_clicked), NULL);

    btn_prev = GTK_WIDGET(gtk_builder_get_object(b_dash, "btn_prev"));
    g_signal_connect(btn_prev, "clicked", G_CALLBACK(on_prev_track_clicked), NULL);
    btn_next = GTK_WIDGET(gtk_builder_get_object(b_dash, "btn_next"));
    g_signal_connect(btn_next, "clicked", G_CALLBACK(on_next_track_clicked), NULL);

    btn_loop_countdown = GTK_WIDGET(gtk_builder_get_object(b_dash, "btn_loop_countdown"));
    if (btn_loop_countdown) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(btn_loop_countdown),
                                     ui_state.config.loop_countdown_active != 0);
        g_signal_connect(btn_loop_countdown, "toggled", G_CALLBACK(on_loop_countdown_toggled),
                         NULL);
    }

    btn_overlay_clip = GTK_WIDGET(gtk_builder_get_object(b_dash, "btn_overlay_clip"));
    if (btn_overlay_clip) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(btn_overlay_clip),
                                     ui_state.config.overlay_soft_clip != 0);
        g_signal_connect(btn_overlay_clip, "toggled", G_CALLBACK(on_overlay_clip_toggled), NULL);
    }

    btn_speed = GTK_WIDGET(gtk_builder_get_object(b_dash, "btn_speed"));
    if (btn_speed) {
        gtk_widget_set_tooltip_text(btn_speed, "Reset Speed (C)\n[ : Slow down\n] : Speed up");
        g_signal_connect(btn_speed, "clicked", G_CALLBACK(on_speed_clicked), NULL);
    }

    btn_zoom = GTK_WIDGET(gtk_builder_get_object(b_dash, "btn_zoom"));
    gtk_widget_set_tooltip_text(btn_zoom, "Crop Canvas to Active Length");
    g_signal_connect(btn_zoom, "clicked", G_CALLBACK(on_crop_clicked), NULL);

    btn_playlist_toggle = GTK_WIDGET(gtk_builder_get_object(b_dash, "btn_playlist_toggle"));
    gtk_widget_set_tooltip_text(btn_playlist_toggle, "Add Track/Session to Playlist");
    g_signal_connect(btn_playlist_toggle, "clicked", G_CALLBACK(on_playlist_toggle_clicked), NULL);

    GtkWidget *btn_shortcuts = GTK_WIDGET(gtk_builder_get_object(b_dash, "btn_shortcuts"));
    if (btn_shortcuts) {
        gtk_widget_set_tooltip_text(btn_shortcuts, "Keyboard Shortcuts");
        g_signal_connect(btn_shortcuts, "clicked", G_CALLBACK(on_shortcuts_clicked), NULL);
    }

    input_pref_drop = GTK_WIDGET(gtk_builder_get_object(b_dash, "input_pref_drop"));
    if (input_pref_drop) {
        GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
        g_signal_connect(factory, "setup", G_CALLBACK(on_pref_drop_setup), NULL);
        g_signal_connect(factory, "bind", G_CALLBACK(on_pref_drop_bind), NULL);
        gtk_drop_down_set_factory(GTK_DROP_DOWN(input_pref_drop), factory);
        g_object_unref(factory);

        g_signal_connect(input_pref_drop, "notify::selected", G_CALLBACK(on_pref_selection_changed),
                         NULL);
        update_dashboard_pref_ui();
    }

    GtkWidget *btn_telemetry = GTK_WIDGET(gtk_builder_get_object(b_dash, "btn_telemetry"));
    if (btn_telemetry) {
        gtk_widget_set_tooltip_text(btn_telemetry, "Engine Telemetry & Stats");
        g_signal_connect(btn_telemetry, "clicked", G_CALLBACK(on_telemetry_clicked), NULL);
    }

    waveform_area_bt = GTK_WIDGET(gtk_builder_get_object(b_wave, "waveform_area_bt"));
    GtkEventController *scroll_ctrl_bt =
        gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
    g_signal_connect(scroll_ctrl_bt, "scroll", G_CALLBACK(on_waveform_scroll), NULL);
    gtk_widget_add_controller(waveform_area_bt, scroll_ctrl_bt);

    // --- Hover Tracking (Standard Mouse Movement) ---
    GtkEventController *motion_ctrl_bt = gtk_event_controller_motion_new();
    g_signal_connect(motion_ctrl_bt, "motion", G_CALLBACK(on_canvas_hover_motion), NULL);
    g_signal_connect(motion_ctrl_bt, "enter", G_CALLBACK(on_canvas_hover_enter), NULL);
    g_signal_connect(motion_ctrl_bt, "leave", G_CALLBACK(on_canvas_hover_leave), NULL);
    gtk_widget_add_controller(waveform_area_bt, motion_ctrl_bt);

    // --- Hover Tracking (Drag-and-Drop Override) ---
    GtkEventController *drop_motion_ctrl_bt = gtk_drop_controller_motion_new();
    g_signal_connect(drop_motion_ctrl_bt, "motion", G_CALLBACK(on_canvas_hover_motion), NULL);
    g_signal_connect(drop_motion_ctrl_bt, "enter", G_CALLBACK(on_canvas_hover_enter), NULL);
    g_signal_connect(drop_motion_ctrl_bt, "leave", G_CALLBACK(on_canvas_hover_leave), NULL);
    gtk_widget_add_controller(waveform_area_bt, drop_motion_ctrl_bt);

    waveform_area_input = GTK_WIDGET(gtk_builder_get_object(b_wave, "waveform_area_input"));
    GtkEventController *scroll_ctrl_in =
        gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
    g_signal_connect(scroll_ctrl_in, "scroll", G_CALLBACK(on_waveform_scroll), NULL);
    gtk_widget_add_controller(waveform_area_input, scroll_ctrl_in);
}
