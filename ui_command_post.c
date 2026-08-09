#include "ui_command_post.h"
#include "ui_globals.h"
#include "audio_engine.h"
#include "ui_looper.h"
#include "ui_dashboard.h"
#include "ui_settings.h"
#include "ui_playlist.h"
#include "session.h"
#include "ui_waveforms.h"
#include <sndfile.h>
#include <string.h>

GPid active_muxer_pid = 0;

static void on_file_chosen(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    (void)user_data;
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source_object);
    GError *error = NULL;
    GFile *file = gtk_file_dialog_open_finish(dialog, res, &error);

    if (file) {
        char *path = g_file_get_path(file);
        char *dir = g_file_get_parent(file) ? g_file_get_path(g_file_get_parent(file)) : NULL;

        if (g_str_has_suffix(path, ".m3u")) {
            load_playlist_from_file(path);
        } else {
            strncpy(ui_state.selected_track_path, path, sizeof(ui_state.selected_track_path) - 1);
            trigger_track_load();
        }

        if (dir) {
            strncpy(ui_state.config.last_track_dir, dir, sizeof(ui_state.config.last_track_dir) - 1);
            save_qjams_config(ui_state.config_path, &ui_state.config);
            g_free(dir);
        }

        g_free(path);
        g_object_unref(file);
    }
}

void on_select_track_clicked(GtkButton *button, gpointer window) {
    (void)button;
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Export Mix");

    // FIX: Set the exact final file name so GTK handles the file overwrite prompt natively
    if (atomic_load_explicit(&is_looper_mode, memory_order_acquire)) {
        gtk_file_dialog_set_initial_name(dialog, "MySong-MIX.flac");
    } else {
        gtk_file_dialog_set_initial_name(dialog, "MySong-MIX.mkv");
    }

    if (strlen(ui_state.config.recordings_dir) > 0) {
        GFile *initial_folder = g_file_new_for_path(ui_state.config.last_track_dir);
        gtk_file_dialog_set_initial_folder(dialog, initial_folder);
        g_object_unref(initial_folder);
    }

    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Media Files (FLAC, WAV, OGG, MP3, MKV, M3U)");
    static const char *supported_exts[] = {"*.flac", "*.wav", "*.ogg", "*.mp3", "*.mkv", "*.m3u"};
    for (size_t i = 0; i < sizeof(supported_exts) / sizeof(supported_exts[0]); i++) {
        gtk_file_filter_add_pattern(filter, supported_exts[i]);
    }

    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, filter);
    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
    gtk_file_dialog_set_default_filter(dialog, filter);
    g_object_unref(filter);
    g_object_unref(filters);

    gtk_file_dialog_open(dialog, GTK_WINDOW(window), NULL, on_file_chosen, NULL);
}

static void on_muxer_finished(GPid pid, gint status, gpointer user_data) {
    (void)user_data;
    g_spawn_close_pid(pid);
    active_muxer_pid = 0;
    gtk_spinner_stop(GTK_SPINNER(main_spinner)); // Halt the spinner

    if (status == 0) gtk_label_set_text(GTK_LABEL(lbl_status), "Status: Muxing Complete!");
    else gtk_label_set_text(GTK_LABEL(lbl_status), "Status: Muxing Failed (FFmpeg Error)");
    gtk_widget_set_sensitive(btn_save_mux, TRUE);
}

