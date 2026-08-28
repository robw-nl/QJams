#include "ui_command_post.h"
#include "ui_globals.h"
#include "audio_engine.h"
#include "ui_multitrack.h"
#include "ui_dashboard.h"
#include "ui_settings.h"
#include "ui_playlist.h"
#include "session.h"
#include "ui_waveforms.h"
#include <sndfile.h>
#include <string.h>

GPid active_muxer_pid = 0;
static char temp_wav_path[1024] = ""; // Track the ephemeral WAV file for cleanup

static void on_multitrack_mode_toggled(GtkToggleButton *button, gpointer user_data);
static GtkWidget *btn_load_session = NULL;

static void on_file_chosen(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    (void)user_data;
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source_object);
    GError *error = NULL;
    GFile *file = gtk_file_dialog_open_finish(dialog, res, &error);

    if (file) {
        char *path = g_file_get_path(file);
        char *dir = NULL;

        GFile *parent = g_file_get_parent(file);
        if (parent) {
            dir = g_file_get_path(parent);
            g_object_unref(parent);
        }

        if (g_str_has_suffix(path, ".m3u")) {
            load_playlist_from_file(path);
        } else {
            if (atomic_load_explicit(&is_multitrack_mode, memory_order_acquire)) {
                int rec_track = atomic_load_explicit(&current_recording_track, memory_order_acquire);
                multitrack_import_file_async(path, rec_track);
            } else {
                strncpy(ui_state.selected_track_path, path, sizeof(ui_state.selected_track_path) - 1);
                ui_state.selected_track_path[sizeof(ui_state.selected_track_path) - 1] = '\0';
                trigger_track_load();
            }
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

static void execute_load_track_dialog(GtkWidget *window) {
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Select Media Track");

    if (strlen(ui_state.config.last_track_dir) > 0) {
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
    gtk_spinner_stop(GTK_SPINNER(main_spinner));

    if (status == 0) gtk_label_set_text(GTK_LABEL(lbl_status), "Status: Muxing Complete!");
    else gtk_label_set_text(GTK_LABEL(lbl_status), "Status: Muxing Failed (FFmpeg Error)");
    gtk_widget_set_sensitive(btn_save_mux, TRUE);

    // Wipe the temporary audio file from the /tmp partition
    if (strlen(temp_wav_path) > 0) {
        remove(temp_wav_path);
        temp_wav_path[0] = '\0';
    }
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

    char base_no_ext[512];
    strncpy(base_no_ext, basename_ext, sizeof(base_no_ext) - 1);
    base_no_ext[sizeof(base_no_ext) - 1] = '\0';

    char *dot = strrchr(base_no_ext, '.');
    if (dot) *dot = '\0';

    size_t len = strlen(base_no_ext);
    if (len > 4 && strcmp(base_no_ext + len - 4, "-MIX") == 0) {
        base_no_ext[len - 4] = '\0';
    }

    bool is_multitrack = atomic_load_explicit(&is_multitrack_mode, memory_order_acquire);
    char raw_path[1024];
    char mix_path[1024];
    snprintf(raw_path, sizeof(raw_path), "%s/%s-RAW.mkv", dir, base_no_ext);
    snprintf(mix_path, sizeof(mix_path), "%s/%s-MIX.mkv", dir, base_no_ext);

    // Write to the tracked global string so it can be deleted later
    snprintf(temp_wav_path, sizeof(temp_wav_path), "/tmp/%s-RAW.wav", base_no_ext);

    time_t t = time(NULL);
    struct tm tm = *localtime(&t);
    char date_str[32];
    snprintf(date_str, sizeof(date_str), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);

    gtk_label_set_text(GTK_LABEL(lbl_status), "Status: Exporting Audio/Video...");
    gtk_spinner_start(GTK_SPINNER(main_spinner));

    size_t start_f = 0;
    size_t end_f = atomic_load_explicit(&backing_track_frames, memory_order_acquire);

    if (atomic_load_explicit(&loop_active, memory_order_acquire)) {
        start_f = atomic_load_explicit(&loop_start_frame, memory_order_acquire);
        end_f = atomic_load_explicit(&loop_end_frame, memory_order_acquire);
    }

    if (is_multitrack) {
        SF_INFO sfinfo = {0};
        sfinfo.channels = 2;
        sfinfo.samplerate = atomic_load_explicit(&active_sample_rate, memory_order_acquire);
        sfinfo.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;

        SNDFILE *outfile = sf_open(temp_wav_path, SFM_WRITE, &sfinfo);
        size_t frames_to_write = end_f - start_f;

        if (outfile && frames_to_write > 0) {
            float *mix_buf = NULL;

            // Delegate the heavy lifting to the mathematically accurate unified DSP engine
            if (await_rt_thread_detach()) {
                extern float* render_mixdown_region(size_t, size_t);
                mix_buf = render_mixdown_region(start_f, end_f);
                resume_rt_thread();
            }

            if (mix_buf) {
                sf_writef_float(outfile, mix_buf, frames_to_write);
                free(mix_buf);
            }
            sf_close(outfile);

            char *argv[64];
            int argc = 0;
            argv[argc++] = "ffmpeg"; argv[argc++] = "-y";
            argv[argc++] = "-i"; argv[argc++] = temp_wav_path;

            char *meta_title = g_strdup_printf("title=%s", base_no_ext);
            char *meta_date = g_strdup_printf("recordingdate=%s", date_str);
            char *meta_comment = g_strdup("comment=Recorded with QJams. (c) Rob Wijhenke. https://sites.google.com/view/qjams");

            argv[argc++] = "-metadata"; argv[argc++] = meta_title;
            argv[argc++] = "-metadata"; argv[argc++] = meta_date;
            argv[argc++] = "-metadata"; argv[argc++] = meta_comment;

            char mix_path_flac[1024];
            char mix_path_wav[1024];
            snprintf(mix_path_flac, sizeof(mix_path_flac), "%s/%s-MIX.flac", dir, base_no_ext);
            snprintf(mix_path_wav, sizeof(mix_path_wav), "%s/%s-MIX.wav", dir, base_no_ext);

            int fmt = ui_state.config.export_format;
            if (fmt == 0 || fmt == 2) {
                argv[argc++] = "-c:a"; argv[argc++] = "flac";
                argv[argc++] = mix_path_flac;
            }
            if (fmt == 1 || fmt == 2) {
                argv[argc++] = "-c:a"; argv[argc++] = "pcm_s16le";
                argv[argc++] = mix_path_wav;
            }
            argv[argc++] = NULL;

            GError *spawn_err = NULL;
            if (g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL, &active_muxer_pid, &spawn_err)) {
                g_child_watch_add(active_muxer_pid, on_muxer_finished, NULL);
                gtk_label_set_text(GTK_LABEL(lbl_status), "Status: Saved, Tagging Audio...");
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
            gtk_label_set_text(GTK_LABEL(lbl_status), "Status: Failed to open raw output for writing");
            gtk_spinner_stop(GTK_SPINNER(main_spinner));
            gtk_widget_set_sensitive(btn_save_mux, TRUE);
        }
        g_free(dir); g_free(basename_ext); g_free(chosen_path);
        return;
    }

    char *argv[64];
    int argc = 0;
    argv[argc++] = "ffmpeg"; argv[argc++] = "-y";

    argv[argc++] = "-i"; argv[argc++] = current_raw_path;

    char *meta_title = g_strdup_printf("title=%s", base_no_ext);
    char *meta_date = g_strdup_printf("recordingdate=%s", date_str);
    char *meta_comment = g_strdup("comment=Recorded with QJams. (c) Rob Wijhenke. https://sites.google.com/view/qjams");

    // We no longer need to apply volume or merge tracks, as they were mixed perfectly in real-time.
    // Copy the exact unified recording as the RAW archive
    argv[argc++] = "-map"; argv[argc++] = "0:v?";
    argv[argc++] = "-map"; argv[argc++] = "0:a:0?";
    argv[argc++] = "-c"; argv[argc++] = "copy";
    argv[argc++] = "-metadata"; argv[argc++] = meta_title;
    argv[argc++] = "-metadata"; argv[argc++] = meta_date;
    argv[argc++] = "-metadata"; argv[argc++] = meta_comment;
    argv[argc++] = raw_path;

    // Generate the optimized MIX file with FLAC compression
    argv[argc++] = "-map"; argv[argc++] = "0:v?";
    argv[argc++] = "-map"; argv[argc++] = "0:a:0?";
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

    gtk_widget_set_sensitive(btn_save_mux, FALSE);

    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Export Mix");

    if (atomic_load_explicit(&is_multitrack_mode, memory_order_acquire)) {
        if (ui_state.config.export_format == 1) {
            gtk_file_dialog_set_initial_name(dialog, "MySong-MIX.wav");
        } else {
            gtk_file_dialog_set_initial_name(dialog, "MySong-MIX.flac");
        }
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
            strncpy(ui_state.selected_track_path, path, sizeof(ui_state.selected_track_path) - 1);
            ui_state.selected_track_path[sizeof(ui_state.selected_track_path) - 1] = '\0';
            update_playlist_toggle_state();

            char *basename = g_path_get_basename(path);
            char status[512];
            snprintf(status, sizeof(status), "Status: Session saved to %s", basename);
            gtk_label_set_text(GTK_LABEL(lbl_status), status);
            g_free(basename);

            gtk_widget_remove_css_class(btn_save_mux, "needs-save");
            gtk_widget_remove_css_class(btn_save_session, "needs-save");

            ui_state.is_existing_session = true;
            ui_state.session_is_dirty = false;
        } else {
            gtk_label_set_text(GTK_LABEL(lbl_status), "Status: Failed to save session.");
        }
        g_free(path); g_object_unref(file);
    }
}

static void on_save_session_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Save Multi-Track Session");
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

static gboolean deferred_session_redraw(gpointer user_data) {
    (void)user_data;
    extern void invalidate_waveform_caches(void);
    extern GtkWidget *waveform_area_bt;
    extern GtkWidget *waveform_area_input;

    invalidate_waveform_caches();
    if (waveform_area_bt) gtk_widget_queue_draw(waveform_area_bt);
    if (waveform_area_input) gtk_widget_queue_draw(waveform_area_input);

    return G_SOURCE_REMOVE; // Ensures it only fires once
}

static void load_session_thread(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable) {
    (void)source_object; (void)cancellable;
    char *path = (char *)task_data;
    int result = load_qjams_session(path);
    g_task_return_int(task, result);
}

static void load_session_ready(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    (void)source_object; (void)user_data;
    GError *error = NULL;
    int load_status = g_task_propagate_int(G_TASK(res), &error);
    char *path = (char *)g_task_get_task_data(G_TASK(res));

    gtk_spinner_stop(GTK_SPINNER(main_spinner));

    if (load_status >= 0) {
        ui_state.is_existing_session = true;
        ui_state.session_is_dirty = false; // Freshly loaded from disk, so it's clean

        // 1. Force Engine into Multitrack Mode FIRST to ensure cache builder reads correct flags
        if (!atomic_load_explicit(&is_multitrack_mode, memory_order_acquire)) {
            atomic_store_explicit(&is_multitrack_mode, true, memory_order_release);

            g_signal_handlers_block_by_func(btn_multitrack_mode, G_CALLBACK(on_multitrack_mode_toggled), NULL);
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(btn_multitrack_mode), TRUE);
            g_signal_handlers_unblock_by_func(btn_multitrack_mode, G_CALLBACK(on_multitrack_mode_toggled), NULL);

            extern GtkWidget *mode_stack;
            gtk_stack_set_visible_child_name(GTK_STACK(mode_stack), "multitrack_page");
        }

        strncpy(ui_state.selected_track_path, path, sizeof(ui_state.selected_track_path) - 1);
        ui_state.selected_track_path[sizeof(ui_state.selected_track_path) - 1] = '\0';
        update_playlist_toggle_state();

        // 2. Refresh UI elements
        refresh_multitrack_tracks_ui();
        extern void update_multitrack_status_ui(void);
        update_multitrack_status_ui();

        char *basename = g_path_get_basename(path);
        char status[512];
        if (load_status == 1) {
            snprintf(status, sizeof(status), "Status: Session '%s' loaded (WARNING: Truncated File)", basename);
        } else {
            snprintf(status, sizeof(status), "Status: Session '%s' loaded", basename);
        }
        gtk_label_set_text(GTK_LABEL(lbl_status), status);

        int active_rate = atomic_load_explicit(&active_sample_rate, memory_order_acquire);
        size_t total_frames = atomic_load_explicit(&backing_track_frames, memory_order_acquire);
        int total_secs = (active_rate > 0) ? (total_frames / active_rate) : 0;

        char ui_text[600];
        snprintf(ui_text, sizeof(ui_text), "Track: %s [%02d:%02d]", basename, total_secs / 60, total_secs % 60);
        gtk_label_set_text(GTK_LABEL(lbl_track), ui_text);

        extern void update_zoom_button_label_to_length(void);
        update_zoom_button_label_to_length();
        g_free(basename);

        gtk_widget_set_sensitive(btn_play, TRUE);
        gtk_widget_set_sensitive(btn_record, TRUE);

        // 3. Defer the redraw to the GTK idle loop to guarantee all states have settled
        g_idle_add(deferred_session_redraw, NULL);

    } else {
        gtk_label_set_text(GTK_LABEL(lbl_status), "Status: Failed to load session (Invalid file or RAM exceeded).");
        gtk_label_set_text(GTK_LABEL(lbl_track), "Track: None Selected");
    }

    gtk_widget_set_sensitive(btn_load, TRUE);
    gtk_widget_set_sensitive(btn_prev, TRUE);
    gtk_widget_set_sensitive(btn_next, TRUE);
    gtk_widget_set_sensitive(btn_playlist_toggle, TRUE);

    if (btn_load_session) gtk_widget_set_sensitive(btn_load_session, TRUE);
}

void command_post_load_session(const char *path) {
    on_stop_clicked(NULL, NULL);

    // 1. SAFELY DISARM THE AUDIO ENGINE
    // Drop the active frame count and track flags to 0 immediately.
    // This stops the JACK real-time thread from reading the memory buffers
    // while the background thread is busy freeing and reallocating them.
    extern _Atomic size_t backing_track_frames;
    atomic_store_explicit(&backing_track_frames, 0, memory_order_release);

    for (int i = 0; i < 12; i++) {
        atomic_store_explicit(&master_tracks[i].has_audio, false, memory_order_release);
    }

    gtk_spinner_start(GTK_SPINNER(main_spinner));
    gtk_label_set_text(GTK_LABEL(lbl_status), "Status: Extracting session to RAM...");
    gtk_label_set_text(GTK_LABEL(lbl_track), "Track: Loading...");

    gtk_widget_set_sensitive(btn_load, FALSE);
    gtk_widget_set_sensitive(btn_prev, FALSE);
    gtk_widget_set_sensitive(btn_next, FALSE);
    gtk_widget_set_sensitive(btn_play, FALSE);
    gtk_widget_set_sensitive(btn_record, FALSE);
    gtk_widget_set_sensitive(btn_playlist_toggle, FALSE);

    if (btn_load_session) gtk_widget_set_sensitive(btn_load_session, FALSE);

    GTask *task = g_task_new(NULL, NULL, load_session_ready, NULL);
    g_task_set_task_data(task, g_strdup(path), g_free);
    g_task_run_in_thread(task, load_session_thread);
    g_object_unref(task);
}

static void on_load_session_file_chosen(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    (void)user_data;
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source_object);
    GError *error = NULL;
    GFile *file = gtk_file_dialog_open_finish(dialog, res, &error);
    if (file) {
        char *path = g_file_get_path(file);
        command_post_load_session(path);
        g_free(path);
        g_object_unref(file);
    }
}

