#include "ui_settings.h"
#include "ui_globals.h"
#include "audio_engine.h"
#include "video_engine.h"
#include "scanner.h"
#include "ui_dashboard.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

extern jack_client_t *client;
extern SPSC_Video_Queue video_queue;

typedef struct {
    GtkWidget *win;
    GtkWidget *v_drop;
    GtkWidget *a_drop;
    GtkWidget *p_drop;
    GtkWidget *s_drop;
    GtkWidget *spin_free; // NEW: Pointer to Freestyle spinner
    GtkWidget *spin_loop; // NEW: Pointer to Looper spinner
    VideoDevice *v_devs;
    AudioDevice *a_devs;
    AudioDevice *p_devs;
} SettingsData;

/**
 * @brief Applies new hardware configuration changes, re-patching JACK and restarting V4L2 as needed.
 * @param new_config Pointer to the updated configuration structure.
 * @return void
 */
static void apply_hardware_changes(QJamsConfig *new_config) {
    // 1. Audio Capture (JACK Hot-Patch)
    if (strcmp(ui_state.config.audio_device, new_config->audio_device) != 0) {
        printf("State Manager: Audio Capture changed to %s\n", new_config->audio_device);
        patch_audio_ports(new_config->audio_device);

        char new_label[256];
        snprintf(new_label, sizeof(new_label), "%s Gain (dB):", new_config->audio_device);
        gtk_label_set_text(GTK_LABEL(lbl_input_device), new_label);

        // Fetch stored gain profile or default to 1.0x (0dB)
        float target_multiplier = 1.0f;
        for (int i = 0; i < ui_state.config.num_saved_input_gains; i++) {
            if (strcmp(ui_state.config.input_gains[i].device_name, new_config->audio_device) == 0) {
                target_multiplier = ui_state.config.input_gains[i].gain_multiplier;
                break;
            }
        }
        new_config->input_gain_multiplier = target_multiplier;
        set_input_gain(target_multiplier);

        // Update UI Spinner safely (using a timeout prevents GObject signal lockups)
        float loaded_db = (target_multiplier <= 0.001f) ? -24.0f : 20.0f * log10f(target_multiplier);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(input_gain_spinner), loaded_db);
    }

    // 2. Audio Playback (JACK Hot-Patch)
    if (strcmp(ui_state.config.playback_target, new_config->playback_target) != 0) {
        printf("State Manager: Audio Playback changed to %s\n", new_config->playback_target);
        patch_playback_ports(new_config->playback_target);
    }

    // 3. Video Capture (V4L2 Teardown and Re-Init)
    // RESTART if device changed OR if preview resolution changed
    if (strcmp(ui_state.config.video_device, new_config->video_device) != 0 ||
        ui_state.config.video_preview_width != new_config->video_preview_width) {
        printf("State Manager: Video Capture / Resolution changed.\n");
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
    guint a_idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(sd->a_drop));
    guint p_idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(sd->p_drop));
    guint s_idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(sd->s_drop));

    QJamsConfig new_config = ui_state.config;

    if (s_idx != GTK_INVALID_LIST_POSITION) {
        if (s_idx == 0) { new_config.video_preview_width = 676; new_config.video_preview_height = 380; }
        else if (s_idx == 1) { new_config.video_preview_width = 854; new_config.video_preview_height = 480; }
        else if (s_idx == 2) { new_config.video_preview_width = 1280; new_config.video_preview_height = 720; }
    }

    if (v_idx != GTK_INVALID_LIST_POSITION) {
        strncpy(new_config.video_device, sd->v_devs[v_idx].device_path, sizeof(new_config.video_device) - 1);
    }
    if (a_idx != GTK_INVALID_LIST_POSITION) {
        strncpy(new_config.audio_device, sd->a_devs[a_idx].display_name, sizeof(new_config.audio_device) - 1);
    }
    if (p_idx != GTK_INVALID_LIST_POSITION) {
        strncpy(new_config.playback_target, sd->p_devs[p_idx].display_name, sizeof(new_config.playback_target) - 1);
    }

    new_config.freestyle_duration_min = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(sd->spin_free));
    new_config.looper_duration_min = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(sd->spin_loop));

    // Cache the old width BEFORE overwriting the global state
    int old_vid_w = ui_state.config.video_preview_width;

    apply_hardware_changes(&new_config);

    // Apply UI scaling dynamically if changed
    if (old_vid_w != new_config.video_preview_width) {
        apply_video_preview_size(new_config.video_preview_width, new_config.video_preview_height);
    }

    // NEW: Instantly resize and refresh the active blank canvas (if one exists)
    adjust_blank_canvas_for_mode(new_config.looper_mode_active != 0);

    // Destroying the window automatically triggers on_settings_destroy, freeing the memory safely.
    gtk_window_destroy(GTK_WINDOW(sd->win));

    printf("Hardware and Workflow settings applied dynamically.\n");
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
 * @brief Destructor callback for the settings window to prevent memory leaks on modal dismissal.
 * @param window The settings window.
 * @param user_data Pointer to the SettingsData struct.
 * @return void
 */
