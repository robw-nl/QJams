#include "ui_command_post.h"
#include "audio_core.h"
#include "audio_internal.h"
#include "audio_memory.h"
#include "config.h"
#include "media_io.h"
#include "ui_globals.h"
#include "ui_lifecycle.h"
#include "ui_multitrack.h"
#include "ui_playlist.h"
#include "ui_settings.h"
#include "ui_transport.h"
#include "ui_waveforms.h"
#include "video_engine.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

GPid active_muxer_pid = 0;
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
            strncpy(ui_state.config.last_track_dir, dir,
                    sizeof(ui_state.config.last_track_dir) - 1);
            save_qjams_config(ui_state.config_path, &ui_state.config);
            g_free(dir);
        }

        g_free(path);
        g_object_unref(file);
    } else if (error) {
        g_error_free(error);
    }
}

static void execute_load_track_dialog(GtkWidget *window) {
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Import Media");

    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    GtkFileFilter *filter;

    // 1. All Supported Audio & Video (Default)
    filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "All Supported Media");
    gtk_file_filter_add_mime_type(filter, "audio/flac");
    gtk_file_filter_add_mime_type(filter, "audio/x-wav");
    gtk_file_filter_add_mime_type(filter, "audio/mpeg");
    gtk_file_filter_add_mime_type(filter, "audio/ogg");
    gtk_file_filter_add_mime_type(filter, "audio/mp4");
    gtk_file_filter_add_mime_type(filter, "audio/x-aiff");
    gtk_file_filter_add_mime_type(filter, "video/x-matroska");
    gtk_file_filter_add_mime_type(filter, "video/mp4");
    gtk_file_filter_add_mime_type(filter, "video/webm");
    g_list_store_append(filters, filter);
    g_object_unref(filter);

    // 2. Audio Only
    filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Audio Files (FLAC, WAV, MP3, M4A, OGG, AIFF)");
    gtk_file_filter_add_mime_type(filter, "audio/flac");
    gtk_file_filter_add_mime_type(filter, "audio/x-wav");
    gtk_file_filter_add_mime_type(filter, "audio/mpeg");
    gtk_file_filter_add_mime_type(filter, "audio/ogg");
    gtk_file_filter_add_mime_type(filter, "audio/mp4");
    gtk_file_filter_add_mime_type(filter, "audio/x-aiff");
    g_list_store_append(filters, filter);
    g_object_unref(filter);

    // 3. Video Only
    filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Video Files (MKV, MP4, WebM)");
    gtk_file_filter_add_mime_type(filter, "video/x-matroska");
    gtk_file_filter_add_mime_type(filter, "video/mp4");
    gtk_file_filter_add_mime_type(filter, "video/webm");
    g_list_store_append(filters, filter);
    g_object_unref(filter);

    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
    g_object_unref(filters);

    gtk_file_dialog_open(dialog, GTK_WINDOW(window), NULL, on_file_chosen, NULL);
    g_object_unref(dialog);
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

    if (status == 0)
        log_and_notify(1, "Status: Muxing Complete!");
    else
        log_and_notify(1, "Status: Muxing Failed (FFmpeg Error)");
    gtk_widget_set_sensitive(btn_save_mux, TRUE);
}

#define PUMP_CHUNK_FRAMES 1024

typedef struct {
    float *mix_buf;
    size_t frames;
    int fd;
    int num_tracks;
    int track_indices[QJ_MAX_TRACKS];
    int track_fds[QJ_MAX_TRACKS];
    size_t start_f;
    size_t v_end_f;
    bool holds_read_lock;
    float stage_buf[QJ_MAX_TRACKS][PUMP_CHUNK_FRAMES * 2];
    size_t stage_off[QJ_MAX_TRACKS];
    size_t stage_len[QJ_MAX_TRACKS];
    size_t track_cur_f[QJ_MAX_TRACKS];
} PumperData;

/**
 * @brief Background worker that multiplexes the stereo mixdown and all active isolated
 *        DAW tracks into FFmpeg via non-blocking POSIX pipes using poll().
 * @param arg Pointer to heap-allocated PumperData.
 * @return NULL upon completion.
 */
