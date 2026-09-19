#include "ui_settings.h"
#include "ui_globals.h"
#include "audio_engine.h"
#include "video_engine.h"
#include "scanner.h"
#include "ui_dashboard.h"
#include <string.h>
#include <stdio.h>

extern jack_client_t *client;
extern SPSC_Video_Queue video_queue;

typedef struct {
    AudioDevice dev;
    GtkWidget *rb_primary;
    GtkWidget *rb_fallback;
    GtkWidget *cb_pref;
} AudioRowData;

typedef struct {
    GtkWidget *win;
    GtkWidget *v_drop;
    GtkWidget *p_drop;
    GtkWidget *f_drop;
    GtkWidget *spin_canvas_limit;
    GtkWidget *cb_logging;
    AudioRowData audio_rows[MAX_AUDIO_DEVICES];
    int num_audio_rows;
    int a_count_online;
    VideoDevice v_devs[MAX_CAMERAS];
    AudioDevice a_devs[MAX_AUDIO_DEVICES];
    AudioDevice p_devs[MAX_AUDIO_DEVICES];
} SettingsData;

static void apply_hardware_changes(QJamsConfig *new_config) {
    log_and_notify(0, "State Manager: Re-patching Audio Capture to %s\n", new_config->audio_device);
    patch_audio_ports(new_config->audio_device);

    float target_multiplier = 1.0f;
    for (int i = 0; i < ui_state.config.num_audio_profiles; i++) {
        if (strcmp(ui_state.config.audio_profiles[i].device_name, new_config->audio_device) == 0) {
            target_multiplier = ui_state.config.audio_profiles[i].gain_multiplier;
            break;
        }
    }
    new_config->input_gain_multiplier = target_multiplier;
    set_input_gain(target_multiplier);

    float loaded_db = (target_multiplier <= 0.001f) ? -24.0f : 20.0f * log10f(target_multiplier);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(input_gain_spinner), loaded_db);

    log_and_notify(0, "State Manager: Re-patching Audio Playback to %s\n", new_config->playback_target);
    patch_playback_ports(new_config->playback_target);

    if (strcmp(ui_state.config.video_device, new_config->video_device) != 0) {
        log_and_notify(0, "State Manager: Video Capture device changed.\n");
        stop_video_engine();
        if (init_and_start_video_engine(new_config->video_device, &video_queue) != 0) {
            log_and_notify(0, "State Manager: Failed to start new video device.\n");
        }
    }

    ui_state.config = *new_config;
    save_qjams_config(ui_state.config_path, &ui_state.config);
}