static void on_settings_destroy(GtkWindow *window, gpointer user_data) {
    (void)window;
    SettingsData *sd = (SettingsData*)user_data;
    if (sd) {
        free(sd->v_devs);
        free(sd->a_devs);
        free(sd->p_devs);
        free(sd);
    }
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
    gtk_window_set_default_size(GTK_WINDOW(settings_win), 450, 420); // Slightly taller to fit workflow settings

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 15);
    gtk_widget_set_margin_start(vbox, 20);
    gtk_widget_set_margin_end(vbox, 20);
    gtk_widget_set_margin_top(vbox, 20);
    gtk_widget_set_margin_bottom(vbox, 20);
    gtk_window_set_child(GTK_WINDOW(settings_win), vbox);

    VideoDevice *v_devs = calloc(MAX_CAMERAS, sizeof(VideoDevice));
    AudioDevice *a_devs = calloc(MAX_AUDIO_DEVICES, sizeof(AudioDevice));
    AudioDevice *p_devs = calloc(MAX_AUDIO_DEVICES, sizeof(AudioDevice));

    int v_count = scan_video_devices(v_devs, MAX_CAMERAS);
    int a_count = scan_audio_devices(client, a_devs, MAX_AUDIO_DEVICES);
    int p_count = scan_playback_devices(client, p_devs, MAX_AUDIO_DEVICES);

    // VIDEO SELECTOR
    gtk_box_append(GTK_BOX(vbox), gtk_label_new("Camera Source:"));
    GtkStringList *v_list = gtk_string_list_new(NULL);
    int v_active = 0;
    for (int i = 0; i < v_count; i++) {
        char label[128];
        snprintf(label, sizeof(label), "%s (%s)", v_devs[i].device_name, v_devs[i].device_path);
        gtk_string_list_append(v_list, label);
        if (strcmp(v_devs[i].device_path, ui_state.config.video_device) == 0) v_active = i;
    }
    GtkWidget *v_drop = gtk_drop_down_new(G_LIST_MODEL(v_list), NULL);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(v_drop), v_active);
    gtk_box_append(GTK_BOX(vbox), v_drop);

    // VIDEO PREVIEW SIZE SELECTOR
    gtk_box_append(GTK_BOX(vbox), gtk_label_new("Video Preview Size:"));
    GtkStringList *s_list = gtk_string_list_new(NULL);
    gtk_string_list_append(s_list, "360p (676x380)");
    gtk_string_list_append(s_list, "480p (854x480) - Default");
    gtk_string_list_append(s_list, "720p (1280x720)");
    GtkWidget *s_drop = gtk_drop_down_new(G_LIST_MODEL(s_list), NULL);

    guint active_size = 1; // Default 480p
    if (ui_state.config.video_preview_width <= 676) active_size = 0;
    else if (ui_state.config.video_preview_width == 854) active_size = 1;
    else if (ui_state.config.video_preview_width >= 1280) active_size = 2;

    gtk_drop_down_set_selected(GTK_DROP_DOWN(s_drop), active_size);
    gtk_box_append(GTK_BOX(vbox), s_drop);

    // AUDIO SELECTOR
    gtk_box_append(GTK_BOX(vbox), gtk_label_new("Audio Capture Source:"));
    GtkStringList *a_list = gtk_string_list_new(NULL);
    int a_active = 0;
    for (int i = 0; i < a_count; i++) {
        char label[128];
        snprintf(label, sizeof(label), "%s [%d ch]", a_devs[i].display_name, a_devs[i].channel_count);
        gtk_string_list_append(a_list, label);
        if (strcmp(a_devs[i].display_name, ui_state.config.audio_device) == 0) a_active = i;
    }
    GtkWidget *a_drop = gtk_drop_down_new(G_LIST_MODEL(a_list), NULL);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(a_drop), a_active);
    gtk_box_append(GTK_BOX(vbox), a_drop);

    // PLAYBACK SELECTOR
    gtk_box_append(GTK_BOX(vbox), gtk_label_new("Audio Playback Target:"));
    GtkStringList *p_list = gtk_string_list_new(NULL);
    int p_active = 0;
    for (int i = 0; i < p_count; i++) {
        char label[128];
        snprintf(label, sizeof(label), "%s [%d ch]", p_devs[i].display_name, p_devs[i].channel_count);
        gtk_string_list_append(p_list, label);
        if (strcmp(p_devs[i].display_name, ui_state.config.playback_target) == 0) p_active = i;
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

    // WORKFLOW PREFERENCES (NEW)
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
    gtk_box_append(GTK_BOX(loop_vbox), gtk_label_new("Looper Limit (min):"));
    GtkAdjustment *adj_loop = gtk_adjustment_new(ui_state.config.looper_duration_min > 0 ? ui_state.config.looper_duration_min : 5, 1, 60, 1, 5, 0);
    GtkWidget *spin_loop = gtk_spin_button_new(adj_loop, 1, 0);
    gtk_box_append(GTK_BOX(loop_vbox), spin_loop);
    gtk_box_append(GTK_BOX(wf_hbox), loop_vbox);

    gtk_box_append(GTK_BOX(vbox), wf_hbox);

    // SAVE BUTTON
    GtkWidget *btn_save = gtk_button_new_with_label("Save & Close");
    gtk_widget_set_halign(btn_save, GTK_ALIGN_END);
    gtk_widget_set_margin_top(btn_save, 10);
    gtk_box_append(GTK_BOX(vbox), btn_save);

    SettingsData *sd = malloc(sizeof(SettingsData));
    sd->win = settings_win;
    sd->v_drop = v_drop;
    sd->a_drop = a_drop;
    sd->p_drop = p_drop;
    sd->s_drop = s_drop;
    sd->spin_free = spin_free;
    sd->spin_loop = spin_loop;
    sd->v_devs = v_devs;
    sd->a_devs = a_devs;
    sd->p_devs = p_devs;

    // Bind the cleanup function to the window destruction phase
    g_signal_connect(settings_win, "destroy", G_CALLBACK(on_settings_destroy), sd);
    g_signal_connect(btn_save, "clicked", G_CALLBACK(on_settings_save), sd);
    gtk_window_present(GTK_WINDOW(settings_win));
}