static void *pump_mixdown_thread(void *arg) {
    PumperData *pd = (PumperData *)arg;

    // Critical Safety: Prevent application crash if FFmpeg closes any pipe prematurely
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    size_t mix_bytes_left = pd->frames * 2 * sizeof(float);
    uint8_t *mix_ptr = (uint8_t *)pd->mix_buf;

    for (int k = 0; k < pd->num_tracks; k++) {
        pd->track_cur_f[k] = pd->start_f;
        pd->stage_off[k] = 0;
        pd->stage_len[k] = 0;
    }

    while (true) {
        struct pollfd pfds[QJ_MAX_TRACKS + 1];
        int pfd_map[QJ_MAX_TRACKS + 1];
        nfds_t nfds = 0;

        if (pd->fd != -1) {
            pfds[nfds].fd = pd->fd;
            pfds[nfds].events = POLLOUT;
            pfds[nfds].revents = 0;
            pfd_map[nfds] = -1;
            nfds++;
        }

        for (int k = 0; k < pd->num_tracks; k++) {
            if (pd->track_fds[k] != -1) {
                pfds[nfds].fd = pd->track_fds[k];
                pfds[nfds].events = POLLOUT;
                pfds[nfds].revents = 0;
                pfd_map[nfds] = k;
                nfds++;
            }
        }

        if (nfds == 0)
            break;

        int ret = poll(pfds, nfds, 1000);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (ret == 0)
            continue;

        for (nfds_t i = 0; i < nfds; i++) {
            if (!(pfds[i].revents & (POLLOUT | POLLERR | POLLHUP | POLLNVAL)))
                continue;

            int idx = pfd_map[i];
            if (idx == -1) {
                size_t chunk = mix_bytes_left > 65536 ? 65536 : mix_bytes_left;
                ssize_t written = write(pd->fd, mix_ptr, chunk);
                if (written > 0) {
                    mix_ptr += written;
                    mix_bytes_left -= (size_t)written;
                    if (mix_bytes_left == 0) {
                        close(pd->fd);
                        pd->fd = -1;
                    }
                } else if (written < 0 &&
                           (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                    continue;
                } else {
                    close(pd->fd);
                    pd->fd = -1;
                }
            } else {
                int trk = pd->track_indices[idx];
                if (pd->stage_off[idx] >= pd->stage_len[idx]) {
                    if (pd->track_cur_f[idx] >= pd->v_end_f) {
                        close(pd->track_fds[idx]);
                        pd->track_fds[idx] = -1;
                        continue;
                    }
                    size_t f_rem = pd->v_end_f - pd->track_cur_f[idx];
                    size_t f_to_stage = (f_rem > PUMP_CHUNK_FRAMES) ? PUMP_CHUNK_FRAMES : f_rem;
                    float *src_buf = master_tracks[trk].active_buffer;
                    size_t alloc_f = master_tracks[trk].allocated_frames;
                    float gain =
                        atomic_load_explicit(&master_tracks[trk].gain, memory_order_relaxed);

                    for (size_t f = 0; f < f_to_stage; f++) {
                        size_t pos = pd->track_cur_f[idx] + f;
                        if (src_buf && pos < alloc_f) {
                            pd->stage_buf[idx][f * 2] = soft_clip(src_buf[pos * 2] * gain);
                            pd->stage_buf[idx][f * 2 + 1] = soft_clip(src_buf[pos * 2 + 1] * gain);
                        } else {
                            pd->stage_buf[idx][f * 2] = 0.0f;
                            pd->stage_buf[idx][f * 2 + 1] = 0.0f;
                        }
                    }
                    pd->track_cur_f[idx] += f_to_stage;
                    pd->stage_off[idx] = 0;
                    pd->stage_len[idx] = f_to_stage * 2 * sizeof(float);
                }

                uint8_t *s_ptr = ((uint8_t *)pd->stage_buf[idx]) + pd->stage_off[idx];
                size_t s_rem = pd->stage_len[idx] - pd->stage_off[idx];
                ssize_t written = write(pd->track_fds[idx], s_ptr, s_rem);
                if (written > 0) {
                    pd->stage_off[idx] += (size_t)written;
                    if (pd->stage_off[idx] >= pd->stage_len[idx] &&
                        pd->track_cur_f[idx] >= pd->v_end_f) {
                        close(pd->track_fds[idx]);
                        pd->track_fds[idx] = -1;
                    }
                } else if (written < 0 &&
                           (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                    continue;
                } else {
                    close(pd->track_fds[idx]);
                    pd->track_fds[idx] = -1;
                }
            }
        }
    }

    if (pd->fd != -1)
        close(pd->fd);
    for (int k = 0; k < pd->num_tracks; k++) {
        if (pd->track_fds[k] != -1)
            close(pd->track_fds[k]);
    }
    if (pd->holds_read_lock)
        release_track_read();

    free(pd->mix_buf);
    free(pd);
    return NULL;
}

static void export_mixdown_ready(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    (void)source_object;
    (void)user_data;
    GError *error = NULL;
    bool success = g_task_propagate_boolean(G_TASK(res), &error);
    ExportTaskData *data = (ExportTaskData *)g_task_get_task_data(G_TASK(res));

    GtkWindow *win =
        gtk_application_get_active_window(GTK_APPLICATION(g_application_get_default()));
    if (win) {
        GtkWidget *master_box = gtk_window_get_child(win);
        if (master_box)
            gtk_widget_set_sensitive(master_box, TRUE);
    }

    if (!success) {
        if (main_spinner) {
            gtk_spinner_stop(GTK_SPINNER(main_spinner));
            gtk_widget_queue_draw(main_spinner);
        }
        if (btn_save_mux)
            gtk_widget_set_sensitive(btn_save_mux, TRUE);
        log_and_notify(1, "Status: Failed to render audio mixdown");
        set_app_waiting_state(false);
        return;
    }

    char *argv[256];
    int argc = 0;
    argv[argc++] = "ffmpeg";
    argv[argc++] = "-y";
    argv[argc++] = "-loglevel";
    argv[argc++] = "error";

    char *meta_title = g_strdup_printf("title=%s", data->base_no_ext);
    char *meta_date = g_strdup_printf("recordingdate=%s", data->date_str);
    char *meta_comment = g_strdup(
        "comment=Recorded with QJams. (c) Rob Wijhenke. https://sites.google.com/view/qjams");

    char mix_path_flac[1024];
    char mix_path_wav[1024];
    snprintf(mix_path_flac, sizeof(mix_path_flac), "%s/%s-MIX.flac", data->dir, data->base_no_ext);
    snprintf(mix_path_wav, sizeof(mix_path_wav), "%s/%s-MIX.wav", data->dir, data->base_no_ext);

    char sr_str[32];
    snprintf(sr_str, sizeof(sr_str), "%d", data->samplerate);

    int fmt = ui_state.config.export_format;

    int num_raw_tracks = 0;
    int raw_track_indices[QJ_MAX_TRACKS];
    int raw_pipe_read_fds[QJ_MAX_TRACKS];
    int raw_pipe_write_fds[QJ_MAX_TRACKS];
    char fd_uri_strs[QJ_MAX_TRACKS][64];
    char map_trk_strs[QJ_MAX_TRACKS][32];
    char meta_spec_strs[QJ_MAX_TRACKS + 1][32];
    char meta_trk_titles[QJ_MAX_TRACKS + 1][128];
    bool holds_read_lock = false;
    size_t v_end_target = (data->video_end_f > data->start_f) ? data->video_end_f : data->end_f;

    if (!data->has_video) {
        argv[argc++] = "-f";
        argv[argc++] = "f32le";
        argv[argc++] = "-ar";
        argv[argc++] = sr_str;
        argv[argc++] = "2";
        argv[argc - 1] = "-ac";
        argv[argc++] = "2";
        argv[argc++] = "-i";
        argv[argc++] = "pipe:0";

        if (fmt == 0 || fmt == 2) {
            argv[argc++] = "-map";
            argv[argc++] = "0:a:0";
            argv[argc++] = "-c:a";
            argv[argc++] = "flac";
            argv[argc++] = "-sample_fmt";
            argv[argc++] = "s32";
            argv[argc++] = "-metadata";
            argv[argc++] = meta_title;
            argv[argc++] = "-metadata";
            argv[argc++] = meta_date;
            argv[argc++] = "-metadata";
            argv[argc++] = meta_comment;
            argv[argc++] = mix_path_flac;
        }
        if (fmt == 1 || fmt == 2) {
            argv[argc++] = "-map";
            argv[argc++] = "0:a:0";
            argv[argc++] = "-c:a";
            argv[argc++] = "pcm_s24le";
            argv[argc++] = "-metadata";
            argv[argc++] = meta_title;
            argv[argc++] = "-metadata";
            argv[argc++] = meta_date;
            argv[argc++] = "-metadata";
            argv[argc++] = meta_comment;
            argv[argc++] = mix_path_wav;
        }

        if (fmt == 0 || fmt == 2)
            remove(mix_path_flac);
        if (fmt == 1 || fmt == 2)
            remove(mix_path_wav);
    } else {
        bool is_live_raw = has_active_live_video();
        const char *video_src_path = is_live_raw ? current_raw_path : ui_state.selected_track_path;
        char raw_path[1024];
        char mix_path[1024];
        snprintf(raw_path, sizeof(raw_path), "%s/%s-RAW.mkv", data->dir, data->base_no_ext);
        snprintf(mix_path, sizeof(mix_path), "%s/%s-MIX.mkv", data->dir, data->base_no_ext);
        float current_speed =
            atomic_load_explicit(&get_transport_state()->playback_speed, memory_order_relaxed);
        if (current_speed <= 0.0f)
            current_speed = 1.0f;
        double v_start_sec = ((double)video_start_frame * (double)current_speed) / data->samplerate;
        double a_start_sec = (double)data->start_f / data->samplerate;
        double trim_start = a_start_sec - v_start_sec;
        if (trim_start < 0.0)
            trim_start = 0.0;
        double v_duration = (double)(v_end_target - data->start_f) / data->samplerate;
        double audio_duration = (double)(data->end_f - data->start_f) / data->samplerate;
        char trim_str[G_ASCII_DTOSTR_BUF_SIZE], v_dur_str[G_ASCII_DTOSTR_BUF_SIZE],
            a_dur_str[G_ASCII_DTOSTR_BUF_SIZE];
        g_ascii_dtostr(trim_str, sizeof(trim_str), trim_start);
        g_ascii_dtostr(v_dur_str, sizeof(v_dur_str), v_duration);
        g_ascii_dtostr(a_dur_str, sizeof(a_dur_str), audio_duration);

        // Input 0: Video source file
        argv[argc++] = "-ss";
        argv[argc++] = trim_str;
        argv[argc++] = "-t";
        argv[argc++] = v_dur_str;
        argv[argc++] = "-i";
        argv[argc++] = (char *)video_src_path;

        // Input 1: Master Mix (pipe:0)
        argv[argc++] = "-f";
        argv[argc++] = "f32le";
        argv[argc++] = "-ar";
        argv[argc++] = sr_str;
        argv[argc++] = "-ac";
        argv[argc++] = "2";
        argv[argc++] = "-i";
        argv[argc++] = "pipe:0";

        // Inputs 2..N: Live DAW tracks (including post-video overdubs) for -RAW.mkv
        if (try_acquire_track_read()) {
            holds_read_lock = true;
            for (int t = 0; t < QJ_MAX_TRACKS; t++) {
                if (master_tracks[t].active_buffer &&
                    atomic_load_explicit(&master_tracks[t].has_audio, memory_order_acquire)) {
                    int pfd[2];
                    if (pipe(pfd) == 0) {
                        fcntl(pfd[0], F_SETFD, 0);
                        fcntl(pfd[1], F_SETFD, FD_CLOEXEC);
                        fcntl(pfd[1], F_SETFL, fcntl(pfd[1], F_GETFL, 0) | O_NONBLOCK);

                        raw_track_indices[num_raw_tracks] = t;
                        raw_pipe_read_fds[num_raw_tracks] = pfd[0];
                        raw_pipe_write_fds[num_raw_tracks] = pfd[1];
                        snprintf(fd_uri_strs[num_raw_tracks], sizeof(fd_uri_strs[0]), "/dev/fd/%d",
                                 pfd[0]);

                        argv[argc++] = "-f";
                        argv[argc++] = "f32le";
                        argv[argc++] = "-ar";
                        argv[argc++] = sr_str;
                        argv[argc++] = "-ac";
                        argv[argc++] = "2";
                        argv[argc++] = "-i";
                        argv[argc++] = fd_uri_strs[num_raw_tracks];
                        num_raw_tracks++;
                    }
                }
            }
        }

        if (true) {
            argv[argc++] = "-map";
            argv[argc++] = "0:v:0";
            if (num_raw_tracks > 0) {
                // Stream 0:a:0 -> Master Mix
                argv[argc++] = "-map";
                argv[argc++] = "1:a:0";
                snprintf(meta_spec_strs[0], sizeof(meta_spec_strs[0]), "-metadata:s:a:0");
                snprintf(meta_trk_titles[0], sizeof(meta_trk_titles[0]), "title=Master Mix");
                argv[argc++] = meta_spec_strs[0];
                argv[argc++] = meta_trk_titles[0];

                // Streams 0:a:1..K -> Isolated DAW tracks (all overdubs included)
                for (int k = 0; k < num_raw_tracks; k++) {
                    snprintf(map_trk_strs[k], sizeof(map_trk_strs[0]), "%d:a:0", k + 2);
                    argv[argc++] = "-map";
                    argv[argc++] = map_trk_strs[k];

                    char t_name[64];
                    get_multitrack_track_name(raw_track_indices[k], t_name);
                    snprintf(meta_spec_strs[k + 1], sizeof(meta_spec_strs[0]), "-metadata:s:a:%d",
                             k + 1);
                    snprintf(meta_trk_titles[k + 1], sizeof(meta_trk_titles[0]), "title=%s",
                             t_name);
                    argv[argc++] = meta_spec_strs[k + 1];
                    argv[argc++] = meta_trk_titles[k + 1];
                }
            } else {
                argv[argc++] = "-map";
                argv[argc++] = "0:a";
            }
            argv[argc++] = "-t";
            argv[argc++] = v_dur_str;
            argv[argc++] = "-c:v";
            argv[argc++] = "copy";
            argv[argc++] = "-c:a";
            argv[argc++] = "pcm_s24le";
            argv[argc++] = "-metadata";
            argv[argc++] = meta_title;
            argv[argc++] = "-metadata";
            argv[argc++] = meta_date;
            argv[argc++] = "-metadata";
            argv[argc++] = meta_comment;
            argv[argc++] = raw_path;
            remove(raw_path);
        }
        argv[argc++] = "-map";
        argv[argc++] = "0:v:0";
        argv[argc++] = "-map";
        argv[argc++] = "1:a:0";
        argv[argc++] = "-t";
        argv[argc++] = v_dur_str;
        argv[argc++] = "-c:v";
        argv[argc++] = "copy";
        argv[argc++] = "-c:a";
        argv[argc++] = "flac";
        argv[argc++] = "-sample_fmt";
        argv[argc++] = "s32";
        argv[argc++] = "-metadata";
        argv[argc++] = meta_title;
        argv[argc++] = "-metadata";
        argv[argc++] = meta_date;
        argv[argc++] = "-metadata";
        argv[argc++] = meta_comment;
        argv[argc++] = mix_path;

        // --- Standalone Audio Outputs (run for the full audio duration up to 03:30) ---
        if (fmt == 0 || fmt == 2) {
            argv[argc++] = "-map";
            argv[argc++] = "1:a:0";
            argv[argc++] = "-t";
            argv[argc++] = a_dur_str;
            argv[argc++] = "-c:a";
            argv[argc++] = "flac";
            argv[argc++] = "-sample_fmt";
            argv[argc++] = "s32";
            argv[argc++] = "-metadata";
            argv[argc++] = meta_title;
            argv[argc++] = "-metadata";
            argv[argc++] = meta_date;
            argv[argc++] = "-metadata";
            argv[argc++] = meta_comment;
            argv[argc++] = mix_path_flac;
        }
        if (fmt == 1 || fmt == 2) {
            argv[argc++] = "-map";
            argv[argc++] = "1:a:0";
            argv[argc++] = "-t";
            argv[argc++] = a_dur_str;
            argv[argc++] = "-c:a";
            argv[argc++] = "pcm_s24le";
            argv[argc++] = "-metadata";
            argv[argc++] = meta_title;
            argv[argc++] = "-metadata";
            argv[argc++] = meta_date;
            argv[argc++] = "-metadata";
            argv[argc++] = meta_comment;
            argv[argc++] = mix_path_wav;
        }
        remove(raw_path);
        remove(mix_path);
        if (fmt == 0 || fmt == 2)
            remove(mix_path_flac);
        if (fmt == 1 || fmt == 2)
            remove(mix_path_wav);
    }

    argv[argc++] = NULL;

    int stdin_fd = -1;
    GError *spawn_err = NULL;
    GSpawnFlags spawn_flags = G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD;
    if (num_raw_tracks > 0)
        spawn_flags |= G_SPAWN_LEAVE_DESCRIPTORS_OPEN;

    if (g_spawn_async_with_pipes(NULL, argv, NULL, spawn_flags, NULL, NULL, &active_muxer_pid,
                                 &stdin_fd, NULL, NULL, &spawn_err)) {
        for (int k = 0; k < num_raw_tracks; k++) {
            close(raw_pipe_read_fds[k]);
        }
        fcntl(stdin_fd, F_SETFL, fcntl(stdin_fd, F_GETFL, 0) | O_NONBLOCK);
        g_child_watch_add(active_muxer_pid, on_muxer_finished, NULL);

        PumperData *pd = calloc(1, sizeof(PumperData));
        if (pd) {
            pd->mix_buf = data->mix_buf_ptr;
            pd->frames = data->end_f - data->start_f;
            pd->fd = stdin_fd;
            pd->num_tracks = num_raw_tracks;
            pd->start_f = data->start_f;
            pd->v_end_f = v_end_target;
            pd->holds_read_lock = holds_read_lock;
            for (int k = 0; k < num_raw_tracks; k++) {
                pd->track_indices[k] = raw_track_indices[k];
                pd->track_fds[k] = raw_pipe_write_fds[k];
            }

            pthread_t pump_tid;
            if (pthread_create(&pump_tid, NULL, pump_mixdown_thread, pd) == 0) {
                pthread_detach(pump_tid);
                log_and_notify(1, "Status: Saved, Muxing in Background (24-Bit)");
            } else {
                close(stdin_fd);
                for (int k = 0; k < num_raw_tracks; k++)
                    close(raw_pipe_write_fds[k]);
                if (holds_read_lock)
                    release_track_read();
                free(data->mix_buf_ptr);
                free(pd);
                log_and_notify(1, "Status: Export Thread Creation Failed");
            }
        } else {
            close(stdin_fd);
            for (int k = 0; k < num_raw_tracks; k++)
                close(raw_pipe_write_fds[k]);
            if (holds_read_lock)
                release_track_read();
            free(data->mix_buf_ptr);
            log_and_notify(1, "Status: Out of Memory for Export Pipe");
        }
    } else {
        for (int k = 0; k < num_raw_tracks; k++) {
            close(raw_pipe_read_fds[k]);
            close(raw_pipe_write_fds[k]);
        }
        if (holds_read_lock)
            release_track_read();
        log_and_notify(1, "Status: FFmpeg Launch Failed");
        if (main_spinner) {
            gtk_spinner_stop(GTK_SPINNER(main_spinner));
            gtk_widget_queue_draw(main_spinner);
        }
        g_error_free(spawn_err);
        free(data->mix_buf_ptr);
        if (btn_save_mux)
            gtk_widget_set_sensitive(btn_save_mux, TRUE);
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
        if (error)
            g_error_free(error);
        if (btn_save_mux)
            gtk_widget_set_sensitive(btn_save_mux, TRUE);
        log_and_notify(1, "Status: Export Cancelled");
        return;
    }

    if (btn_save_mux)
        gtk_widget_remove_css_class(btn_save_mux, "needs-save");
    if (btn_save_session)
        gtk_widget_remove_css_class(btn_save_session, "needs-save");

    char *chosen_path = g_file_get_path(file);
    g_object_unref(file);

    char *dir = g_path_get_dirname(chosen_path);
    char *basename_ext = g_path_get_basename(chosen_path);

    char base_no_ext[512];
    strncpy(base_no_ext, basename_ext, sizeof(base_no_ext) - 1);
    base_no_ext[sizeof(base_no_ext) - 1] = '\0';

    char *dot = strrchr(base_no_ext, '.');
    if (dot)
        *dot = '\0';

    size_t len = strlen(base_no_ext);
    if (len > 4 && strcmp(base_no_ext + len - 4, "-MIX") == 0) {
        base_no_ext[len - 4] = '\0';
    } else if (len > 4 && strcmp(base_no_ext + len - 4, "-RAW") == 0) {
        base_no_ext[len - 4] = '\0';
    }

    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    char date_str[32];
    snprintf(date_str, sizeof(date_str), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1,
             tm.tm_mday);
    bool has_video = has_active_live_video() || (video_end_frame > video_start_frame &&
                                                 strlen(ui_state.selected_track_path) > 0 &&
                                                 media_has_video(ui_state.selected_track_path) &&
                                                 access(ui_state.selected_track_path, F_OK) == 0);
    int fmt = ui_state.config.export_format;
    const char *fmt_str = (fmt == 0) ? "FLAC" : (fmt == 1) ? "WAV" : "FLAC + WAV";
    log_and_notify(0, "[EXPORT] Muxing started | Video: %s | Audio Format: %s\n",
                   has_video ? "YES" : "NO", fmt_str);

    ExportTaskData *data = g_new0(ExportTaskData, 1);
    strncpy(data->dir, dir, sizeof(data->dir) - 1);
    strncpy(data->base_no_ext, base_no_ext, sizeof(data->base_no_ext) - 1);
    strncpy(data->date_str, date_str, sizeof(data->date_str) - 1);
    data->has_video = has_video;
    data->mix_buf_ptr = NULL;

    size_t start_f = 0;
    size_t end_f = pristine_frames;
    size_t video_end_f = pristine_frames;

    if (atomic_load_explicit(&loop_active, memory_order_acquire)) {
        float current_speed =
            atomic_load_explicit(&get_transport_state()->playback_speed, memory_order_relaxed);
        if (current_speed <= 0.0f)
            current_speed = 1.0f;
        size_t l_start = atomic_load_explicit(&loop_start_frame, memory_order_acquire);
        size_t l_end = atomic_load_explicit(&loop_end_frame, memory_order_acquire);
        start_f = (size_t)((double)l_start * (double)current_speed);
        end_f = (size_t)((double)l_end * (double)current_speed);
        if (end_f > pristine_frames)
            end_f = pristine_frames;
        video_end_f = end_f;
        if (has_video && video_end_frame > video_start_frame) {
            size_t v_end = (size_t)((double)video_end_frame * (double)current_speed);
            if (v_end < video_end_f)
                video_end_f = v_end;
        }
    } else if (has_video && video_end_frame > video_start_frame) {
        float current_speed =
            atomic_load_explicit(&get_transport_state()->playback_speed, memory_order_relaxed);
        if (current_speed <= 0.0f)
            current_speed = 1.0f;
        start_f = (size_t)((double)video_start_frame * (double)current_speed);
        video_end_f = (size_t)((double)video_end_frame * (double)current_speed);
        if (video_end_f > pristine_frames)
            video_end_f = pristine_frames;

        size_t last_rec = get_last_recorded_frame();
        end_f = (last_rec > video_end_f) ? last_rec : video_end_f;
        if (end_f > pristine_frames)
            end_f = pristine_frames;
    } else {
        size_t last_rec = get_last_recorded_frame();
        if (last_rec > 0 && last_rec < pristine_frames) {
            end_f = last_rec;
        }
        video_end_f = end_f;
    }
    data->start_f = start_f;
    data->end_f = end_f;
    data->video_end_f = video_end_f;
    data->samplerate =
        atomic_load_explicit(&get_transport_state()->active_sample_rate, memory_order_acquire);

    log_and_notify(1, "Status: Exporting Audio/Video...");
    if (main_spinner)
        gtk_spinner_start(GTK_SPINNER(main_spinner));
    set_app_waiting_state(true);

    // Lock the master container so the frame clock stays alive for the spinner
    GtkWindow *win =
        gtk_application_get_active_window(GTK_APPLICATION(g_application_get_default()));
    if (win) {
        GtkWidget *master_box = gtk_window_get_child(win);
        if (master_box)
            gtk_widget_set_sensitive(master_box, FALSE);
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
    (void)button;
    (void)user_data;
    extern size_t get_last_recorded_frame(void);
    if (pristine_frames == 0 || get_last_recorded_frame() == 0) {
        log_and_notify(1, "Status: Canvas is empty, nothing to export.");
        return;
    }
    gtk_widget_set_sensitive(btn_save_mux, FALSE);
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Export Mix");
    bool has_video = has_active_live_video() || (video_end_frame > video_start_frame &&
                                                 strlen(ui_state.selected_track_path) > 0 &&
                                                 media_has_video(ui_state.selected_track_path) &&
                                                 access(ui_state.selected_track_path, F_OK) == 0);
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

    gtk_file_dialog_save(dialog, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(button))), NULL,
                         on_save_mux_file_chosen, NULL);
    g_object_unref(dialog);
}

static void save_session_thread(GTask *task, gpointer source_object, gpointer task_data,
                                GCancellable *cancellable) {
    (void)source_object;
    (void)cancellable;
    char *path = (char *)task_data;

    int status = save_qjams_session(path);

    g_task_return_int(task, status);
}

static void save_session_ready(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    (void)source_object;
    (void)user_data;
    GError *error = NULL;
    int save_status = g_task_propagate_int(G_TASK(res), &error);
    char *path = (char *)g_task_get_task_data(G_TASK(res));

    set_app_waiting_state(false);

    if (save_status == 0) {
        purge_autosave_file();

        strncpy(ui_state.selected_track_path, path, sizeof(ui_state.selected_track_path) - 1);
        ui_state.selected_track_path[sizeof(ui_state.selected_track_path) - 1] = '\0';
        update_playlist_toggle_state();

        char *basename = g_path_get_basename(path);
        char *escaped_base = g_markup_escape_text(basename, -1);
        log_and_notify(1, "Status: Session saved to %s", escaped_base);
        g_free(escaped_base);
        g_free(basename);
        if (btn_save_mux)
            gtk_widget_remove_css_class(btn_save_mux, "needs-save");
        if (btn_save_session)
            gtk_widget_remove_css_class(btn_save_session, "needs-save");

        ui_state.is_existing_session = true;
        mark_session_saved();
        update_multitrack_status_ui();

        if (pending_quit_after_save) {
            GList *windows =
                gtk_application_get_windows(GTK_APPLICATION(g_application_get_default()));
            if (windows)
                gtk_window_destroy(GTK_WINDOW(windows->data));
        }
    } else {
        log_and_notify(1, "Status: Failed to save session.");
        pending_quit_after_save = false;
    }
}

/**
 * @brief Dispatches the background GTask to write the active multitrack session to disk.
 * @param owned_path Heap-allocated destination file path (ownership transferred to the GTask).
 */
static void execute_save_session_to_path(char *owned_path) {
    on_stop_clicked(NULL, NULL);

    set_app_waiting_state(true);
    log_and_notify(1, "Status: Saving session...");

    GTask *task = g_task_new(NULL, NULL, save_session_ready, NULL);
    g_task_set_task_data(task, owned_path, g_free);
    g_task_run_in_thread(task, save_session_thread);
    g_object_unref(task);
}

static void on_save_session_file_chosen(GObject *source_object, GAsyncResult *res,
                                        gpointer user_data) {
    (void)user_data;
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source_object);
    GError *error = NULL;
    GFile *file = gtk_file_dialog_save_finish(dialog, res, &error);

    if (file) {
        char *path = g_file_get_path(file);
        if (path && !g_str_has_suffix(path, ".qjams")) {
            char *with_ext = g_strdup_printf("%s.qjams", path);
            g_free(path);
            path = with_ext;
        }
        g_object_unref(file);
        if (path) {
            execute_save_session_to_path(path);
        } else {
            pending_quit_after_save = false;
        }
    } else {
        if (error)
            g_error_free(error);
        pending_quit_after_save = false;
    }
}