static void on_save_mux_file_chosen(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    (void)user_data;
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source_object);
    GError *error = NULL;
    GFile *file = gtk_file_dialog_save_finish(dialog, res, &error);

    if (!file) {
        gtk_widget_set_sensitive(btn_save_mux, TRUE);
        gtk_label_set_text(GTK_LABEL(lbl_status), "Status: Export Cancelled");
        return;
    }

    gtk_widget_remove_css_class(btn_save_mux, "needs-save");
    gtk_widget_remove_css_class(btn_save_session, "needs-save");

    char *chosen_path = g_file_get_path(file);
    g_object_unref(file);

    char *dir = g_path_get_dirname(chosen_path);
    char *basename_ext = g_path_get_basename(chosen_path);

    // 1. Safely derive the pure title without extensions or existing "-MIX" suffixes
    char base_no_ext[512];
    strncpy(base_no_ext, basename_ext, sizeof(base_no_ext) - 1);
    char *dot = strrchr(base_no_ext, '.');
    if (dot) *dot = '\0';

    size_t len = strlen(base_no_ext);
    if (len > 4 && strcmp(base_no_ext + len - 4, "-MIX") == 0) {
        base_no_ext[len - 4] = '\0';
    }

    // 2. UNCONDITIONALLY enforce the exact file suffixes to guarantee proper naming
    bool is_looper = atomic_load_explicit(&is_looper_mode, memory_order_acquire);
    char raw_path[1024];
    char mix_path[1024];
    snprintf(raw_path, sizeof(raw_path), "%s/%s-RAW.mkv", dir, base_no_ext);
    snprintf(mix_path, sizeof(mix_path), "%s/%s-MIX.%s", dir, base_no_ext, is_looper ? "flac" : "mkv");

    char temp_flac[1024];
    snprintf(temp_flac, sizeof(temp_flac), "/tmp/%s-RAW.flac", base_no_ext);

    time_t t = time(NULL);
    struct tm tm = *localtime(&t);
    char date_str[32];
    snprintf(date_str, sizeof(date_str), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);

    gtk_label_set_text(GTK_LABEL(lbl_status), "Status: Exporting Audio/Video...");
    gtk_spinner_start(GTK_SPINNER(main_spinner)); // Start the spinner

    size_t start_f = 0;
    size_t end_f = atomic_load_explicit(&backing_track_frames, memory_order_acquire);

    if (atomic_load_explicit(&loop_active, memory_order_acquire)) {
        start_f = atomic_load_explicit(&loop_start_frame, memory_order_acquire);
        end_f = atomic_load_explicit(&loop_end_frame, memory_order_acquire);
    }

    if (is_looper) {
        SF_INFO sfinfo = {0};
        sfinfo.channels = 2;
        sfinfo.samplerate = atomic_load_explicit(&active_sample_rate, memory_order_acquire);
        sfinfo.format = SF_FORMAT_FLAC | SF_FORMAT_PCM_16;

        SNDFILE *outfile = sf_open(temp_flac, SFM_WRITE, &sfinfo);
        size_t frames_to_write = end_f - start_f;

        if (outfile && frames_to_write > 0) {
            int active = atomic_load_explicit(&active_layer_count, memory_order_acquire);
            float *mix_buf = calloc(frames_to_write * 2, sizeof(float));
            if (mix_buf) {
                for (int l = 0; l < active && l < MAX_LOOPS; l++) {
                    if (loop_layers[l] && !atomic_load_explicit(&layer_is_muted[l], memory_order_acquire)) {
                        for (size_t i = 0; i < frames_to_write; i++) {
                            mix_buf[i * 2] += loop_layers[l][(start_f + i) * 2];
                            mix_buf[i * 2 + 1] += loop_layers[l][(start_f + i) * 2 + 1];
                        }
                    }
                }
                for (size_t i = 0; i < frames_to_write * 2; i++) {
                    if (mix_buf[i] > 1.0f) mix_buf[i] = 1.0f;
                    else if (mix_buf[i] < -1.0f) mix_buf[i] = -1.0f;
                }
                sf_writef_float(outfile, mix_buf, frames_to_write);
                free(mix_buf);
            }
            sf_close(outfile);

            char *argv[32];
            int argc = 0;
            argv[argc++] = "ffmpeg"; argv[argc++] = "-y";
            argv[argc++] = "-i"; argv[argc++] = temp_flac;

            char *meta_title = g_strdup_printf("title=%s", base_no_ext);
            char *meta_date = g_strdup_printf("recordingdate=%s", date_str);
            char *meta_comment = g_strdup("comment=Recorded with QJams. (c) Rob Wijhenke - https://sites.google.com/view/qjams");

            argv[argc++] = "-metadata"; argv[argc++] = meta_title;
            argv[argc++] = "-metadata"; argv[argc++] = meta_date;
            argv[argc++] = "-metadata"; argv[argc++] = meta_comment;

            argv[argc++] = "-c:a"; argv[argc++] = "copy";
            argv[argc++] = mix_path; argv[argc++] = NULL;

            GError *spawn_err = NULL;
            if (g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL, &active_muxer_pid, &spawn_err)) {
                g_child_watch_add(active_muxer_pid, on_muxer_finished, NULL);
                gtk_label_set_text(GTK_LABEL(lbl_status), "Status: Saved, Tagging FLAC...");
            } else {
                gtk_label_set_text(GTK_LABEL(lbl_status), "Status: FFmpeg Tagging Failed");
                gtk_spinner_stop(GTK_SPINNER(main_spinner));
                g_error_free(spawn_err);
                gtk_widget_set_sensitive(btn_save_mux, TRUE);
            }

            g_free(meta_title);
            g_free(meta_date);
            g_free(meta_comment);
        } else {
            gtk_label_set_text(GTK_LABEL(lbl_status), "Status: Failed to open FLAC for writing");
            gtk_spinner_stop(GTK_SPINNER(main_spinner));
            gtk_widget_set_sensitive(btn_save_mux, TRUE);
        }
        g_free(dir); g_free(basename_ext); g_free(chosen_path);
        return;
    }

    // Standard Mode: Explicitly route RAW cache through FFmpeg for dual-output tagging and mixdown
    char volume_filter[512];
    int in_w = (int)ui_state.config.input_gain_multiplier;
    int in_f = (int)(((ui_state.config.input_gain_multiplier - in_w) * 10000.0f) + 0.5f);

    if (end_f == 0) {
        snprintf(volume_filter, sizeof(volume_filter),
                 "[0:a:1]volume=%d.%04d[a]",
                 in_w, in_f);
    } else {
        int bt_w = (int)ui_state.config.bt_gain_multiplier;
        int bt_f = (int)(((ui_state.config.bt_gain_multiplier - bt_w) * 10000.0f) + 0.5f);
        snprintf(volume_filter, sizeof(volume_filter),
                 "[0:a:0]volume=%d.%04d[a0];[0:a:1]volume=%d.%04d[a1];[a0][a1]amerge=inputs=2[am];[am]pan=stereo|c0=c0+c2|c1=c1+c3[ap];[ap]alimiter=limit=0.944:attack=0.1:release=50[a]",
                 bt_w, bt_f, in_w, in_f);
    }

    char *argv[64];
    int argc = 0;
    argv[argc++] = "ffmpeg"; argv[argc++] = "-y";

    // FIX: The current_raw_path MKV is already exactly the duration of the recording session!
    // Never apply -ss or -to arguments here, as it will cause an out-of-bounds seek error.
    argv[argc++] = "-i"; argv[argc++] = current_raw_path;

    char *meta_title = g_strdup_printf("title=%s", base_no_ext);
    char *meta_date = g_strdup_printf("recordingdate=%s", date_str);
    char *meta_comment = g_strdup("comment=Recorded with QJams. (c) Rob Wijhenke - https://sites.google.com/view/qjams");

    argv[argc++] = "-filter_complex"; argv[argc++] = volume_filter;

    // OUTPUT 1: The Tagged RAW File
    if (end_f == 0) {
        argv[argc++] = "-map"; argv[argc++] = "0:v?";
        argv[argc++] = "-map"; argv[argc++] = "0:a:1";
    } else {
        argv[argc++] = "-map"; argv[argc++] = "0";
    }
    argv[argc++] = "-c"; argv[argc++] = "copy";
    argv[argc++] = "-metadata"; argv[argc++] = meta_title;
    argv[argc++] = "-metadata"; argv[argc++] = meta_date;
    argv[argc++] = "-metadata"; argv[argc++] = meta_comment;
    argv[argc++] = raw_path;

    // OUTPUT 2: The Tagged MIX File
    argv[argc++] = "-map"; argv[argc++] = "0:v?";
    argv[argc++] = "-map"; argv[argc++] = "[a]";
    argv[argc++] = "-c:v"; argv[argc++] = "copy";
    argv[argc++] = "-c:a"; argv[argc++] = "flac";
    argv[argc++] = "-metadata"; argv[argc++] = meta_title;
    argv[argc++] = "-metadata"; argv[argc++] = meta_date;
    argv[argc++] = "-metadata"; argv[argc++] = meta_comment;
    argv[argc++] = mix_path;

    argv[argc++] = NULL;

    GError *spawn_err = NULL;
    if (g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL, &active_muxer_pid, &spawn_err)) {
        g_child_watch_add(active_muxer_pid, on_muxer_finished, NULL);
        gtk_label_set_text(GTK_LABEL(lbl_status), "Status: Saved, Muxing in Background");
    } else {
        gtk_label_set_text(GTK_LABEL(lbl_status), "Status: FFmpeg Launch Failed");
        gtk_spinner_stop(GTK_SPINNER(main_spinner));
        g_error_free(spawn_err);
        gtk_widget_set_sensitive(btn_save_mux, TRUE);
    }

    g_free(meta_title);
    g_free(meta_date);
    g_free(meta_comment);
    g_free(dir);
    g_free(basename_ext);
    g_free(chosen_path);
}