static void on_load_track_overwrite_confirm(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    GtkAlertDialog *alert = GTK_ALERT_DIALOG(source_object);
    if (gtk_alert_dialog_choose_finish(alert, res, NULL) == 0) {
        execute_load_track_dialog(GTK_WIDGET(user_data));
    }
}

void on_select_track_clicked(GtkButton *button, gpointer window) {
    (void)button;
    bool is_multitrack = atomic_load_explicit(&is_multitrack_mode, memory_order_acquire);

    // Destructive only if NOT in multitrack mode (multitrack safely imports as a layer)
    if (!is_multitrack && ui_state.session_is_dirty && pristine_frames > 0) {
        GtkAlertDialog *alert = gtk_alert_dialog_new("Discard Active Recording?");
        gtk_alert_dialog_set_detail(alert, "Loading a new track will discard your current unsaved recording. Continue?");
        const char *buttons[] = { "Discard & Load", "Cancel", NULL };
        gtk_alert_dialog_set_buttons(alert, buttons);
        gtk_alert_dialog_set_cancel_button(alert, 1);
        gtk_alert_dialog_set_default_button(alert, 1);
        gtk_alert_dialog_choose(alert, GTK_WINDOW(window), NULL, on_load_track_overwrite_confirm, window);
    } else {
        execute_load_track_dialog(GTK_WIDGET(window));
    }
}

