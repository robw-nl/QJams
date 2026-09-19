#include "config.h"
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
static char temp_wav_path[1024] = "";
static GtkWidget *btn_load_session = NULL;
_Atomic bool video_is_armed = false;

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
            int rec_track = atomic_load_explicit(&current_recording_track, memory_order_acquire);
            multitrack_import_file_async(path, rec_track);
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

void on_select_track_clicked(GtkButton *button, gpointer window) {
    (void)button;
    execute_load_track_dialog(GTK_WIDGET(window));
}

static void on_muxer_finished(GPid pid, gint status, gpointer user_data) {
    (void)user_data;
    g_spawn_close_pid(pid);
    active_muxer_pid = 0;
    gtk_spinner_stop(GTK_SPINNER(main_spinner));

    if (status == 0) log_and_notify(1, "Status: Muxing Complete!");
    else log_and_notify(1, "Status: Muxing Failed (FFmpeg Error)");
    gtk_widget_set_sensitive(btn_save_mux, TRUE);

    if (strlen(temp_wav_path) > 0) {
        remove(temp_wav_path);
        temp_wav_path[0] = '\0';
    }
}

typedef struct {
    char dir[1024];
    char base_no_ext[1024];
    char date_str[32];
    char temp_wav_path[1024];
    size_t start_f;
    size_t end_f;
    int samplerate;
    bool has_video;
} ExportTaskData;

static void export_mixdown_thread(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable) {
    (void)source_object; (void)cancellable;
    ExportTaskData *data = (ExportTaskData *)task_data;

    // Only render the massive audio payload if no video track overrides it
    if (!data->has_video) {
        SF_INFO sfinfo = {0};
        sfinfo.channels = 2;
        sfinfo.samplerate = data->samplerate;
        sfinfo.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;

        SNDFILE *outfile = sf_open(data->temp_wav_path, SFM_WRITE, &sfinfo);
        size_t frames_to_write = data->end_f - data->start_f;

        if (outfile && frames_to_write > 0) {
            float *mix_buf = NULL;

            // Wait for RT to cleanly detach, protecting the pointers
            if (await_rt_thread_detach()) {
                extern float* render_mixdown_region(size_t, size_t);
                mix_buf = render_mixdown_region(data->start_f, data->end_f);
                resume_rt_thread();
            }

            if (mix_buf) {
                sf_writef_float(outfile, mix_buf, frames_to_write);
                free(mix_buf);
                sf_close(outfile);
                g_task_return_boolean(task, TRUE);
                return;
            }
            sf_close(outfile);
        } else {
            if (outfile) sf_close(outfile);
        }
        g_task_return_boolean(task, FALSE);
    } else {
        g_task_return_boolean(task, TRUE); // Video mode requires no internal prep
    }
}