static void on_settings_save(GtkButton *btn, gpointer user_data) {
    (void)btn;
    SettingsData *sd = (SettingsData*)user_data;

    guint v_idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(sd->v_drop));
    guint p_idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(sd->p_drop));
    guint f_idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(sd->f_drop));

    QJamsConfig new_config = ui_state.config;

    if (f_idx != GTK_INVALID_LIST_POSITION) {
        new_config.export_format = f_idx;
    }

    new_config.enable_logging = gtk_check_button_get_active(GTK_CHECK_BUTTON(sd->cb_logging)) ? 1 : 0;

    if (v_idx != GTK_INVALID_LIST_POSITION) {
        strncpy(new_config.video_device, sd->v_devs[v_idx].device_path, sizeof(new_config.video_device) - 1);
    }
    if (p_idx != GTK_INVALID_LIST_POSITION) {
        strncpy(new_config.playback_target, sd->p_devs[p_idx].display_name, sizeof(new_config.playback_target) - 1);
    }

    char target_device[128] = "";
    int pri_online = 0, fall_online = 0;
    char pri_name[128] = "", fall_name[128] = "";

    for (int i = 0; i < sd->num_audio_rows; i++) {
        if (!gtk_widget_get_visible(sd->audio_rows[i].rb_primary)) continue;

        bool is_pri = gtk_check_button_get_active(GTK_CHECK_BUTTON(sd->audio_rows[i].rb_primary));
        bool is_fall = gtk_check_button_get_active(GTK_CHECK_BUTTON(sd->audio_rows[i].rb_fallback));
        bool in_pref = gtk_check_button_get_active(GTK_CHECK_BUTTON(sd->audio_rows[i].cb_pref));

        int p_idx_audio = -1;
        for (int p = 0; p < new_config.num_audio_profiles; p++) {
            if (strcmp(new_config.audio_profiles[p].device_name, sd->audio_rows[i].dev.display_name) == 0) {
                p_idx_audio = p;
                break;
            }
        }
        if (p_idx_audio == -1 && new_config.num_audio_profiles < MAX_SAVED_DEVICES) {
            p_idx_audio = new_config.num_audio_profiles++;
            strncpy(new_config.audio_profiles[p_idx_audio].device_name, sd->audio_rows[i].dev.display_name, 127);
            new_config.audio_profiles[p_idx_audio].gain_multiplier = 1.0f;
        }

        if (p_idx_audio != -1) {
            new_config.audio_profiles[p_idx_audio].is_primary = is_pri ? 1 : 0;
            new_config.audio_profiles[p_idx_audio].is_fallback = is_fall ? 1 : 0;
            new_config.audio_profiles[p_idx_audio].in_pref = in_pref ? 1 : 0;
        }

        if (is_pri) {
            strncpy(pri_name, sd->audio_rows[i].dev.display_name, 127);
            if (sd->audio_rows[i].dev.channel_count > 0) pri_online = 1;
        }
        if (is_fall) {
            strncpy(fall_name, sd->audio_rows[i].dev.display_name, 127);
            if (sd->audio_rows[i].dev.channel_count > 0) fall_online = 1;
        }
    }

    if (pri_online) strncpy(target_device, pri_name, 127);
    else if (fall_online) strncpy(target_device, fall_name, 127);
    else if (sd->a_count_online > 0) strncpy(target_device, sd->a_devs[0].display_name, 127);

    if (strlen(target_device) > 0) {
        strncpy(new_config.audio_device, target_device, 127);
    }

    new_config.default_canvas_limit_min = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(sd->spin_canvas_limit));
    bool limits_changed = (ui_state.config.default_canvas_limit_min != new_config.default_canvas_limit_min);

    apply_hardware_changes(&new_config);

    if (limits_changed) {
        extern int resize_loop_canvas_seconds(int);
        extern void update_zoom_button_label_to_length(void);
        extern void invalidate_waveform_caches(void);
        extern GtkWidget *waveform_area_bt;
        extern GtkWidget *waveform_area_input;

        int new_limit = new_config.default_canvas_limit_min;
        int resize_result = resize_loop_canvas_seconds(new_limit * 60);

        if (resize_result == 0) {
            update_zoom_button_label_to_length();
            invalidate_waveform_caches();
            if (waveform_area_bt) gtk_widget_queue_draw(waveform_area_bt);
            if (waveform_area_input) gtk_widget_queue_draw(waveform_area_input);

            if (strlen(ui_state.selected_track_path) == 0) {
                char track_lbl[128];
                snprintf(track_lbl, sizeof(track_lbl), "Track: Blank Loop Canvas [%02d:00 limit]", new_limit);
                extern GtkWidget *lbl_track;
                gtk_label_set_text(GTK_LABEL(lbl_track), track_lbl);
            }
        } else if (resize_result == -2) {
            log_and_notify(1, "<span foreground='#ff4444'><b>Status: Insufficient RAM to apply new duration limits!</b></span>");
            ui_state.config.default_canvas_limit_min = ui_state.config.default_canvas_limit_min;
            save_qjams_config(ui_state.config_path, &ui_state.config);
        }
    }

    extern void update_dashboard_pref_ui(void);
    update_dashboard_pref_ui();

    GtkWidget *win = sd->win;
    gtk_window_destroy(GTK_WINDOW(win));

    log_and_notify(0, "Hardware Matrix and Workflow settings applied dynamically.\n");
}