// --- LOAD SESSION INTERCEPTION ---
static void execute_load_session_dialog(GtkWidget *window) {
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Load Multi-Track Session");
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
    gtk_file_dialog_open(dialog, GTK_WINDOW(window), NULL, on_load_session_file_chosen, NULL);
}

static void on_load_session_overwrite_confirm(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    GtkAlertDialog *alert = GTK_ALERT_DIALOG(source_object);
    if (gtk_alert_dialog_choose_finish(alert, res, NULL) == 0) {
        execute_load_session_dialog(GTK_WIDGET(user_data));
    }
}

static void on_load_session_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    if (ui_state.session_is_dirty && pristine_frames > 0) {
        GtkAlertDialog *alert = gtk_alert_dialog_new("Overwrite Active Session?");
        gtk_alert_dialog_set_detail(alert, "Loading a new session will discard your currently recorded, unsaved audio. Continue?");
        const char *buttons[] = { "Discard & Load", "Cancel", NULL };
        gtk_alert_dialog_set_buttons(alert, buttons);
        gtk_alert_dialog_set_cancel_button(alert, 1);
        gtk_alert_dialog_set_default_button(alert, 1);
        gtk_alert_dialog_choose(alert, GTK_WINDOW(user_data), NULL, on_load_session_overwrite_confirm, user_data);
    } else {
        execute_load_session_dialog(GTK_WIDGET(user_data));
    }
}