/**
 * @brief Opens the native file chooser dialog to save the session under a new filename.
 * @param parent Transient parent window for the file dialog.
 */
static void open_save_session_file_dialog(GtkWindow *parent) {
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Save Multi-Track Session");

    if (ui_state.is_existing_session && strlen(ui_state.selected_track_path) > 0 &&
        g_str_has_suffix(ui_state.selected_track_path, ".qjams") &&
        !g_str_has_suffix(ui_state.selected_track_path, ".autosave.qjams")) {
        char *basename = g_path_get_basename(ui_state.selected_track_path);
        gtk_file_dialog_set_initial_name(dialog, basename);
        g_free(basename);

        char *dir = g_path_get_dirname(ui_state.selected_track_path);
        if (dir) {
            GFile *initial_folder = g_file_new_for_path(dir);
            gtk_file_dialog_set_initial_folder(dialog, initial_folder);
            g_object_unref(initial_folder);
            g_free(dir);
        }
    } else {
        gtk_file_dialog_set_initial_name(dialog, "New_Session.qjams");
        if (strlen(ui_state.config.recordings_dir) > 0) {
            GFile *initial_folder = g_file_new_for_path(ui_state.config.recordings_dir);
            gtk_file_dialog_set_initial_folder(dialog, initial_folder);
            g_object_unref(initial_folder);
        }
    }

    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "QJams Session (*.qjams)");
    gtk_file_filter_add_pattern(filter, "*.qjams");
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, filter);
    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
    gtk_file_dialog_set_default_filter(dialog, filter);
    g_object_unref(filter);
    g_object_unref(filters);

    gtk_file_dialog_save(dialog, parent, NULL, on_save_session_file_chosen, NULL);
    g_object_unref(dialog);
}