static void on_recordings_dir_chosen(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source_object);
    GtkWidget *lbl_recordings_dir = GTK_WIDGET(user_data);
    GError *error = NULL;
    GFile *folder = gtk_file_dialog_select_folder_finish(dialog, res, &error);

    if (folder) {
        char *path = g_file_get_path(folder);
        strncpy(ui_state.config.recordings_dir, path, sizeof(ui_state.config.recordings_dir) - 1);
        // FIX: Guarantee null-termination for max-length strings
        ui_state.config.recordings_dir[sizeof(ui_state.config.recordings_dir) - 1] = '\0';

        save_qjams_config(ui_state.config_path, &ui_state.config);
        gtk_label_set_text(GTK_LABEL(lbl_recordings_dir), path);
        g_free(path);
        g_object_unref(folder);
    }
}

static void on_select_recordings_dir(GtkButton *btn, gpointer user_data) {
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Select Recordings Folder");
    if (strlen(ui_state.config.recordings_dir) > 0) {
        GFile *initial = g_file_new_for_path(ui_state.config.recordings_dir);
        gtk_file_dialog_set_initial_folder(dialog, initial);
        g_object_unref(initial);
    }
    gtk_file_dialog_select_folder(dialog, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(btn))), NULL, on_recordings_dir_chosen, user_data);
}

typedef struct {
    char device_name[128];
    GtkWidget *w_lbl;
    GtkWidget *w_pri;
    GtkWidget *w_fall;
    GtkWidget *w_pref;
    GtkWidget *w_del;
} AudioRowDeleteData;

// Memory Safety (Low Risk): Provides strict signature matching for GClosureNotify to prevent ABI mismatch during g_free.
static void free_audio_row_delete_data(gpointer data, GClosure *closure) {
    (void)closure;
    g_free(data);
}

static gboolean execute_delete_profile_idle(gpointer user_data) {
    AudioRowDeleteData *data = (AudioRowDeleteData *)user_data;

    int found_idx = -1;
    for (int i = 0; i < ui_state.config.num_audio_profiles; i++) {
        // Memory Safety (Low Risk): Use strcspn/strncmp to guarantee matches even if config struct retains trailing \r\n
        size_t cfg_len = strcspn(ui_state.config.audio_profiles[i].device_name, "\r\n");
        size_t dat_len = strlen(data->device_name);
        if (cfg_len == dat_len && strncmp(ui_state.config.audio_profiles[i].device_name, data->device_name, dat_len) == 0) {
            found_idx = i;
            break;
        }
    }

    if (found_idx >= 0) {
        for (int i = found_idx; i < ui_state.config.num_audio_profiles - 1; i++) {
            ui_state.config.audio_profiles[i] = ui_state.config.audio_profiles[i + 1];
        }
        ui_state.config.num_audio_profiles--;
        save_qjams_config(ui_state.config_path, &ui_state.config);

        // Safe to hide widgets now that the gesture event loop has released its grab
        gtk_widget_set_visible(data->w_lbl, FALSE);
        gtk_widget_set_visible(data->w_pri, FALSE);
        gtk_widget_set_visible(data->w_fall, FALSE);
        gtk_widget_set_visible(data->w_pref, FALSE);
        gtk_widget_set_visible(data->w_del, FALSE);

        log_and_notify(0, "[CONFIG] Offline audio profile deleted: %s\n", data->device_name);
    }
    return G_SOURCE_REMOVE;
}

static void on_delete_profile_clicked(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data) {
    (void)gesture; (void)n_press; (void)x; (void)y;
    // Memory Safety (Critical): Defer UI unmapping to prevent Wayland/GTK event grab segfaults
    g_idle_add(execute_delete_profile_idle, user_data);
}