void on_save_mux_clicked(GtkButton *button, gpointer user_data) {
    (void)button; (void)user_data;

    // REMOVED: The 'if (end_f == 0) return;' check has been deleted to allow Freestyle Recording

    gtk_widget_set_sensitive(btn_save_mux, FALSE);

    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Export Mix");

    // FIX: Set the exact final file name so GTK handles the file overwrite prompt natively
    if (atomic_load_explicit(&is_looper_mode, memory_order_acquire)) {
        gtk_file_dialog_set_initial_name(dialog, "MySong-MIX.flac");
    } else {
        gtk_file_dialog_set_initial_name(dialog, "MySong-MIX.mkv");
    }

    if (strlen(ui_state.config.recordings_dir) > 0) {
        GFile *initial_folder = g_file_new_for_path(ui_state.config.recordings_dir);
        gtk_file_dialog_set_initial_folder(dialog, initial_folder);
        g_object_unref(initial_folder);
    }

    gtk_file_dialog_save(dialog, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(button))), NULL, on_save_mux_file_chosen, NULL);
}

static void on_save_session_file_chosen(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    (void)user_data;
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source_object);
    GError *error = NULL;
    GFile *file = gtk_file_dialog_save_finish(dialog, res, &error);
    if (file) {
        char *path = g_file_get_path(file);
        on_stop_clicked(NULL, NULL);
        if (save_qjams_session(path) == 0) {
            char *basename = g_path_get_basename(path);
            char status[512];
            snprintf(status, sizeof(status), "Status: Session saved to %s", basename);
            gtk_label_set_text(GTK_LABEL(lbl_status), status);
            g_free(basename);

            gtk_widget_remove_css_class(btn_save_mux, "needs-save");
            gtk_widget_remove_css_class(btn_save_session, "needs-save");
        } else {
            gtk_label_set_text(GTK_LABEL(lbl_status), "Status: Failed to save session.");
        }
        g_free(path); g_object_unref(file);
    }
}

