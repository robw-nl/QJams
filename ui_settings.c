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
    GtkWidget *cb_cycler;
} AudioRowData;

typedef struct {
    GtkWidget *win;
    GtkWidget *v_drop;
    GtkWidget *p_drop;
    GtkWidget *f_drop;
    GtkWidget *spin_free;
    GtkWidget *spin_loop;
    // Hardware Matrix State
    AudioRowData audio_rows[MAX_AUDIO_DEVICES];
    int num_audio_rows;
    int a_count_online;
    // Hardware Pools
    VideoDevice v_devs[MAX_CAMERAS];
    AudioDevice a_devs[MAX_AUDIO_DEVICES];
    AudioDevice p_devs[MAX_AUDIO_DEVICES];
} SettingsData;

/**
 * @brief Applies new hardware configuration changes, re-patching JACK and restarting V4L2 as needed.
 * @param new_config Pointer to the updated configuration structure.
 * @return void
 */
static void apply_hardware_changes(QJamsConfig *new_config) {
    // 1. Audio Capture (JACK Hot-Patch)
    // FIX: Unconditionally patch audio ports to ensure late-booting hardware connects
    printf("State Manager: Re-patching Audio Capture to %s\n", new_config->audio_device);
    patch_audio_ports(new_config->audio_device);

    char new_label[256];
    snprintf(new_label, sizeof(new_label), "%s Gain (dB):", new_config->audio_device);
    gtk_label_set_text(GTK_LABEL(lbl_input_device), new_label);

    // Fetch stored gain profile or default to 1.0x (0dB)
    float target_multiplier = 1.0f;
    for (int i = 0; i < ui_state.config.num_audio_profiles; i++) {
        if (strcmp(ui_state.config.audio_profiles[i].device_name, new_config->audio_device) == 0) {
            target_multiplier = ui_state.config.audio_profiles[i].gain_multiplier;
            break;
        }
    }
    new_config->input_gain_multiplier = target_multiplier;
    set_input_gain(target_multiplier);

    // Update UI Spinner safely (using a timeout prevents GObject signal lockups)
    float loaded_db = (target_multiplier <= 0.001f) ? -24.0f : 20.0f * log10f(target_multiplier);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(input_gain_spinner), loaded_db);

    // 2. Audio Playback (JACK Hot-Patch)
    // FIX: Unconditionally patch playback ports
    printf("State Manager: Re-patching Audio Playback to %s\n", new_config->playback_target);
    patch_playback_ports(new_config->playback_target);

    // 3. Video Capture (V4L2 Teardown and Re-Init)
    // RESTART if device changed
    if (strcmp(ui_state.config.video_device, new_config->video_device) != 0) {
        printf("State Manager: Video Capture device changed.\n");
        stop_video_engine();
    if (init_and_start_video_engine(new_config->video_device, &video_queue) != 0) {
        printf("State Manager: Failed to start new video device.\n");
    }
        }

        // Commit state and save
        ui_state.config = *new_config;
        save_qjams_config(ui_state.config_path, &ui_state.config);
}

/**
 * @brief Callback triggered when saving settings, extracting values from the UI drops and saving to config.
 * Prevents segfaults by verifying the drop-down selection is valid before accessing device arrays.
 * @param btn The save button triggering the callback.
 * @param user_data Pointer to the SettingsData struct containing the dialog's state.
 * @return void
 */
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

    if (v_idx != GTK_INVALID_LIST_POSITION) {
        strncpy(new_config.video_device, sd->v_devs[v_idx].device_path, sizeof(new_config.video_device) - 1);
    }
    if (p_idx != GTK_INVALID_LIST_POSITION) {
        strncpy(new_config.playback_target, sd->p_devs[p_idx].display_name, sizeof(new_config.playback_target) - 1);
    }

    // Capture the matrix row states
    char target_device[128] = "";
    int pri_online = 0, fall_online = 0;
    char pri_name[128] = "", fall_name[128] = "";

    for (int i = 0; i < sd->num_audio_rows; i++) {
        bool is_pri = gtk_check_button_get_active(GTK_CHECK_BUTTON(sd->audio_rows[i].rb_primary));
        bool is_fall = gtk_check_button_get_active(GTK_CHECK_BUTTON(sd->audio_rows[i].rb_fallback));
        bool in_cyc = gtk_check_button_get_active(GTK_CHECK_BUTTON(sd->audio_rows[i].cb_cycler));

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
            new_config.audio_profiles[p_idx_audio].in_cycler = in_cyc ? 1 : 0;
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

    // Prioritize hardware targeting dynamically based on connection status
    if (pri_online) strncpy(target_device, pri_name, 127);
    else if (fall_online) strncpy(target_device, fall_name, 127);
    else if (sd->a_count_online > 0) strncpy(target_device, sd->a_devs[0].display_name, 127);

    if (strlen(target_device) > 0) {
        strncpy(new_config.audio_device, target_device, 127);
    }

    new_config.freestyle_duration_min = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(sd->spin_free));
    new_config.multitrack_duration_min = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(sd->spin_loop));

    bool mode_changed = (ui_state.config.multitrack_mode_active != new_config.multitrack_mode_active);
    bool limits_changed = (ui_state.config.freestyle_duration_min != new_config.freestyle_duration_min) ||
    (ui_state.config.multitrack_duration_min != new_config.multitrack_duration_min);

    apply_hardware_changes(&new_config);

    if (mode_changed) {
        adjust_blank_canvas_for_mode(new_config.multitrack_mode_active != 0);
    } else if (limits_changed) {
        // Apply new limit non-destructively to the active canvas
        extern int resize_loop_canvas_seconds(int);
        extern void update_zoom_button_label_to_length(void);
        extern void invalidate_waveform_caches(void);
        extern GtkWidget *waveform_area_bt;
        extern GtkWidget *waveform_area_input;

        int new_limit = new_config.multitrack_mode_active ? new_config.multitrack_duration_min : new_config.freestyle_duration_min;
        if (resize_loop_canvas_seconds(new_limit * 60) == 0) {
            update_zoom_button_label_to_length();
            invalidate_waveform_caches();
            if (waveform_area_bt) gtk_widget_queue_draw(waveform_area_bt);
            if (waveform_area_input) gtk_widget_queue_draw(waveform_area_input);

            if (strlen(ui_state.selected_track_path) == 0) {
                char track_lbl[128];
                if (new_config.multitrack_mode_active) {
                    snprintf(track_lbl, sizeof(track_lbl), "Track: Blank Loop Canvas [%02d:00 limit]", new_limit);
                } else {
                    snprintf(track_lbl, sizeof(track_lbl), "Track: Freestyle Recording [%02d:00 limit]", new_limit);
                }
                extern GtkWidget *lbl_track;
                gtk_label_set_text(GTK_LABEL(lbl_track), track_lbl);
            }
        }
    }

    extern void update_dashboard_cycler_ui(void);
    update_dashboard_cycler_ui();

    GtkWidget *win = sd->win;
    gtk_window_destroy(GTK_WINDOW(win));

    printf("Hardware Matrix and Workflow settings applied dynamically.\n");
}