static void export_mixdown_ready(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    (void)source_object; (void)user_data;
    GError *error = NULL;
    bool success = g_task_propagate_boolean(G_TASK(res), &error);
    ExportTaskData *data = (ExportTaskData *)g_task_get_task_data(G_TASK(res));

    // 1. Unlock the UI's master container
    GtkWindow *win = gtk_application_get_active_window(GTK_APPLICATION(g_application_get_default()));
    if (win) {
        GtkWidget *master_box = gtk_window_get_child(win);
        if (master_box) gtk_widget_set_sensitive(master_box, TRUE);
    }

    extern GtkWidget *btn_save_mux;

    if (!success) {
        extern GtkWidget *main_spinner;
        if (main_spinner) {
            gtk_spinner_stop(GTK_SPINNER(main_spinner));
            gtk_widget_queue_draw(main_spinner); // Clean up the frozen spinner frame
        }
        if (btn_save_mux) gtk_widget_set_sensitive(btn_save_mux, TRUE);
        log_and_notify(1, "Status: Failed to render audio mixdown");
        set_app_waiting_state(false);
        return;
    }

    char raw_path[1024];
    char mix_path[1024];
    snprintf(raw_path, sizeof(raw_path), "%s/%s-RAW.mkv", data->dir, data->base_no_ext);
    snprintf(mix_path, sizeof(mix_path), "%s/%s-MIX.mkv", data->dir, data->base_no_ext);

    char *argv[64];
    int argc = 0;
    argv[argc++] = "ffmpeg"; argv[argc++] = "-y";

    char *meta_title = g_strdup_printf("title=%s", data->base_no_ext);
    char *meta_date = g_strdup_printf("recordingdate=%s", data->date_str);
    char *meta_comment = g_strdup("comment=Recorded with QJams. (c) Rob Wijhenke. https://sites.google.com/view/qjams");

    if (!data->has_video) {
        argv[argc++] = "-i"; argv[argc++] = data->temp_wav_path;
        argv[argc++] = "-metadata"; argv[argc++] = meta_title;
        argv[argc++] = "-metadata"; argv[argc++] = meta_date;
        argv[argc++] = "-metadata"; argv[argc++] = meta_comment;

        char mix_path_flac[1024];
        char mix_path_wav[1024];
        snprintf(mix_path_flac, sizeof(mix_path_flac), "%s/%s-MIX.flac", data->dir, data->base_no_ext);
        snprintf(mix_path_wav, sizeof(mix_path_wav), "%s/%s-MIX.wav", data->dir, data->base_no_ext);

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
    } else {
        extern char current_raw_path[1024];
        argv[argc++] = "-i"; argv[argc++] = current_raw_path;

        argv[argc++] = "-map"; argv[argc++] = "0:v:0";
        argv[argc++] = "-map"; argv[argc++] = "0:a";
        argv[argc++] = "-c:v"; argv[argc++] = "copy";
        argv[argc++] = "-c:a"; argv[argc++] = "pcm_s16le";
        argv[argc++] = "-metadata"; argv[argc++] = meta_title;
        argv[argc++] = "-metadata"; argv[argc++] = meta_date;
        argv[argc++] = "-metadata"; argv[argc++] = meta_comment;
        argv[argc++] = raw_path;

        argv[argc++] = "-map"; argv[argc++] = "0:v:0";
        argv[argc++] = "-map"; argv[argc++] = "0:a:0";
        argv[argc++] = "-c:v"; argv[argc++] = "copy";
        argv[argc++] = "-c:a"; argv[argc++] = "flac";
        argv[argc++] = "-metadata"; argv[argc++] = meta_title;
        argv[argc++] = "-metadata"; argv[argc++] = meta_date;
        argv[argc++] = "-metadata"; argv[argc++] = meta_comment;
        argv[argc++] = mix_path;

        char mix_path_flac[1024];
        char mix_path_wav[1024];
        snprintf(mix_path_flac, sizeof(mix_path_flac), "%s/%s-MIX.flac", data->dir, data->base_no_ext);
        snprintf(mix_path_wav, sizeof(mix_path_wav), "%s/%s-MIX.wav", data->dir, data->base_no_ext);

        int fmt = ui_state.config.export_format;
        if (fmt == 0 || fmt == 2) {
            argv[argc++] = "-map"; argv[argc++] = "0:a:0";
            argv[argc++] = "-c:a"; argv[argc++] = "flac";
            argv[argc++] = "-metadata"; argv[argc++] = meta_title;
            argv[argc++] = mix_path_flac;
        }
        if (fmt == 1 || fmt == 2) {
            argv[argc++] = "-map"; argv[argc++] = "0:a:0";
            argv[argc++] = "-c:a"; argv[argc++] = "pcm_s16le";
            argv[argc++] = "-metadata"; argv[argc++] = meta_title;
            argv[argc++] = mix_path_wav;
        }

        argv[argc++] = NULL;
    }

    GError *spawn_err = NULL;
    if (g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL, &active_muxer_pid, &spawn_err)) {
        g_child_watch_add(active_muxer_pid, on_muxer_finished, NULL);
        log_and_notify(1, "Status: Saved, Muxing in Background");
    } else {
        log_and_notify(1, "Status: FFmpeg Launch Failed");
        extern GtkWidget *main_spinner;
        if (main_spinner) {
            gtk_spinner_stop(GTK_SPINNER(main_spinner));
            gtk_widget_queue_draw(main_spinner);
        }
        g_error_free(spawn_err);
        if (btn_save_mux) gtk_widget_set_sensitive(btn_save_mux, TRUE);
    }

    g_free(meta_title);
    g_free(meta_date);
    g_free(meta_comment);
    set_app_waiting_state(false);
}