static void on_save_session_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Save Looper Session");
    gtk_file_dialog_set_initial_name(dialog, "New_Session.qjams");
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "QJams Session (*.qjams)");
    gtk_file_filter_add_pattern(filter, "*.qjams");
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, filter);
    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
    gtk_file_dialog_set_default_filter(dialog, filter);
    if (strlen(ui_state.config.recordings_dir) > 0) {
        GFile *initial_folder = g_file_new_for_path(ui_state.config.recordings_dir);
        gtk_file_dialog_set_initial_folder(dialog, initial_folder);
        g_object_unref(initial_folder);
    }
    g_object_unref(filter); g_object_unref(filters);
    gtk_file_dialog_save(dialog, GTK_WINDOW(user_data), NULL, on_save_session_file_chosen, NULL);
}

static void on_load_session_file_chosen(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    (void)user_data;
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source_object);
    GError *error = NULL;
    GFile *file = gtk_file_dialog_open_finish(dialog, res, &error);
    if (file) {
        char *path = g_file_get_path(file);
        on_stop_clicked(NULL, NULL);
        if (load_qjams_session(path) == 0) {
            refresh_looper_layers_ui();
            update_looper_status_ui();
            invalidate_waveform_caches();
            gtk_widget_queue_draw(waveform_area_bt);
            gtk_widget_queue_draw(waveform_area_input);

            char *basename = g_path_get_basename(path);
            char status[512];
            snprintf(status, sizeof(status), "Status: Session '%s' loaded", basename);
            gtk_label_set_text(GTK_LABEL(lbl_status), status);

            if (!atomic_load_explicit(&is_looper_mode, memory_order_acquire)) {
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(btn_looper_mode), TRUE);
            }

            int active_rate = atomic_load_explicit(&active_sample_rate, memory_order_acquire);
            size_t total_frames = atomic_load_explicit(&backing_track_frames, memory_order_acquire);
            int total_secs = (active_rate > 0) ? (total_frames / active_rate) : 0;

            char ui_text[600];
            snprintf(ui_text, sizeof(ui_text), "Track: %s [%02d:%02d]", basename, total_secs / 60, total_secs % 60);
            gtk_label_set_text(GTK_LABEL(lbl_track), ui_text);
            update_zoom_button_label_to_length();
            g_free(basename);
        } else {
            gtk_label_set_text(GTK_LABEL(lbl_status), "Status: Failed to load session (Invalid file or RAM exceeded).");
        }
        g_free(path); g_object_unref(file);
    }
}