/**
 * @brief Callback invoked after the user selects a recordings directory from the file dialog.
 * @param source_object The file dialog object.
 * @param res The async result of the dialog operation.
 * @param user_data Pointer to the label widget to update.
 * @return void
 */
static void on_recordings_dir_chosen(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source_object);
    GtkWidget *lbl_recordings_dir = GTK_WIDGET(user_data);
    GError *error = NULL;
    GFile *folder = gtk_file_dialog_select_folder_finish(dialog, res, &error);

    if (folder) {
        char *path = g_file_get_path(folder);
        strncpy(ui_state.config.recordings_dir, path, sizeof(ui_state.config.recordings_dir) - 1);
        save_qjams_config(ui_state.config_path, &ui_state.config);
        gtk_label_set_text(GTK_LABEL(lbl_recordings_dir), path);
        g_free(path);
        g_object_unref(folder);
    }
}

/**
 * @brief Callback to open a folder selection dialog for changing the recordings directory.
 * @param btn The button triggering the callback.
 * @param user_data Pointer to the label widget that displays the current directory.
 * @return void
 */
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

/**
 * @brief Handles the settings button click, opening the hardware preferences dialog.
 * @param button The GTK button triggering the callback.
 * @param user_data Pointer to the main window to set as the transient parent.
 * @return void
 */