static void on_existing_session_save_response(GObject *source_object, GAsyncResult *res,
                                              gpointer user_data) {
    GtkAlertDialog *alert = GTK_ALERT_DIALOG(source_object);
    int response = gtk_alert_dialog_choose_finish(alert, res, NULL);
    GtkWindow *parent = GTK_WINDOW(user_data);

    if (response == 0) {
        execute_save_session_to_path(g_strdup(ui_state.selected_track_path));
    } else if (response == 1) {
        open_save_session_file_dialog(parent);
    } else {
        pending_quit_after_save = false;
    }
}

static void on_save_session_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    GtkWindow *parent = GTK_WINDOW(user_data);

    if (ui_state.is_existing_session && strlen(ui_state.selected_track_path) > 0 &&
        g_str_has_suffix(ui_state.selected_track_path, ".qjams") &&
        !g_str_has_suffix(ui_state.selected_track_path, ".autosave.qjams")) {

        char *basename = g_path_get_basename(ui_state.selected_track_path);
        char detail[512];
        snprintf(detail, sizeof(detail),
                 "Overwrite existing session '%s', or save as a new session file?", basename);
        g_free(basename);

        GtkAlertDialog *alert = gtk_alert_dialog_new("Save Session");
        gtk_alert_dialog_set_detail(alert, detail);
        const char *buttons[] = {"Save", "Save As...", "Cancel", NULL};
        gtk_alert_dialog_set_buttons(alert, buttons);
        gtk_alert_dialog_set_default_button(alert, 0);
        gtk_alert_dialog_set_cancel_button(alert, 2);

        gtk_alert_dialog_choose(alert, parent, NULL, on_existing_session_save_response, parent);
        g_object_unref(alert);
    } else {
        open_save_session_file_dialog(parent);
    }
}