static void on_multitrack_mode_toggled(GtkToggleButton *button, gpointer user_data) {
    (void)user_data;
    bool active = gtk_toggle_button_get_active(button);
    atomic_store_explicit(&is_multitrack_mode, active, memory_order_release);
    ui_state.config.multitrack_mode_active = active ? 1 : 0;
    save_qjams_config(ui_state.config_path, &ui_state.config);

    adjust_blank_canvas_for_mode(active);

    if (active) {
        gtk_stack_set_visible_child_name(GTK_STACK(mode_stack), "multitrack_page");
        update_multitrack_status_ui();
    } else {
        gtk_stack_set_visible_child_name(GTK_STACK(mode_stack), "video_page");
    }

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
        strncpy(ui_state.config.last_playlist_path, path, sizeof(ui_state.config.last_playlist_path) - 1);
        save_qjams_config(ui_state.config_path, &ui_state.config);
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

    char *dir = g_path_get_dirname(ui_state.playlist_path);
    if (dir) {
        GFile *initial = g_file_new_for_path(dir);
        gtk_file_dialog_set_initial_folder(dialog, initial);
        g_object_unref(initial);
        g_free(dir);
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
        strncpy(ui_state.config.last_playlist_path, path, sizeof(ui_state.config.last_playlist_path) - 1);
        save_qjams_config(ui_state.config_path, &ui_state.config);
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

    char *basename = g_path_get_basename(ui_state.playlist_path);
    gtk_file_dialog_set_initial_name(dialog, basename);
    g_free(basename);

    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "M3U Playlists (*.m3u)");
    gtk_file_filter_add_pattern(filter, "*.m3u");
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, filter);
    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
    gtk_file_dialog_set_default_filter(dialog, filter);

    char *dir = g_path_get_dirname(ui_state.playlist_path);
    if (dir) {
        GFile *initial = g_file_new_for_path(dir);
        gtk_file_dialog_set_initial_folder(dialog, initial);
        g_object_unref(initial);
        g_free(dir);
    }

    gtk_file_dialog_save(dialog, GTK_WINDOW(user_data), NULL, on_save_playlist_file_chosen, NULL);
    g_object_unref(filter); g_object_unref(filters);
}