void on_settings_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    GtkWidget *settings_win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(settings_win), "Hardware & Workflow Preferences");
    gtk_window_set_transient_for(GTK_WINDOW(settings_win), GTK_WINDOW(user_data));
    gtk_window_set_modal(GTK_WINDOW(settings_win), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(settings_win), 540, 520);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 15);
    gtk_widget_set_margin_start(vbox, 20);
    gtk_widget_set_margin_end(vbox, 20);
    gtk_widget_set_margin_top(vbox, 20);
    gtk_widget_set_margin_bottom(vbox, 20);
    gtk_window_set_child(GTK_WINDOW(settings_win), vbox);

    // Allocate the unified data struct. Calloc zeroes out the internal arrays cleanly.
    SettingsData *sd = g_new0(SettingsData, 1);
    sd->win = settings_win;

    int v_count = scan_video_devices(sd->v_devs, MAX_CAMERAS);
    int a_count = scan_audio_devices(client, sd->a_devs, MAX_AUDIO_DEVICES);
    int p_count = scan_playback_devices(client, sd->p_devs, MAX_AUDIO_DEVICES);

    // VIDEO SELECTOR
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

    // AUDIO CAPTURE MATRIX
    gtk_box_append(GTK_BOX(vbox), gtk_label_new("Audio Capture Matrix:"));
    GtkWidget *audio_frame = gtk_frame_new(NULL);
    GtkWidget *audio_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(audio_scroll), 150);
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
    GtkWidget *h_cyc = gtk_label_new(NULL); gtk_label_set_markup(GTK_LABEL(h_cyc), "<b>Cycler</b>");

    gtk_widget_set_halign(h_dev, GTK_ALIGN_START);
    gtk_widget_set_hexpand(h_dev, TRUE);

    gtk_grid_attach(GTK_GRID(audio_grid), h_dev, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(audio_grid), h_pri, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(audio_grid), h_fall, 2, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(audio_grid), h_cyc, 3, 0, 1, 1);

    // Combine offline config profiles with actively probed devices into a unified pool
    int row_count = 0;
    for (int p = 0; p < ui_state.config.num_audio_profiles && row_count < MAX_AUDIO_DEVICES; p++) {
        strncpy(sd->audio_rows[row_count].dev.display_name, ui_state.config.audio_profiles[p].device_name, 127);
        sd->audio_rows[row_count].dev.channel_count = 0; // Assume 0 to flag offline unless probed later
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
        gtk_widget_set_halign(rb_pri, GTK_ALIGN_CENTER);
        gtk_grid_attach(GTK_GRID(audio_grid), rb_pri, 1, i + 1, 1, 1);

        GtkWidget *rb_fall = gtk_check_button_new();
        if (fall_group) gtk_check_button_set_group(GTK_CHECK_BUTTON(rb_fall), GTK_CHECK_BUTTON(fall_group));
        else fall_group = rb_fall;
        gtk_widget_set_halign(rb_fall, GTK_ALIGN_CENTER);
        gtk_grid_attach(GTK_GRID(audio_grid), rb_fall, 2, i + 1, 1, 1);

        GtkWidget *cb_cyc = gtk_check_button_new();
        gtk_widget_set_halign(cb_cyc, GTK_ALIGN_CENTER);
        gtk_grid_attach(GTK_GRID(audio_grid), cb_cyc, 3, i + 1, 1, 1);

        sd->audio_rows[i].rb_primary = rb_pri;
        sd->audio_rows[i].rb_fallback = rb_fall;
        sd->audio_rows[i].cb_cycler = cb_cyc;

        for (int p = 0; p < ui_state.config.num_audio_profiles; p++) {
            if (strcmp(ui_state.config.audio_profiles[p].device_name, sd->audio_rows[i].dev.display_name) == 0) {
                if (ui_state.config.audio_profiles[p].is_primary) gtk_check_button_set_active(GTK_CHECK_BUTTON(rb_pri), TRUE);
                if (ui_state.config.audio_profiles[p].is_fallback) gtk_check_button_set_active(GTK_CHECK_BUTTON(rb_fall), TRUE);
                if (ui_state.config.audio_profiles[p].in_cycler) gtk_check_button_set_active(GTK_CHECK_BUTTON(cb_cyc), TRUE);
                break;
            }
        }
    }

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(audio_scroll), audio_grid);
    gtk_frame_set_child(GTK_FRAME(audio_frame), audio_scroll);
    gtk_box_append(GTK_BOX(vbox), audio_frame);

    // PLAYBACK SELECTOR
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

    // RECORDINGS FOLDER SELECTOR
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

    // WORKFLOW PREFERENCES
    gtk_box_append(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    GtkWidget *wf_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 30);
    gtk_widget_set_halign(wf_hbox, GTK_ALIGN_CENTER);

    // Freestyle Spinner
    GtkWidget *free_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_box_append(GTK_BOX(free_vbox), gtk_label_new("Freestyle Limit (min):"));
    GtkAdjustment *adj_free = gtk_adjustment_new(ui_state.config.freestyle_duration_min > 0 ? ui_state.config.freestyle_duration_min : 15, 1, 120, 1, 5, 0);
    GtkWidget *spin_free = gtk_spin_button_new(adj_free, 1, 0);
    gtk_box_append(GTK_BOX(free_vbox), spin_free);
    gtk_box_append(GTK_BOX(wf_hbox), free_vbox);

    // Looper Spinner
    GtkWidget *loop_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_box_append(GTK_BOX(loop_vbox), gtk_label_new("Multi-Track Limit (min):"));
    GtkAdjustment *adj_loop = gtk_adjustment_new(ui_state.config.multitrack_duration_min > 0 ? ui_state.config.multitrack_duration_min : 5, 1, 60, 1, 5, 0);
    GtkWidget *spin_loop = gtk_spin_button_new(adj_loop, 1, 0);
    gtk_box_append(GTK_BOX(loop_vbox), spin_loop);
    gtk_box_append(GTK_BOX(wf_hbox), loop_vbox);

    // Export Format Selector
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

    // SAVE BUTTON
    GtkWidget *btn_save = gtk_button_new_with_label("Save & Close");
    gtk_widget_set_halign(btn_save, GTK_ALIGN_END);
    gtk_widget_set_margin_top(btn_save, 10);
    gtk_box_append(GTK_BOX(vbox), btn_save);

    sd->v_drop = v_drop;
    sd->p_drop = p_drop;
    sd->f_drop = f_drop;
    sd->spin_free = spin_free;
    sd->spin_loop = spin_loop;

    // Bind the struct explicitly to the GObject finalizer to guarantee leak-free cleanup
    g_object_set_data_full(G_OBJECT(settings_win), "settings_data", sd, g_free);

    g_signal_connect(btn_save, "clicked", G_CALLBACK(on_settings_save), sd);
    gtk_window_present(GTK_WINDOW(settings_win));
}