static gboolean deferred_session_redraw(gpointer user_data) {
    (void)user_data;

    invalidate_waveform_caches();
    if (waveform_area_bt)
        gtk_widget_queue_draw(waveform_area_bt);
    if (waveform_area_input)
        gtk_widget_queue_draw(waveform_area_input);

    return G_SOURCE_REMOVE;
}

static void load_session_thread(GTask *task, gpointer source_object, gpointer task_data,
                                GCancellable *cancellable) {
    (void)source_object;
    (void)cancellable;
    char *path = (char *)task_data;
    int result = load_qjams_session(path);
    g_task_return_int(task, result);
}

static void load_session_ready(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    (void)source_object;
    (void)user_data;
    GError *error = NULL;
    int load_status = g_task_propagate_int(G_TASK(res), &error);
    char *path = (char *)g_task_get_task_data(G_TASK(res));

    GtkWindow *win =
        gtk_application_get_active_window(GTK_APPLICATION(g_application_get_default()));
    if (win) {
        GtkWidget *master_box = gtk_window_get_child(win);
        if (master_box)
            gtk_widget_set_sensitive(master_box, TRUE);
    }

    gtk_spinner_stop(GTK_SPINNER(main_spinner));
    set_app_waiting_state(false);

    if (load_status >= 0) {
        ui_state.is_existing_session = true;
        mark_session_saved();

        strncpy(ui_state.selected_track_path, path, sizeof(ui_state.selected_track_path) - 1);
        ui_state.selected_track_path[sizeof(ui_state.selected_track_path) - 1] = '\0';
        update_playlist_toggle_state();

        refresh_multitrack_tracks_ui();
        update_multitrack_status_ui();

        char *basename = g_path_get_basename(path);
        char *escaped_base = g_markup_escape_text(basename, -1);
        if (load_status == 1) {
            snprintf(ui_state.workspace_status, sizeof(ui_state.workspace_status),
                     "Status: Session '%s' loaded (WARNING: Truncated)", escaped_base);
        } else {
            snprintf(ui_state.workspace_status, sizeof(ui_state.workspace_status),
                     "Status: Session '%s' loaded", escaped_base);
        }
        g_free(escaped_base);
        log_and_notify(1, "%s", ui_state.workspace_status);
        int active_rate =
            atomic_load_explicit(&get_transport_state()->active_sample_rate, memory_order_acquire);
        size_t total_frames = atomic_load_explicit(&get_transport_state()->backing_track_frames,
                                                   memory_order_acquire);
        int total_secs = (active_rate > 0) ? (total_frames / active_rate) : 0;

        char ui_text[600];
        snprintf(ui_text, sizeof(ui_text), "Track: %s [%02d:%02d]", basename, total_secs / 60,
                 total_secs % 60);
        gtk_label_set_text(GTK_LABEL(lbl_track), ui_text);

        sync_speed_ui_label(
            atomic_load_explicit(&get_transport_state()->playback_speed, memory_order_acquire));
        update_zoom_button_label_to_length();
        g_free(basename);

        gtk_widget_set_sensitive(btn_play, TRUE);
        gtk_widget_set_sensitive(btn_record, TRUE);
        if (btn_record_video)
            gtk_widget_set_sensitive(btn_record_video, is_camera_detected());

        if (has_active_live_video()) {
            launch_mkv_video_preview(current_raw_path);
        } else if (media_has_video(ui_state.selected_track_path) &&
                   access(ui_state.selected_track_path, F_OK) == 0) {
            launch_mkv_video_preview(ui_state.selected_track_path);
        }

        g_idle_add(deferred_session_redraw, NULL);

    } else {
        if (load_status == -5 || load_status == -6) {
            log_and_notify(1, "<span foreground='#ff4444'><b>Status: Insufficient RAM to load "
                              "session</b></span>");
            if (win) {
                GtkAlertDialog *alert = gtk_alert_dialog_new("Insufficient Memory");
                gtk_alert_dialog_set_detail(
                    alert, "Your system does not have enough free RAM to uncompress and load this "
                           "session into the real-time audio canvas. Please free up memory or load "
                           "a smaller session.");
                gtk_alert_dialog_show(alert, win);
                g_object_unref(alert);
            }
        } else if (load_status == -3) {
            log_and_notify(1, "<span foreground='#ff4444'><b>Status: Session Corrupt or "
                              "Unsupported Version</b></span>");
        } else {
            log_and_notify(1, "Status: Failed to load session.");
        }
        gtk_label_set_text(GTK_LABEL(lbl_track), "Track: None Selected");
    }

    gtk_widget_set_sensitive(btn_load, TRUE);
    gtk_widget_set_sensitive(btn_prev, TRUE);
    gtk_widget_set_sensitive(btn_next, TRUE);
    gtk_widget_set_sensitive(btn_playlist_toggle, TRUE);

    if (btn_load_session)
        gtk_widget_set_sensitive(btn_load_session, TRUE);
}