static void on_save_mux_file_chosen(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    (void)user_data;
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source_object);
    GError *error = NULL;
    GFile *file = gtk_file_dialog_save_finish(dialog, res, &error);

    if (!file) {
        extern GtkWidget *btn_save_mux;
        if (btn_save_mux) gtk_widget_set_sensitive(btn_save_mux, TRUE);
        log_and_notify(1, "Status: Export Cancelled");
        return;
    }

    extern GtkWidget *btn_save_mux;
    if (btn_save_mux) gtk_widget_remove_css_class(btn_save_mux, "needs-save");
    extern GtkWidget *btn_save_session;
    if (btn_save_session) gtk_widget_remove_css_class(btn_save_session, "needs-save");

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

    time_t t = time(NULL);
    struct tm tm = *localtime(&t);
    char date_str[32];
    snprintf(date_str, sizeof(date_str), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);

    extern char current_raw_path[1024];
    bool has_video = (strlen(current_raw_path) > 0 && access(current_raw_path, F_OK) == 0);

    int fmt = ui_state.config.export_format;
    const char *fmt_str = (fmt == 0) ? "FLAC" : (fmt == 1) ? "WAV" : "FLAC + WAV";
    log_and_notify(0, "[EXPORT] Muxing started | Video: %s | Audio Format: %s\n", has_video ? "YES" : "NO", fmt_str);

    ExportTaskData *data = g_new0(ExportTaskData, 1);
    strncpy(data->dir, dir, sizeof(data->dir) - 1);
    strncpy(data->base_no_ext, base_no_ext, sizeof(data->base_no_ext) - 1);
    strncpy(data->date_str, date_str, sizeof(data->date_str) - 1);
    data->has_video = has_video;

    if (!has_video) {
        extern char temp_wav_path[1024];
        snprintf(temp_wav_path, sizeof(temp_wav_path), "/tmp/%s-RAW.wav", base_no_ext);
        strncpy(data->temp_wav_path, temp_wav_path, sizeof(data->temp_wav_path) - 1);

        size_t start_f = 0;
        size_t end_f = atomic_load_explicit(&backing_track_frames, memory_order_acquire);

        if (atomic_load_explicit(&loop_active, memory_order_acquire)) {
            start_f = atomic_load_explicit(&loop_start_frame, memory_order_acquire);
            end_f = atomic_load_explicit(&loop_end_frame, memory_order_acquire);
        }
        data->start_f = start_f;
        data->end_f = end_f;
        data->samplerate = atomic_load_explicit(&active_sample_rate, memory_order_acquire);
    }

    log_and_notify(1, "Status: Exporting Audio/Video...");
    extern GtkWidget *main_spinner;
    if (main_spinner) gtk_spinner_start(GTK_SPINNER(main_spinner));
    set_app_waiting_state(true);

    // Lock the master container so the frame clock stays alive for the spinner
    GtkWindow *win = gtk_application_get_active_window(GTK_APPLICATION(g_application_get_default()));
    if (win) {
        GtkWidget *master_box = gtk_window_get_child(win);
        if (master_box) gtk_widget_set_sensitive(master_box, FALSE);
    }

    GTask *task = g_task_new(NULL, NULL, export_mixdown_ready, NULL);
    g_task_set_task_data(task, data, g_free);
    g_task_run_in_thread(task, export_mixdown_thread);
    g_object_unref(task);

    g_free(dir);
    g_free(basename_ext);
    g_free(chosen_path);
}