static void on_load_session_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Load Looper Session");
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "QJams Session (*.qjams)");
    gtk_file_filter_add_pattern(filter, "*.qjams");
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, filter);
    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
    gtk_file_dialog_set_default_filter(dialog, filter);
    if (strlen(ui_state.config.recordings_dir) > 0) {
        GFile *initial_folder = g_file_new_for_path(ui_state.config.recordings_dir);
        gtk_file_dialog_set_initial_folder(dialog, initial_folder);
        g_object_unref(initial_folder);
    }
    g_object_unref(filter); g_object_unref(filters);
    gtk_file_dialog_open(dialog, GTK_WINDOW(user_data), NULL, on_load_session_file_chosen, NULL);
}

static void on_looper_mode_toggled(GtkToggleButton *button, gpointer user_data) {
    (void)user_data;
    bool active = gtk_toggle_button_get_active(button);
    atomic_store_explicit(&is_looper_mode, active, memory_order_release);
    ui_state.config.looper_mode_active = active ? 1 : 0;
    save_qjams_config(ui_state.config_path, &ui_state.config);

    // FIX: Adjust canvas FIRST so memory state is correct before UI queries it
    adjust_blank_canvas_for_mode(active);

    if (active) {
        gtk_stack_set_visible_child_name(GTK_STACK(mode_stack), "looper_page");
        update_looper_status_ui();
    } else {
        gtk_stack_set_visible_child_name(GTK_STACK(mode_stack), "video_page");
    }

    // FIX: Disable Save Session button in Normal mode (not applicable)
    if (btn_save_session) gtk_widget_set_sensitive(btn_save_session, active);
}

static void on_load_playlist_file_chosen(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    (void)user_data;
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source_object);
    GError *error = NULL;
    GFile *file = gtk_file_dialog_open_finish(dialog, res, &error);
    if (file) {
        char *path = g_file_get_path(file);
        strncpy(ui_state.playlist_path, path, sizeof(ui_state.playlist_path) - 1);
        load_playlist_from_file(path);
        g_free(path);
        g_object_unref(file);
    }
}

static void on_load_playlist_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Load Playlist");
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "M3U Playlists (*.m3u)");
    gtk_file_filter_add_pattern(filter, "*.m3u");
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, filter);
    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
    gtk_file_dialog_set_default_filter(dialog, filter);

    if (strlen(ui_state.config.recordings_dir) > 0) {
        GFile *initial = g_file_new_for_path(ui_state.config.recordings_dir);
        gtk_file_dialog_set_initial_folder(dialog, initial);
        g_object_unref(initial);
    }

    gtk_file_dialog_open(dialog, GTK_WINDOW(user_data), NULL, on_load_playlist_file_chosen, NULL);
    g_object_unref(filter); g_object_unref(filters);
}