void command_post_load_session(const char *path) {
    on_stop_clicked(NULL, NULL);
    zoom_multiplier = 1.0;
    if (waveform_adj) {
        gtk_adjustment_set_value(waveform_adj, 0.0);
    }
    abort_active_video_preview(false);
    atomic_store_explicit(&get_transport_state()->backing_track_frames, 0, memory_order_release);

    for (int i = 0; i < QJ_MAX_TRACKS; i++) {
        atomic_store_explicit(&master_tracks[i].has_audio, false, memory_order_release);
    }

    current_raw_path[0] = '\0';


    video_start_frame = 0;
    video_end_frame = 0;
    video_track_index = -1;

    atomic_store_explicit(&video_is_armed, false, memory_order_release);

    GtkWindow *win =
        gtk_application_get_active_window(GTK_APPLICATION(g_application_get_default()));
    if (win) {
        GtkWidget *master_box = gtk_window_get_child(win);
        if (master_box)
            gtk_widget_set_sensitive(master_box, FALSE);
    }

    gtk_spinner_start(GTK_SPINNER(main_spinner));
    log_and_notify(1, "Status: Extracting session to RAM...");
    gtk_label_set_text(GTK_LABEL(lbl_track), "Track: Loading...");

    gtk_widget_set_sensitive(btn_load, FALSE);
    gtk_widget_set_sensitive(btn_prev, FALSE);
    gtk_widget_set_sensitive(btn_next, FALSE);
    gtk_widget_set_sensitive(btn_play, FALSE);
    gtk_widget_set_sensitive(btn_record, FALSE);
    if (btn_record_video)
        gtk_widget_set_sensitive(btn_record_video, FALSE);
    gtk_widget_set_sensitive(btn_playlist_toggle, FALSE);

    if (btn_load_session)
        gtk_widget_set_sensitive(btn_load_session, FALSE);

    set_app_waiting_state(true);
    GTask *task = g_task_new(NULL, NULL, load_session_ready, NULL);
    g_task_set_task_data(task, g_strdup(path), g_free);
    g_task_run_in_thread(task, load_session_thread);
    g_object_unref(task);
}