void on_settings_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    GtkWidget *settings_win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(settings_win), "Hardware & Workflow Preferences");
    gtk_window_set_transient_for(GTK_WINDOW(settings_win), GTK_WINDOW(user_data));
    gtk_window_set_modal(GTK_WINDOW(settings_win), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(settings_win), 640, -1);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 15);
    gtk_widget_set_margin_start(vbox, 20);
    gtk_widget_set_margin_end(vbox, 20);
    gtk_widget_set_margin_top(vbox, 20);
    gtk_widget_set_margin_bottom(vbox, 20);
    gtk_window_set_child(GTK_WINDOW(settings_win), vbox);

    SettingsData *sd = g_new0(SettingsData, 1);
    sd->win = settings_win;

    int v_count = scan_video_devices(sd->v_devs, MAX_CAMERAS);
    int a_count = scan_audio_devices(client, sd->a_devs, MAX_AUDIO_DEVICES);
    int p_count = scan_playback_devices(client, sd->p_devs, MAX_AUDIO_DEVICES);

    gtk_box_append(GTK_BOX(vbox), gtk_label_new("Camera Source:"));
    GtkStringList *v_list = gtk_string_list_new(NULL);
    int v_active = 0;
    for (int i = 0; i < v_count; i++) {
        char label[256];
        snprintf(label, sizeof(label), "%s (%s)", sd->v_devs[i].device_name, sd->v_devs[i].device_path);
        gtk_string_list_append(v_list, label);
        if (strcmp(sd->v_devs[i].device_path, ui_state.config.video_device) == 0) v_active = i;
    }
    GtkWidget *v_drop = gtk_drop_down_new(G_LIST_MODEL(v_list), NULL);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(v_drop), v_active);
    gtk_box_append(GTK_BOX(vbox), v_drop);

    gtk_box_append(GTK_BOX(vbox), gtk_label_new("Audio Capture Matrix:"));
    GtkWidget *audio_frame = gtk_frame_new(NULL);
    GtkWidget *audio_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(audio_scroll), 245);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(audio_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

    GtkWidget *audio_grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(audio_grid), 25);
    gtk_grid_set_row_spacing(GTK_GRID(audio_grid), 8);
    gtk_widget_set_margin_start(audio_grid, 10);
    gtk_widget_set_margin_end(audio_grid, 10);
    gtk_widget_set_margin_top(audio_grid, 10);
    gtk_widget_set_margin_bottom(audio_grid, 10);

    GtkWidget *h_dev = gtk_label_new(NULL); gtk_label_set_markup(GTK_LABEL(h_dev), "<b>Device</b>");
    GtkWidget *h_pri = gtk_label_new(NULL); gtk_label_set_markup(GTK_LABEL(h_pri), "<b>Primary</b>");
    GtkWidget *h_fall = gtk_label_new(NULL); gtk_label_set_markup(GTK_LABEL(h_fall), "<b>Fallback</b>");
    GtkWidget *h_pref = gtk_label_new(NULL); gtk_label_set_markup(GTK_LABEL(h_pref), "<b>Selector</b>");
    GtkWidget *h_del = gtk_label_new(NULL); gtk_label_set_markup(GTK_LABEL(h_del), "<b>Del </b>");

    gtk_widget_set_halign(h_dev, GTK_ALIGN_START);
    gtk_widget_set_hexpand(h_dev, TRUE);

    gtk_grid_attach(GTK_GRID(audio_grid), h_dev, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(audio_grid), h_pri, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(audio_grid), h_fall, 2, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(audio_grid), h_pref, 3, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(audio_grid), h_del, 4, 0, 1, 1);

    int row_count = 0;
    for (int p = 0; p < ui_state.config.num_audio_profiles && row_count < MAX_AUDIO_DEVICES; p++) {
        // Memory Safety (Low Risk): strncpy bound to 127 prevents overflow.
        strncpy(sd->audio_rows[row_count].dev.display_name, ui_state.config.audio_profiles[p].device_name, 127);

        // Strip accidental newlines from config file reads that cause massive grid row heights
        sd->audio_rows[row_count].dev.display_name[strcspn(sd->audio_rows[row_count].dev.display_name, "\r\n")] = '\0';

        sd->audio_rows[row_count].dev.channel_count = 0;
        row_count++;
    }

    for (int i = 0; i < a_count && row_count < MAX_AUDIO_DEVICES; i++) {
        int found = 0;
        for (int j = 0; j < row_count; j++) {
            if (strcmp(sd->audio_rows[j].dev.display_name, sd->a_devs[i].display_name) == 0) {
                sd->audio_rows[j].dev.channel_count = sd->a_devs[i].channel_count;
                found = 1;
                break;
            }
        }
        if (!found) {
            strncpy(sd->audio_rows[row_count].dev.display_name, sd->a_devs[i].display_name, 127);
            sd->audio_rows[row_count].dev.channel_count = sd->a_devs[i].channel_count;
            row_count++;
        }
    }

    sd->num_audio_rows = row_count;
    sd->a_count_online = a_count;

    // --- SORTING PIPELINE ---
    int scores[MAX_AUDIO_DEVICES] = {0};
    for (int i = 0; i < row_count; i++) {
        scores[i] = 5; // Default to unselected baseline

        for (int p = 0; p < ui_state.config.num_audio_profiles; p++) {
            if (strcmp(ui_state.config.audio_profiles[p].device_name, sd->audio_rows[i].dev.display_name) == 0) {
                scores[i] = 4; // Device is kept in preferences by virtue of existing in the config array
                if (ui_state.config.audio_profiles[p].is_primary) scores[i] = 1;
                else if (ui_state.config.audio_profiles[p].is_fallback) scores[i] = 2;
                else if (ui_state.config.audio_profiles[p].in_pref) scores[i] = 3;
                break;
            }
        }
    }

    // Memory Safety (Low Risk): O(N^2) selection sort strictly bounded by MAX_AUDIO_DEVICES.
    // Byte-level memcpy swap guarantees ABI structural integrity without requiring a localized typedef cast.
    for (int i = 0; i < row_count - 1; i++) {
        for (int j = i + 1; j < row_count; j++) {
            if (scores[j] < scores[i]) {
                int temp_score = scores[i];
                scores[i] = scores[j];
                scores[j] = temp_score;

                char temp_row[sizeof(sd->audio_rows[0])];
                memcpy(temp_row, &sd->audio_rows[i], sizeof(sd->audio_rows[0]));
                memcpy(&sd->audio_rows[i], &sd->audio_rows[j], sizeof(sd->audio_rows[0]));
                memcpy(&sd->audio_rows[j], temp_row, sizeof(sd->audio_rows[0]));
            }
        }
    }

    GtkWidget *pri_group = NULL;
    GtkWidget *fall_group = NULL;

    for (int i = 0; i < row_count; i++) {
        char label[256];
        if (sd->audio_rows[i].dev.channel_count > 0) {
            snprintf(label, sizeof(label), "%s [%d ch]", sd->audio_rows[i].dev.display_name, sd->audio_rows[i].dev.channel_count);
        } else {
            snprintf(label, sizeof(label), "%s [Offline]", sd->audio_rows[i].dev.display_name);
        }

        GtkWidget *lbl = gtk_label_new(label);
        gtk_widget_set_halign(lbl, GTK_ALIGN_START);
        gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
        gtk_widget_set_hexpand(lbl, TRUE);
        gtk_grid_attach(GTK_GRID(audio_grid), lbl, 0, i + 1, 1, 1);

        GtkWidget *rb_pri = gtk_check_button_new();
        if (pri_group) gtk_check_button_set_group(GTK_CHECK_BUTTON(rb_pri), GTK_CHECK_BUTTON(pri_group));
        else pri_group = rb_pri;

        GtkWidget *rb_fall = gtk_check_button_new();
        if (fall_group) gtk_check_button_set_group(GTK_CHECK_BUTTON(rb_fall), GTK_CHECK_BUTTON(fall_group));
        else fall_group = rb_fall;

        gtk_widget_set_halign(rb_pri, GTK_ALIGN_CENTER);
        gtk_widget_set_halign(rb_fall, GTK_ALIGN_CENTER);
        gtk_grid_attach(GTK_GRID(audio_grid), rb_pri, 1, i + 1, 1, 1);
        gtk_grid_attach(GTK_GRID(audio_grid), rb_fall, 2, i + 1, 1, 1);

        GtkWidget *cb_pref = gtk_check_button_new();
        gtk_widget_set_halign(cb_pref, GTK_ALIGN_CENTER);
        gtk_grid_attach(GTK_GRID(audio_grid), cb_pref, 3, i + 1, 1, 1);

        sd->audio_rows[i].rb_primary = rb_pri;
        sd->audio_rows[i].rb_fallback = rb_fall;
        sd->audio_rows[i].cb_pref = cb_pref;

        GtkWidget *btn_del = gtk_label_new("x");
        gtk_widget_set_halign(btn_del, GTK_ALIGN_CENTER);

        // Clamp vertical layout to bypass GTK4's default interactive touch-target padding
        gtk_widget_set_valign(btn_del, GTK_ALIGN_CENTER);
        gtk_widget_set_vexpand(btn_del, FALSE);

        gtk_widget_set_cursor_from_name(btn_del, "pointer");

        GtkGesture *click = gtk_gesture_click_new();
        gtk_widget_add_controller(btn_del, GTK_EVENT_CONTROLLER(click));

        bool is_online = (sd->audio_rows[i].dev.channel_count > 0);
        gtk_widget_set_sensitive(btn_del, !is_online);
        gtk_widget_set_tooltip_text(btn_del, is_online ? "Active devices cannot be deleted" : "Delete offline profile");

        // Memory Safety (Low Risk): g_new0 guarantees zero-initialized memory for the UI callback struct, preventing dangling pointers.
        AudioRowDeleteData *del_data = g_new0(AudioRowDeleteData, 1);
        strncpy(del_data->device_name, sd->audio_rows[i].dev.display_name, 127);
        del_data->w_lbl = lbl;
        del_data->w_pri = rb_pri;
        del_data->w_fall = rb_fall;
        del_data->w_pref = cb_pref;
        del_data->w_del = btn_del;

        g_signal_connect_data(click, "pressed", G_CALLBACK(on_delete_profile_clicked), del_data, (GClosureNotify)free_audio_row_delete_data, 0);

        gtk_grid_attach(GTK_GRID(audio_grid), btn_del, 4, i + 1, 1, 1);

        for (int p = 0; p < ui_state.config.num_audio_profiles; p++) {
            if (strcmp(ui_state.config.audio_profiles[p].device_name, sd->audio_rows[i].dev.display_name) == 0) {
                if (ui_state.config.audio_profiles[p].is_primary) gtk_check_button_set_active(GTK_CHECK_BUTTON(rb_pri), TRUE);
                if (ui_state.config.audio_profiles[p].is_fallback) gtk_check_button_set_active(GTK_CHECK_BUTTON(rb_fall), TRUE);
                if (ui_state.config.audio_profiles[p].in_pref) gtk_check_button_set_active(GTK_CHECK_BUTTON(cb_pref), TRUE);
                break;
            }
        }
    }

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(audio_scroll), audio_grid);
    gtk_frame_set_child(GTK_FRAME(audio_frame), audio_scroll);
    gtk_box_append(GTK_BOX(vbox), audio_frame);

    gtk_box_append(GTK_BOX(vbox), gtk_label_new("Audio Playback Target:"));
    GtkStringList *p_list = gtk_string_list_new(NULL);
    int p_active = 0;
    for (int i = 0; i < p_count; i++) {
        char label[256];
        snprintf(label, sizeof(label), "%s [%d ch]", sd->p_devs[i].display_name, sd->p_devs[i].channel_count);
        gtk_string_list_append(p_list, label);
        if (strcmp(sd->p_devs[i].display_name, ui_state.config.playback_target) == 0) p_active = i;
    }
    GtkWidget *p_drop = gtk_drop_down_new(G_LIST_MODEL(p_list), NULL);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(p_drop), p_active);
    gtk_box_append(GTK_BOX(vbox), p_drop);

    gtk_box_append(GTK_BOX(vbox), gtk_label_new("Recordings Folder:"));
    GtkWidget *rec_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    GtkWidget *lbl_rec_dir = gtk_label_new(ui_state.config.recordings_dir);
    gtk_widget_set_hexpand(lbl_rec_dir, TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(lbl_rec_dir), PANGO_ELLIPSIZE_START);
    gtk_label_set_xalign(GTK_LABEL(lbl_rec_dir), 0.0);
    GtkWidget *btn_rec_dir = gtk_button_new_from_icon_name("folder-open-symbolic");
    g_signal_connect(btn_rec_dir, "clicked", G_CALLBACK(on_select_recordings_dir), lbl_rec_dir);
    gtk_box_append(GTK_BOX(rec_hbox), lbl_rec_dir);
    gtk_box_append(GTK_BOX(rec_hbox), btn_rec_dir);
    gtk_box_append(GTK_BOX(vbox), rec_hbox);

    gtk_box_append(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    GtkWidget *wf_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 30);
    gtk_widget_set_halign(wf_hbox, GTK_ALIGN_CENTER);

    GtkWidget *limit_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_box_append(GTK_BOX(limit_vbox), gtk_label_new("Default Canvas Limit (min):"));
    GtkAdjustment *adj_limit = gtk_adjustment_new(ui_state.config.default_canvas_limit_min > 0 ? ui_state.config.default_canvas_limit_min : 5, 1, 120, 1, 5, 0);
    GtkWidget *spin_canvas_limit = gtk_spin_button_new(adj_limit, 1, 0);
    gtk_box_append(GTK_BOX(limit_vbox), spin_canvas_limit);
    gtk_box_append(GTK_BOX(wf_hbox), limit_vbox);

    GtkWidget *fmt_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_box_append(GTK_BOX(fmt_vbox), gtk_label_new("Audio Export:"));
    GtkStringList *f_list = gtk_string_list_new(NULL);
    gtk_string_list_append(f_list, "FLAC");
    gtk_string_list_append(f_list, "WAV");
    gtk_string_list_append(f_list, "FLAC + WAV");
    GtkWidget *f_drop = gtk_drop_down_new(G_LIST_MODEL(f_list), NULL);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(f_drop), ui_state.config.export_format);
    gtk_box_append(GTK_BOX(fmt_vbox), f_drop);
    gtk_box_append(GTK_BOX(wf_hbox), fmt_vbox);

    gtk_box_append(GTK_BOX(vbox), wf_hbox);

    // --- Logging Toggle ---
    GtkWidget *log_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_halign(log_hbox, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(log_hbox, 5);
    GtkWidget *cb_logging = gtk_check_button_new_with_label("Enable Persistent Logging to QJams.log (Requires Restart)");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(cb_logging), ui_state.config.enable_logging != 0);
    gtk_box_append(GTK_BOX(log_hbox), cb_logging);
    gtk_box_append(GTK_BOX(vbox), log_hbox);

    // --- Copyright & Footer ---
    GtkWidget *lbl_copyright = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lbl_copyright),
                         "<span size='small' foreground='#888888'>QJams (c) 2026 Rob Wijhenke. All rights reserved.\n"
                         "Beta version for testing. <a href='https://sites.google.com/view/qjams'>sites.google.com/view/qjams</a></span>");
    gtk_label_set_justify(GTK_LABEL(lbl_copyright), GTK_JUSTIFY_CENTER);
    gtk_widget_set_margin_top(lbl_copyright, 15);
    gtk_box_append(GTK_BOX(vbox), lbl_copyright);

    GtkWidget *btn_save = gtk_button_new_with_label("Save & Close");
    gtk_widget_set_halign(btn_save, GTK_ALIGN_END);
    gtk_widget_set_margin_top(btn_save, 10);
    gtk_box_append(GTK_BOX(vbox), btn_save);

    // --- Bind Data & Signals ---
    sd->v_drop = v_drop;
    sd->p_drop = p_drop;
    sd->f_drop = f_drop;
    sd->spin_canvas_limit = spin_canvas_limit;
    sd->cb_logging = cb_logging;

    g_object_set_data_full(G_OBJECT(settings_win), "settings_data", sd, g_free);

    g_signal_connect(btn_save, "clicked", G_CALLBACK(on_settings_save), sd);
    gtk_window_present(GTK_WINDOW(settings_win));
}