void on_save_mux_clicked(GtkButton *button, gpointer user_data) {
    (void)button; (void)user_data;

    gtk_widget_set_sensitive(btn_save_mux, FALSE);

    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Export Mix");

    bool has_video = (strlen(current_raw_path) > 0 && access(current_raw_path, F_OK) == 0);

    if (!has_video) {
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
    extern bool pending_quit_after_save;

    if (file) {
        char *path = g_file_get_path(file);
        on_stop_clicked(NULL, NULL);

        set_app_waiting_state(true);
        int save_status = save_qjams_session(path);
        set_app_waiting_state(false);

        if (save_status == 0) {
            extern void purge_autosave_file(void);
            purge_autosave_file();
            purge_autosave_file();

            strncpy(ui_state.selected_track_path, path, sizeof(ui_state.selected_track_path) - 1);
            ui_state.selected_track_path[sizeof(ui_state.selected_track_path) - 1] = '\0';
            extern void update_playlist_toggle_state(void);
            update_playlist_toggle_state();

            char *basename = g_path_get_basename(path);
            char status[512];
            snprintf(status, sizeof(status), "Status: Session saved to %s", basename);
            log_and_notify(1, status);
            g_free(basename);

            gtk_widget_remove_css_class(btn_save_mux, "needs-save");
            gtk_widget_remove_css_class(btn_save_session, "needs-save");

            ui_state.is_existing_session = true;
            mark_session_saved();

            if (pending_quit_after_save) {
                GList *windows = gtk_application_get_windows(GTK_APPLICATION(g_application_get_default()));
                if (windows) gtk_window_destroy(GTK_WINDOW(windows->data));
            }
        } else {
            log_and_notify(1, "Status: Failed to save session.");
            pending_quit_after_save = false;
        }
        g_free(path); g_object_unref(file);
    } else {
        pending_quit_after_save = false;
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

    return G_SOURCE_REMOVE;
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

    GtkWindow *win = gtk_application_get_active_window(GTK_APPLICATION(g_application_get_default()));
    if (win) {
        GtkWidget *master_box = gtk_window_get_child(win);
        if (master_box) gtk_widget_set_sensitive(master_box, TRUE);
    }

    gtk_spinner_stop(GTK_SPINNER(main_spinner));
    set_app_waiting_state(false);

    if (load_status >= 0) {
        ui_state.is_existing_session = true;
        mark_session_saved();

        strncpy(ui_state.selected_track_path, path, sizeof(ui_state.selected_track_path) - 1);
        ui_state.selected_track_path[sizeof(ui_state.selected_track_path) - 1] = '\0';
        extern void update_playlist_toggle_state(void);
        update_playlist_toggle_state();

        refresh_multitrack_tracks_ui();
        extern void update_multitrack_status_ui(void);
        update_multitrack_status_ui();

        char *basename = g_path_get_basename(path);
        if (load_status == 1) {
            snprintf(ui_state.workspace_status, sizeof(ui_state.workspace_status), "Status: Session '%s' loaded (WARNING: Truncated)", basename);
        } else {
            snprintf(ui_state.workspace_status, sizeof(ui_state.workspace_status), "Status: Session '%s' loaded", basename);
        }
        log_and_notify(1, ui_state.workspace_status);

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
        extern GtkWidget *btn_record_video;
        if (btn_record_video) gtk_widget_set_sensitive(btn_record_video, TRUE);

        g_idle_add(deferred_session_redraw, NULL);

    } else {
        log_and_notify(1, "Status: Failed to load session.");
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

    abort_active_video_preview(false);

    extern _Atomic size_t backing_track_frames;
    atomic_store_explicit(&backing_track_frames, 0, memory_order_release);

    for (int i = 0; i < MAX_TRACKS; i++) {
        atomic_store_explicit(&master_tracks[i].has_audio, false, memory_order_release);
    }

    GtkWindow *win = gtk_application_get_active_window(GTK_APPLICATION(g_application_get_default()));
    if (win) {
        GtkWidget *master_box = gtk_window_get_child(win);
        if (master_box) gtk_widget_set_sensitive(master_box, FALSE);
    }

    gtk_spinner_start(GTK_SPINNER(main_spinner));
    log_and_notify(1, "Status: Extracting session to RAM...");
    gtk_label_set_text(GTK_LABEL(lbl_track), "Track: Loading...");

    gtk_widget_set_sensitive(btn_load, FALSE);
    gtk_widget_set_sensitive(btn_prev, FALSE);
    gtk_widget_set_sensitive(btn_next, FALSE);
    gtk_widget_set_sensitive(btn_play, FALSE);
    gtk_widget_set_sensitive(btn_record, FALSE);
    extern GtkWidget *btn_record_video;
    if (btn_record_video) gtk_widget_set_sensitive(btn_record_video, FALSE);
    gtk_widget_set_sensitive(btn_playlist_toggle, FALSE);

    if (btn_load_session) gtk_widget_set_sensitive(btn_load_session, FALSE);

    set_app_waiting_state(true);
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
    if (is_session_dirty() && pristine_frames > 0) {
        GtkAlertDialog *alert = gtk_alert_dialog_new("Overwrite Active Session?");
        gtk_alert_dialog_set_detail(alert, "Loading a new session will discard your currently recorded audio. Continue?");
        const char *buttons[] = { "Discard & Load", "Cancel", NULL };
        gtk_alert_dialog_set_buttons(alert, buttons);
        gtk_alert_dialog_set_cancel_button(alert, 1);
        gtk_alert_dialog_set_default_button(alert, 1);
        gtk_alert_dialog_choose(alert, GTK_WINDOW(user_data), NULL, on_load_session_overwrite_confirm, user_data);
    } else {
        execute_load_session_dialog(GTK_WIDGET(user_data));
    }
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
        log_and_notify(1, "Status: Playlist Saved");
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

void ui_command_post_init(GtkBuilder *b_cmd, GtkWindow *window) {
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
}