static void on_load_session_file_chosen(GObject *source_object, GAsyncResult *res,
                                        gpointer user_data) {
    (void)user_data;
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source_object);
    GError *error = NULL;
    GFile *file = gtk_file_dialog_open_finish(dialog, res, &error);
    if (file) {
        char *path = g_file_get_path(file);
        command_post_load_session(path);
        g_free(path);
        g_object_unref(file);
    } else if (error) {
        g_error_free(error);
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
    g_object_unref(filter);
    g_object_unref(filters);
    gtk_file_dialog_open(dialog, GTK_WINDOW(window), NULL, on_load_session_file_chosen, NULL);
    g_object_unref(dialog);
}

static void on_load_session_overwrite_confirm(GObject *source_object, GAsyncResult *res,
                                              gpointer user_data) {
    GtkAlertDialog *alert = GTK_ALERT_DIALOG(source_object);
    if (gtk_alert_dialog_choose_finish(alert, res, NULL) == 0) {
        execute_load_session_dialog(GTK_WIDGET(user_data));
    }
}

static void on_load_session_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    bool has_any_audio = false;
    for (int i = 0; i < QJ_MAX_TRACKS; i++) {
        if (atomic_load_explicit(&master_tracks[i].has_audio, memory_order_acquire)) {
            has_any_audio = true;
            break;
        }
    }

    if (has_any_audio && is_session_dirty() && pristine_frames > 0) {
        GtkAlertDialog *alert = gtk_alert_dialog_new("Overwrite Active Session?");
        gtk_alert_dialog_set_detail(
            alert, "Loading a new session will discard your currently recorded audio. Continue?");
        const char *buttons[] = {"Discard & Load", "Cancel", NULL};
        gtk_alert_dialog_set_buttons(alert, buttons);
        gtk_alert_dialog_set_cancel_button(alert, 1);
        gtk_alert_dialog_set_default_button(alert, 1);
        gtk_alert_dialog_choose(alert, GTK_WINDOW(user_data), NULL,
                                on_load_session_overwrite_confirm, user_data);
        g_object_unref(alert);
    } else {
        execute_load_session_dialog(GTK_WIDGET(user_data));
    }
}

static void on_load_playlist_file_chosen(GObject *source_object, GAsyncResult *res,
                                         gpointer user_data) {
    (void)user_data;
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source_object);
    GError *error = NULL;
    GFile *file = gtk_file_dialog_open_finish(dialog, res, &error);
    if (file) {
        char *path = g_file_get_path(file);
        strncpy(ui_state.playlist_path, path, sizeof(ui_state.playlist_path) - 1);
        strncpy(ui_state.config.last_playlist_path, path,
                sizeof(ui_state.config.last_playlist_path) - 1);
        save_qjams_config(ui_state.config_path, &ui_state.config);
        load_playlist_from_file(path);
        g_free(path);
        g_object_unref(file);
    } else if (error) {
        g_error_free(error);
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
    g_object_unref(filter);
    g_object_unref(filters);
    g_object_unref(dialog);
}

static void on_save_playlist_file_chosen(GObject *source_object, GAsyncResult *res,
                                         gpointer user_data) {
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
        strncpy(ui_state.config.last_playlist_path, path,
                sizeof(ui_state.config.last_playlist_path) - 1);
        save_qjams_config(ui_state.config_path, &ui_state.config);
        save_current_playlist();
        log_and_notify(1, "Status: Playlist Saved");
        g_free(path);
        g_object_unref(file);
    } else if (error) {
        g_error_free(error);
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
    g_object_unref(filter);
    g_object_unref(filters);
    g_object_unref(dialog);
}

void ui_command_post_init(GtkBuilder *b_cmd, GtkWindow *window) {
    btn_load = GTK_WIDGET(gtk_builder_get_object(b_cmd, "btn_load"));
    g_signal_connect(btn_load, "clicked", G_CALLBACK(on_select_track_clicked), window);

    btn_load_session = GTK_WIDGET(gtk_builder_get_object(b_cmd, "btn_load_session"));
    g_signal_connect(btn_load_session, "clicked", G_CALLBACK(on_load_session_clicked), window);

    btn_load_playlist = GTK_WIDGET(gtk_builder_get_object(b_cmd, "btn_load_playlist"));
    g_signal_connect(btn_load_playlist, "clicked", G_CALLBACK(on_load_playlist_clicked), window);

    btn_save_playlist = GTK_WIDGET(gtk_builder_get_object(b_cmd, "btn_save_playlist"));
    g_signal_connect(btn_save_playlist, "clicked", G_CALLBACK(on_save_playlist_clicked), window);

    btn_save_mux = GTK_WIDGET(gtk_builder_get_object(b_cmd, "btn_save_mux"));
    g_signal_connect(btn_save_mux, "clicked", G_CALLBACK(on_save_mux_clicked), NULL);

    btn_save_session = GTK_WIDGET(gtk_builder_get_object(b_cmd, "btn_save_session"));
    g_signal_connect(btn_save_session, "clicked", G_CALLBACK(on_save_session_clicked), window);

    btn_settings = GTK_WIDGET(gtk_builder_get_object(b_cmd, "btn_settings"));
    g_signal_connect(btn_settings, "clicked", G_CALLBACK(on_settings_clicked), window);
}