void ui_command_post_init(GtkBuilder *b_cmd, GtkBuilder *b_stack, GtkWindow *window) {
    btn_load = GTK_WIDGET(gtk_builder_get_object(b_cmd, "btn_load"));
    g_signal_connect(btn_load, "clicked", G_CALLBACK(on_select_track_clicked), window);

    btn_load_session = GTK_WIDGET(gtk_builder_get_object(b_cmd, "btn_load_session"));
    g_signal_connect(btn_load_session, "clicked", G_CALLBACK(on_load_session_clicked), window);

    GtkWidget *btn_load_playlist = GTK_WIDGET(gtk_builder_get_object(b_cmd, "btn_load_playlist"));
    g_signal_connect(btn_load_playlist, "clicked", G_CALLBACK(on_load_playlist_clicked), window);

    GtkWidget *btn_save_playlist = GTK_WIDGET(gtk_builder_get_object(b_cmd, "btn_save_playlist"));
    g_signal_connect(btn_save_playlist, "clicked", G_CALLBACK(on_save_playlist_clicked), window);

    btn_save_mux = GTK_WIDGET(gtk_builder_get_object(b_cmd, "btn_save_mux"));
    g_signal_connect(btn_save_mux, "clicked", G_CALLBACK(on_save_mux_clicked), NULL);

    btn_save_session = GTK_WIDGET(gtk_builder_get_object(b_cmd, "btn_save_session"));
    g_signal_connect(btn_save_session, "clicked", G_CALLBACK(on_save_session_clicked), window);

    btn_settings = GTK_WIDGET(gtk_builder_get_object(b_cmd, "btn_settings"));
    g_signal_connect(btn_settings, "clicked", G_CALLBACK(on_settings_clicked), window);

    mode_stack = GTK_WIDGET(gtk_builder_get_object(b_stack, "mode_stack"));

    btn_multitrack_mode = GTK_WIDGET(gtk_builder_get_object(b_cmd, "btn_multitrack_mode"));
    g_signal_connect(btn_multitrack_mode, "toggled", G_CALLBACK(on_multitrack_mode_toggled), NULL);

    g_signal_handlers_block_by_func(btn_multitrack_mode, G_CALLBACK(on_multitrack_mode_toggled), NULL);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(btn_multitrack_mode), ui_state.config.multitrack_mode_active != 0);
    g_signal_handlers_unblock_by_func(btn_multitrack_mode, G_CALLBACK(on_multitrack_mode_toggled), NULL);

    atomic_store_explicit(&is_multitrack_mode, ui_state.config.multitrack_mode_active != 0, memory_order_release);

    if (ui_state.config.multitrack_mode_active != 0) {
        gtk_stack_set_visible_child_name(GTK_STACK(mode_stack), "multitrack_page");
    } else {
        gtk_stack_set_visible_child_name(GTK_STACK(mode_stack), "video_page");
    }

    lbl_status = GTK_WIDGET(gtk_builder_get_object(b_cmd, "lbl_status"));
    main_spinner = GTK_WIDGET(gtk_builder_get_object(b_cmd, "main_spinner"));
    lbl_roadmap = GTK_WIDGET(gtk_builder_get_object(b_cmd, "lbl_roadmap"));
    if (lbl_roadmap) {
        gtk_label_set_markup(GTK_LABEL(lbl_roadmap),
                             "<span size='small' foreground='#888888'>QJams is (c) 2026 Rob Wijhenke. All rights reserved.\n"
                             "This beta version is intended for testing purposes only.\n\n"
                             "Visit <a href='https://sites.google.com/view/qjams'>https://sites.google.com/view/qjams</a></span>");
    }
}