static void on_save_playlist_file_chosen(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    (void)user_data;
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source_object);
    GError *error = NULL;
    GFile *file = gtk_file_dialog_save_finish(dialog, res, &error);
    if (file) {
        char *path = g_file_get_path(file);
        if (!g_str_has_suffix(path, ".m3u")) {
            char *tmp = g_strdup_printf("%s.m3u", path);
            g_free(path);
            path = tmp;
        }
        strncpy(ui_state.playlist_path, path, sizeof(ui_state.playlist_path) - 1);
        save_current_playlist();
        gtk_label_set_text(GTK_LABEL(lbl_status), "Status: Playlist Saved");
        g_free(path);
        g_object_unref(file);
    }
}

static void on_save_playlist_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Save Playlist");
    gtk_file_dialog_set_initial_name(dialog, "MySetlist.m3u");
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "M3U Playlists (*.m3u)");
    gtk_file_filter_add_pattern(filter, "*.m3u");
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, filter);
    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
    gtk_file_dialog_set_default_filter(dialog, filter);

    if (strlen(ui_state.config.recordings_dir) > 0) {
        GFile *initial = g_file_new_for_path(ui_state.config.recordings_dir);
        gtk_file_dialog_set_initial_folder(dialog, initial);
        g_object_unref(initial);
    }

    gtk_file_dialog_save(dialog, GTK_WINDOW(user_data), NULL, on_save_playlist_file_chosen, NULL);
    g_object_unref(filter); g_object_unref(filters);
}

void ui_command_post_init(GtkBuilder *builder, GtkWindow *window) {
    btn_load = GTK_WIDGET(gtk_builder_get_object(builder, "btn_load"));
    g_signal_connect(btn_load, "clicked", G_CALLBACK(on_select_track_clicked), window);

    GtkWidget *btn_load_session = GTK_WIDGET(gtk_builder_get_object(builder, "btn_load_session"));
    g_signal_connect(btn_load_session, "clicked", G_CALLBACK(on_load_session_clicked), window);

    // NEW: Map the Playlist buttons
    GtkWidget *btn_load_playlist = GTK_WIDGET(gtk_builder_get_object(builder, "btn_load_playlist"));
    g_signal_connect(btn_load_playlist, "clicked", G_CALLBACK(on_load_playlist_clicked), window);

    GtkWidget *btn_save_playlist = GTK_WIDGET(gtk_builder_get_object(builder, "btn_save_playlist"));
    g_signal_connect(btn_save_playlist, "clicked", G_CALLBACK(on_save_playlist_clicked), window);

    btn_save_mux = GTK_WIDGET(gtk_builder_get_object(builder, "btn_save_mux"));
    g_signal_connect(btn_save_mux, "clicked", G_CALLBACK(on_save_mux_clicked), NULL);

    btn_save_session = GTK_WIDGET(gtk_builder_get_object(builder, "btn_save_session"));
    g_signal_connect(btn_save_session, "clicked", G_CALLBACK(on_save_session_clicked), window);

    btn_settings = GTK_WIDGET(gtk_builder_get_object(builder, "btn_settings"));
    g_signal_connect(btn_settings, "clicked", G_CALLBACK(on_settings_clicked), window);

    // Map the mode_stack globally before evaluating the toggle state so the page flip succeeds on boot
    mode_stack = GTK_WIDGET(gtk_builder_get_object(builder, "mode_stack"));

    btn_looper_mode = GTK_WIDGET(gtk_builder_get_object(builder, "btn_looper_mode"));
    g_signal_connect(btn_looper_mode, "toggled", G_CALLBACK(on_looper_mode_toggled), NULL);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(btn_looper_mode), ui_state.config.looper_mode_active != 0);

    lbl_status = GTK_WIDGET(gtk_builder_get_object(builder, "lbl_status"));
    main_spinner = GTK_WIDGET(gtk_builder_get_object(builder, "main_spinner")); // Map the spinner
    lbl_roadmap = GTK_WIDGET(gtk_builder_get_object(builder, "lbl_roadmap"));
    if (lbl_roadmap) {
        gtk_label_set_markup(GTK_LABEL(lbl_roadmap),
                             "<span size='small' foreground='#888888'>QJams is (c) 2026 Rob Wijhenke.\nThis beta version is intended for testing purposes only.</span>");
    }
}