static void on_audio_recovery_response(GObject *source_object, GAsyncResult *res,
                                       gpointer user_data) {
    GtkAlertDialog *alert = GTK_ALERT_DIALOG(source_object);
    int response = gtk_alert_dialog_choose_finish(alert, res, NULL);
    char *autosave_path = (char *)user_data;

    if (response == 0) {
        command_post_load_session(autosave_path);
    } else {
        remove(autosave_path);
    }
    g_free(autosave_path);
}

/**
 * @brief Handles user response to the orphaned video crash-recovery alert dialog.
 *        Imports the recovered MKV directly onto the active track without pre-populating
 *        current_raw_path, preventing abort_active_video_preview() from unlinking the file.
 */
static void on_video_recovery_response(GObject *source_object, GAsyncResult *res,
                                       gpointer user_data) {
    GtkAlertDialog *alert = GTK_ALERT_DIALOG(source_object);
    int response = gtk_alert_dialog_choose_finish(alert, res, NULL);
    char *raw_path = (char *)user_data;

    if (response == 0) {
        current_raw_path[0] = '\0';
        strncpy(ui_state.selected_track_path, raw_path, sizeof(ui_state.selected_track_path) - 1);
        ui_state.selected_track_path[sizeof(ui_state.selected_track_path) - 1] = '\0';

        int rec_track = atomic_load_explicit(&current_recording_track, memory_order_acquire);
        multitrack_import_file_async(raw_path, rec_track);

        if (btn_save_mux) {
            gtk_widget_remove_css_class(btn_save_mux, "needs-save");
            gtk_widget_add_css_class(btn_save_mux, "needs-save");
        }
    } else {
        remove(raw_path);
    }
    g_free(raw_path);
}

gboolean check_for_crash_recovery(gpointer user_data) {
    GtkWindow *window = GTK_WINDOW(user_data);

    char autosave_path[1024];
    snprintf(autosave_path, sizeof(autosave_path), "%s/.autosave.qjams",
             ui_state.config.recordings_dir);

    if (access(autosave_path, F_OK) == 0) {
        log_and_notify(
            0, "[RECOVERY] Orphaned autosave detected. Prompting user for session restore.\n");
        GtkAlertDialog *alert = gtk_alert_dialog_new("Recover Unsaved Session?");
        gtk_alert_dialog_set_detail(alert, "QJams recovered an unsaved multitrack session from a "
                                           "previous crash. Would you like to restore it?");
        const char *buttons[] = {"Restore Session", "Discard", NULL};
        gtk_alert_dialog_set_buttons(alert, buttons);
        gtk_alert_dialog_set_cancel_button(alert, 1);
        gtk_alert_dialog_set_default_button(alert, 0);

        gtk_alert_dialog_choose(alert, window, NULL, on_audio_recovery_response,
                                g_strdup(autosave_path));
        g_object_unref(alert);
        return G_SOURCE_REMOVE;
    }

    GDir *dir = g_dir_open("/tmp", 0, NULL);
    if (dir) {
        const char *filename;
        char latest_raw[1024] = "";
        time_t latest_time = 0;

        while ((filename = g_dir_read_name(dir)) != NULL) {
            if (g_str_has_prefix(filename, "qjams_raw_") && g_str_has_suffix(filename, ".mkv")) {
                char full_path[1024];
                snprintf(full_path, sizeof(full_path), "/tmp/%s", filename);

                struct stat st;
                if (stat(full_path, &st) == 0) {
                    if (st.st_mtime > latest_time) {
                        if (strlen(latest_raw) > 0)
                            remove(latest_raw);
                        latest_time = st.st_mtime;
                        strncpy(latest_raw, full_path, sizeof(latest_raw) - 1);
                    } else {
                        remove(full_path);
                    }
                }
            }
        }
        g_dir_close(dir);

        if (strlen(latest_raw) > 0) {
            log_and_notify(
                0,
                "[RECOVERY] Orphaned un-exported video detected. Prompting user for recovery.\n");
            GtkAlertDialog *alert = gtk_alert_dialog_new("Recover Un-Exported Video?");
            gtk_alert_dialog_set_detail(alert, "QJams recovered an un-exported video recording. "
                                               "Would you like to load it for export?");

            const char *buttons[] = {"Restore Recording", "Discard", NULL};

            gtk_alert_dialog_set_buttons(alert, buttons);
            gtk_alert_dialog_set_cancel_button(alert, 1);
            gtk_alert_dialog_set_default_button(alert, 0);

            gtk_alert_dialog_choose(alert, window, NULL, on_video_recovery_response,
                                    g_strdup(latest_raw));
            g_object_unref(alert);
        }
    }

    return G_SOURCE_REMOVE;
}

static void autosave_thread(GTask *task, gpointer source_object, gpointer task_data,
                            GCancellable *cancellable) {
    (void)source_object;
    (void)cancellable;
    char *path = (char *)task_data;

    int result = save_qjams_session(path);

    g_task_return_int(task, result);
}

static void autosave_ready(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    (void)source_object;
    (void)user_data;
    GError *error = NULL;
    int status = g_task_propagate_int(G_TASK(res), &error);

    if (main_spinner)
        gtk_spinner_stop(GTK_SPINNER(main_spinner));

    // Communicate status without stealing focus
    if (status == 0) {
        log_and_notify(1, "Status: Autosave Complete");
    } else {
        log_and_notify(1, "Status: Autosave Failed");
    }
}

gboolean idle_autosave_tick(gpointer user_data) {
    (void)user_data;
    static int last_autosave_head = -1;

    if (main_spinner && gtk_spinner_get_spinning(GTK_SPINNER(main_spinner))) {
        return G_SOURCE_CONTINUE;
    }

    bool is_playing =
        atomic_load_explicit(&get_transport_state()->is_playing, memory_order_acquire);
    bool is_recording =
        atomic_load_explicit(&get_transport_state()->is_recording, memory_order_acquire);

    if (is_recording || is_playing) {
        return G_SOURCE_CONTINUE;
    }

    if (!is_session_dirty() || pristine_frames == 0) {
        return G_SOURCE_CONTINUE;
    }

    if (global_undo_ledger.head == last_autosave_head) {
        return G_SOURCE_CONTINUE;
    }
    last_autosave_head = global_undo_ledger.head;

    char autosave_path[1024];
    snprintf(autosave_path, sizeof(autosave_path), "%s/.autosave.qjams",
             ui_state.config.recordings_dir);

    if (main_spinner)
        gtk_spinner_start(GTK_SPINNER(main_spinner));
    log_and_notify(1, "Status: Autosaving in background...");

    GTask *task = g_task_new(NULL, NULL, autosave_ready, NULL);
    g_task_set_task_data(task, g_strdup(autosave_path), g_free);
    g_task_run_in_thread(task, autosave_thread);
    g_object_unref(task);

    return G_SOURCE_CONTINUE;
}
