#include "ui_dashboard.h"
#include "ui_globals.h"
#include "audio_engine.h"
#include "ui_waveforms.h"
#include "ui_playlist.h"
#include "ui_multitrack.h"
#include "encoder.h"
#include "scanner.h"
#include "video_engine.h"
#include "ringbuffer.h"
#include <math.h>
#include <string.h>
#include <time.h>
#include <stdatomic.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <unistd.h>

extern jack_client_t *client;
extern SPSC_Video_Queue video_queue;
extern SPSC_Preview_Queue preview_queue;

static pthread_t mkv_video_thread;
_Atomic bool mkv_video_keep_running = false;
_Atomic bool suppress_video_overwrite = false;
_Atomic bool is_loop_wrapping = false;
static uint8_t* mkv_rgba_pool[32] = {NULL};

char final_save_path[1024] = "";
char current_raw_path[1024] = "";
char stashed_raw_path[1024] = "";

_Atomic bool is_mkv_mode = false;

size_t video_start_frame = 0;
size_t video_end_frame = 0;

static guint speed_debounce_id = 0;
static float pending_speed = 1.0f;
static guint port_update_debounce_id = 0;

static GtkWidget *btn_telemetry = NULL;
static GtkWidget *telemetry_popover = NULL;
GtkWidget *btn_record_video = NULL;

static gboolean apply_port_update_deferred(gpointer user_data) {
    (void)user_data;

    // GUARD: Ignore port routing spam from PipeWire until the canvas is fully allocated
    extern size_t pristine_frames;
    if (pristine_frames == 0) {
        port_update_debounce_id = 0;
        return G_SOURCE_REMOVE;
    }

    update_dashboard_pref_ui();
    port_update_debounce_id = 0;
    return G_SOURCE_REMOVE;
}

gboolean on_jack_port_registration_ui(gpointer data) {
    (void)data;
    if (port_update_debounce_id != 0) {
        g_source_remove(port_update_debounce_id);
    }
    port_update_debounce_id = g_timeout_add(500, apply_port_update_deferred, NULL);
    return G_SOURCE_REMOVE;
}

static guint countdown_timer_id = 0;
static int countdown_val = 3;
static GtkWidget *countdown_window = NULL;
static GtkWidget *countdown_label = NULL;

static GtkWidget *input_pref_drop = NULL;
static GtkStringList *pref_model = NULL;
static bool pref_is_updating = false;

static void on_input_gain_changed(GtkSpinButton *spin_button, gpointer user_data);

static void update_hardware_status_label(void) {
    AudioDevice active_devs[MAX_AUDIO_DEVICES];
    int online_count = scan_audio_devices(client, active_devs, MAX_AUDIO_DEVICES);

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
        if (!dev_name || strlen(dev_name) == 0) continue;

        if (i == 1 && primary_dev && strcmp(dev_name, primary_dev) == 0) continue;

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
        if (toast_msg[len - 1] == '\n') toast_msg[len - 1] = '\0';
        log_and_notify(2, "%s", toast_msg);
    } else {
        gtk_widget_set_visible(lbl_hw_toast, FALSE);
    }
}

void update_dashboard_pref_ui(void) {
    if (!input_pref_drop) return;
    pref_is_updating = true;

    if (pref_model) {
        g_object_unref(pref_model);
    }
    pref_model = gtk_string_list_new(NULL);

    int active_idx = -1;
    int count = 0;

    AudioDevice active_devs[MAX_AUDIO_DEVICES];
    int online_count = scan_audio_devices(client, active_devs, MAX_AUDIO_DEVICES);

    update_hardware_status_label();

    // 1. Identify key profiles
    const char *primary_dev = NULL;
    const char *fallback_dev = NULL;
    for (int i = 0; i < ui_state.config.num_audio_profiles; i++) {
        if (ui_state.config.audio_profiles[i].is_primary) primary_dev = ui_state.config.audio_profiles[i].device_name;
        if (ui_state.config.audio_profiles[i].is_fallback) fallback_dev = ui_state.config.audio_profiles[i].device_name;
    }

    // 2. Assess online state
    bool current_is_online = false;
    bool primary_is_online = false;
    bool fallback_is_online = false;

    for (int j = 0; j < online_count; j++) {
        if (strcmp(ui_state.config.audio_device, active_devs[j].display_name) == 0) current_is_online = true;
        if (primary_dev && strcmp(primary_dev, active_devs[j].display_name) == 0) primary_is_online = true;
        if (fallback_dev && strcmp(fallback_dev, active_devs[j].display_name) == 0) fallback_is_online = true;
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
        if (atomic_load_explicit(&engine_is_recording, memory_order_acquire) ||
            atomic_load_explicit(&engine_is_playing, memory_order_acquire)) {
            on_stop_clicked(NULL, NULL);
            }
            extern GtkWidget *lbl_status;
        if (lbl_status) {
            log_and_notify(1, "<span foreground='#ff4444'><b>Device cannot be recovered at the moment. Advise: Boot your PC and/or device.</b></span>");
        }
        gtk_drop_down_set_model(GTK_DROP_DOWN(input_pref_drop), G_LIST_MODEL(pref_model));
        pref_is_updating = false;
        return;
    }

    // 5. Handle Valid Switch (Dropout or Recovery)
    if (strcmp(target_device, ui_state.config.audio_device) != 0) {

        // If the old device dropped offline mid-session, halt transport safely
        if (did_dropout && (atomic_load_explicit(&engine_is_recording, memory_order_acquire) ||
            atomic_load_explicit(&engine_is_playing, memory_order_acquire))) {
            on_stop_clicked(NULL, NULL);
            }

            extern int patch_audio_ports(const char*);
        extern int patch_playback_ports(const char*);
        patch_audio_ports(target_device);
        patch_playback_ports(ui_state.config.playback_target);
        strncpy(ui_state.config.audio_device, target_device, sizeof(ui_state.config.audio_device) - 1);
        ui_state.config.audio_device[sizeof(ui_state.config.audio_device) - 1] = '\0';

        float target_multiplier = 1.0f;
        for (int i = 0; i < ui_state.config.num_audio_profiles; i++) {
            if (strcmp(ui_state.config.audio_profiles[i].device_name, target_device) == 0) {
                target_multiplier = ui_state.config.audio_profiles[i].gain_multiplier;
                break;
            }
        }

        extern void set_input_gain(float);
        ui_state.config.input_gain_multiplier = target_multiplier;
        set_input_gain(target_multiplier);

        float loaded_db = (target_multiplier <= 0.001f) ? -24.0f : 20.0f * log10f(target_multiplier);
        extern void on_input_gain_changed(GtkSpinButton*, gpointer);
        g_signal_handlers_block_by_func(input_gain_spinner, G_CALLBACK(on_input_gain_changed), NULL);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(input_gain_spinner), loaded_db);
        g_signal_handlers_unblock_by_func(input_gain_spinner, G_CALLBACK(on_input_gain_changed), NULL);

        if (current_is_online) { // Only show recovery toast if it didn't just crash out of a session
            log_and_notify(2, "Auto-Switched to: %s", target_device);
        }

        save_qjams_config(ui_state.config_path, &ui_state.config);
    }

    // 6. Build Dropdown
    for (int i = 0; i < ui_state.config.num_audio_profiles; i++) {
        if (ui_state.config.audio_profiles[i].in_pref) {
            bool is_online = false;
            for (int j = 0; j < online_count; j++) {
                if (strcmp(ui_state.config.audio_profiles[i].device_name, active_devs[j].display_name) == 0) {
                    is_online = true;
                    break;
                }
            }

            if (is_online) {
                gtk_string_list_append(pref_model, ui_state.config.audio_profiles[i].device_name);
                if (strcmp(ui_state.config.audio_profiles[i].device_name, target_device) == 0) {
                    active_idx = count;
                }
                count++;
            }
        }
    }

    if (count == 0 || active_idx == -1) {
        gtk_string_list_append(pref_model, target_device);
        active_idx = count;
    }

    gtk_drop_down_set_model(GTK_DROP_DOWN(input_pref_drop), G_LIST_MODEL(pref_model));
    gtk_drop_down_set_selected(GTK_DROP_DOWN(input_pref_drop), active_idx);

    pref_is_updating = false;
}

static void on_pref_selection_changed(GObject *gobject, GParamSpec *pspec, gpointer user_data) {
    (void)pspec; (void)user_data;
    if (pref_is_updating) return;

    guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(gobject));
    if (selected == GTK_INVALID_LIST_POSITION) return;

    const char *new_device = gtk_string_list_get_string(pref_model, selected);
    if (!new_device) return;

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

void update_zoom_button_label_to_length(void) {
    if (!btn_zoom) return;
    size_t frames = atomic_load_explicit(&backing_track_frames, memory_order_acquire);
    int rate = atomic_load_explicit(&active_sample_rate, memory_order_acquire);
    if (frames > 0 && rate > 0) {
        int total_secs = frames / rate;
        char len_str[64];
        if (total_secs >= 3600) {
            snprintf(len_str, sizeof(len_str), "Length %d:%02d:%02d", total_secs / 3600, (total_secs % 3600) / 60, total_secs % 60);
        } else {
            snprintf(len_str, sizeof(len_str), "Length %d:%02d", total_secs / 60, total_secs % 60);
        }
        gtk_button_set_label(GTK_BUTTON(btn_zoom), len_str);
    } else {
        gtk_button_set_label(GTK_BUTTON(btn_zoom), "Length 0:00");
    }
}

static gboolean apply_speed_deferred(gpointer user_data) {
    (void)user_data;
    set_playback_speed(pending_speed);
    speed_debounce_id = 0;
    return G_SOURCE_REMOVE;
}

void on_speed_clicked(GtkButton *button, gpointer user_data) {
    (void)user_data;
    if (atomic_load_explicit(&engine_is_recording, memory_order_acquire)) return;

    const gchar *current_label = gtk_button_get_label(button);

    if (g_strcmp0(current_label, "Speed: 1.0x") == 0) {
        pending_speed = 0.8f;
        gtk_button_set_label(button, "Speed: 0.8x");
    } else if (g_strcmp0(current_label, "Speed: 0.8x") == 0) {
        pending_speed = 0.6f;
        gtk_button_set_label(button, "Speed: 0.6x");
    } else if (g_strcmp0(current_label, "Speed: 0.6x") == 0) {
        pending_speed = 1.2f;
        gtk_button_set_label(button, "Speed: 1.2x");
    } else {
        pending_speed = 1.0f;
        gtk_button_set_label(button, "Speed: 1.0x");
    }

    if (speed_debounce_id != 0) g_source_remove(speed_debounce_id);
    speed_debounce_id = g_timeout_add(500, apply_speed_deferred, NULL);
}

static void async_crop_thread(GTask *task, gpointer source, gpointer task_data, GCancellable *c) {
    (void)source; (void)task_data; (void)c;
    extern void crop_canvas(void);
    crop_canvas();
    g_task_return_boolean(task, TRUE);
}

static void async_crop_ready(GObject *source, GAsyncResult *res, gpointer data) {
    (void)source; (void)data;
    g_task_propagate_boolean(G_TASK(res), NULL);

    extern void mark_session_dirty(void);
    extern void invalidate_waveform_caches(void);
    extern double zoom_multiplier;
    extern void set_app_waiting_state(bool);

    mark_session_dirty();
    zoom_multiplier = 1.0;

    update_zoom_button_label_to_length();
    invalidate_waveform_caches();

    extern GtkWidget *waveform_area_bt;
    extern GtkWidget *waveform_area_input;
    if (waveform_area_bt) gtk_widget_queue_draw(waveform_area_bt);
    if (waveform_area_input) gtk_widget_queue_draw(waveform_area_input);

    extern GtkWidget *main_spinner;
    if (main_spinner) gtk_spinner_stop(GTK_SPINNER(main_spinner));
    set_app_waiting_state(false);

    GtkWindow *win = gtk_application_get_active_window(GTK_APPLICATION(g_application_get_default()));
    if (win) {
        GtkWidget *master_box = gtk_window_get_child(win);
        if (master_box) gtk_widget_set_sensitive(master_box, TRUE);
    }

    log_and_notify(1, "Status: Canvas cropped to recording length");
}

static void on_crop_confirm(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    (void)user_data;
    GtkAlertDialog *alert = GTK_ALERT_DIALOG(source_object);
    if (gtk_alert_dialog_choose_finish(alert, res, NULL) == 0) {
        GtkWindow *win = gtk_application_get_active_window(GTK_APPLICATION(g_application_get_default()));
        if (win) {
            GtkWidget *master_box = gtk_window_get_child(win);
            if (master_box) gtk_widget_set_sensitive(master_box, FALSE);
        }

        extern GtkWidget *main_spinner;
        if (main_spinner) gtk_spinner_start(GTK_SPINNER(main_spinner));

        extern void set_app_waiting_state(bool);
        set_app_waiting_state(true);

        GTask *task = g_task_new(NULL, NULL, async_crop_ready, NULL);
        g_task_run_in_thread(task, async_crop_thread);
        g_object_unref(task);
    }
}

static void on_crop_clicked(GtkButton *button, gpointer user_data) {
    (void)user_data;
    if (atomic_load_explicit(&engine_is_recording, memory_order_acquire)) return;

    GtkAlertDialog *alert = gtk_alert_dialog_new("Crop track to recording length?");
    gtk_alert_dialog_set_detail(alert, "This will shrink the session length to fit your active audio (plus a 2-second reverb tail). Are you sure?");

    const char *buttons[] = { "Crop Canvas", "Cancel", NULL };
    gtk_alert_dialog_set_buttons(alert, buttons);
    gtk_alert_dialog_set_cancel_button(alert, 1);
    gtk_alert_dialog_set_default_button(alert, 0);

    GtkWindow *parent = GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(button)));
    gtk_alert_dialog_choose(alert, parent, NULL, on_crop_confirm, NULL);
    g_object_unref(alert);
}

void prepare_engine_for_new_track(void) {
    on_stop_clicked(NULL, NULL);
    clear_loop_points();

    if (btn_save_mux) gtk_widget_remove_css_class(btn_save_mux, "needs-save");
    if (btn_save_session) gtk_widget_remove_css_class(btn_save_session, "needs-save");

    if (speed_debounce_id != 0) {
        g_source_remove(speed_debounce_id);
        speed_debounce_id = 0;
    }
    pending_speed = 1.0f;
    set_playback_speed(1.0f);
    if (btn_speed) gtk_button_set_label(GTK_BUTTON(btn_speed), "Speed: 1.0x");

    extern int resize_loop_canvas_seconds(int);
    int limit_min = ui_state.config.default_canvas_limit_min > 0 ? ui_state.config.default_canvas_limit_min : 5;
    resize_loop_canvas_seconds(limit_min * 60);

    // Maintain the synchronized backing track bounds
    atomic_store_explicit(&current_recording_track, 0, memory_order_release);
    atomic_store_explicit(&active_track_count, 1, memory_order_release);
    atomic_store_explicit(&selected_tracks_mask, 1, memory_order_release);

    ui_state.is_existing_session = false;
    mark_session_saved();

    extern void update_zoom_button_label_to_length(void);
    update_zoom_button_label_to_length();

    extern void invalidate_waveform_caches(void);
    invalidate_waveform_caches();

    extern GtkWidget *waveform_area_bt;
    if (waveform_area_bt) gtk_widget_queue_draw(waveform_area_bt);
}

static void on_prev_track_clicked(GtkButton *button, gpointer user_data) {
    (void)button; (void)user_data;
    cycle_playlist_track(-1);
}

static void on_next_track_clicked(GtkButton *button, gpointer user_data) {
    (void)button; (void)user_data;
    cycle_playlist_track(1);
}

static void* mkv_video_loop(void *arg) {
    char *filepath = (char*)arg;
    AVFormatContext *fmt_ctx = NULL;

    AVDictionary *opts = NULL;
    av_dict_set(&opts, "probesize", "1048576", 0);       // 1MB max probe (down from 5MB)
    av_dict_set(&opts, "analyzeduration", "1000000", 0); // 1 second max analysis

    if (avformat_open_input(&fmt_ctx, filepath, NULL, &opts) != 0) {
        av_dict_free(&opts);
        free(filepath);
        return NULL;
    }
    av_dict_free(&opts);

    avformat_find_stream_info(fmt_ctx, NULL);

    if (!mkv_rgba_pool[0]) {
        for (int i = 0; i < 32; i++) mkv_rgba_pool[i] = malloc(1280 * 720 * 4);
    }

    int v_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (v_idx < 0) { avformat_close_input(&fmt_ctx); free(filepath); return NULL; }

    AVCodecContext *v_ctx = avcodec_alloc_context3(NULL);
    avcodec_parameters_to_context(v_ctx, fmt_ctx->streams[v_idx]->codecpar);

    // --- NEW: Enable CPU Multithreading ---
    v_ctx->thread_count = 0; // 0 = Auto-detect maximum logical cores
    v_ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;

    const AVCodec *v_codec = avcodec_find_decoder(v_ctx->codec_id);
    avcodec_open2(v_ctx, v_codec, NULL);

    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    struct SwsContext *sws_ctx = NULL;
    AVRational time_base = fmt_ctx->streams[v_idx]->time_base;
    double last_audio_time = -1.0;

    // MATCH LIVE CAMERA PREVIEW DIMENSIONS
    int target_w = 320;
    int target_h = 180;

    // --- DETECT LIVE TAKE OFFSET ---
    bool is_live_take = false;
    extern char current_raw_path[1024];
    if (strcmp(filepath, current_raw_path) == 0) {
        is_live_take = true;
    }

    while (atomic_load_explicit(&mkv_video_keep_running, memory_order_acquire)) {
        int current_rate = atomic_load_explicit(&active_sample_rate, memory_order_acquire);
        if (current_rate == 0) { usleep(10000); continue; }

        size_t pos = atomic_load_explicit(&playback_pos, memory_order_acquire);
        double current_audio_time = (double)pos / current_rate;

        double offset_sec = 0.0;
        if (is_live_take) {
            extern size_t video_start_frame;
            offset_sec = (double)video_start_frame / current_rate;
        }

        double local_audio_time = current_audio_time - offset_sec;

        // DYNAMIC V4L2 HANDOFF: Pure math, entirely decoupled from FFmpeg state
        bool inside_video = false;
        if (is_live_take) {
            extern size_t video_start_frame;
            extern size_t video_end_frame;
            extern _Atomic bool video_is_armed;
            size_t v_end = video_end_frame;
            if (atomic_load_explicit(&engine_is_recording, memory_order_acquire) &&
                atomic_load_explicit(&video_is_armed, memory_order_acquire)) {
                v_end = pos; // Follow playhead while actively recording
                }
                if (pos >= video_start_frame && pos <= v_end) inside_video = true;
        } else {
            extern size_t get_last_audible_frame(void);
            size_t last_f = get_last_audible_frame();
            if (last_f == 0) last_f = atomic_load_explicit(&backing_track_frames, memory_order_acquire);
            if (pos <= last_f) inside_video = true;
        }
        atomic_store_explicit(&is_mkv_mode, inside_video, memory_order_release);

        // Wait for the playhead to reach the video punch-in point
        if (local_audio_time < 0.0) {
            usleep(10000);
            continue;
        }

        // --- Track speed changes to force immediate decoder flush ---
        static float last_seen_speed = 1.0f;
        float current_speed_check = atomic_load_explicit(&playback_speed, memory_order_relaxed);
        bool speed_changed = (current_speed_check != last_seen_speed);
        last_seen_speed = current_speed_check;

        if (last_audio_time < 0.0 || fabs(local_audio_time - last_audio_time) > 0.25 || speed_changed) {
            PreviewPayload stale_payload;
            while (pop_preview_frame(&preview_queue, &stale_payload)) {}

            double target_mkv_time = local_audio_time * (current_speed_check > 0.0f ? current_speed_check : 1.0f);
            av_seek_frame(fmt_ctx, v_idx, (int64_t)(target_mkv_time / av_q2d(time_base)), AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers(v_ctx);
        }
        last_audio_time = local_audio_time;

        if (av_read_frame(fmt_ctx, pkt) >= 0) {
            if (pkt->stream_index == v_idx) {
                if (avcodec_send_packet(v_ctx, pkt) == 0) {
                    while (avcodec_receive_frame(v_ctx, frame) == 0) {
                        double raw_frame_time = frame->pts * av_q2d(time_base);
                        double frame_time = 0.0;
                        double sync_last_audio = local_audio_time;

                        while (atomic_load_explicit(&mkv_video_keep_running, memory_order_acquire)) {
                            float current_speed = atomic_load_explicit(&playback_speed, memory_order_relaxed);
                            frame_time = raw_frame_time / (current_speed > 0.0f ? current_speed : 1.0f);
                            size_t pos = atomic_load_explicit(&playback_pos, memory_order_acquire);
                            double inner_audio_time = (double)pos / current_rate;
                            double inner_local_time = inner_audio_time - offset_sec;

                            if (inner_local_time < sync_last_audio - 0.2) break;
                            sync_last_audio = inner_local_time;
                            double time_diff = frame_time - inner_local_time;
                            if (time_diff <= 0.05) break;
                            long long delay_ns = (long long)((time_diff - 0.05) * 1000000000LL);
                            if (delay_ns > 10000000LL) delay_ns = 10000000LL;
                            struct timespec now, target;
                            clock_gettime(CLOCK_MONOTONIC, &now);
                            target.tv_sec = now.tv_sec + (delay_ns / 1000000000LL);
                            target.tv_nsec = now.tv_nsec + (delay_ns % 1000000000LL);
                            if (target.tv_nsec >= 1000000000LL) { target.tv_sec++; target.tv_nsec -= 1000000000LL; }
                            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &target, NULL);
                        }

                        if (frame_time < local_audio_time - 0.1) continue;

                        if (!sws_ctx) {
                            double src_aspect = (double)frame->width / (double)frame->height;
                            double target_aspect = (double)target_w / (double)target_h;
                            int scaled_w = target_w;
                            int scaled_h = target_h;
                            if (src_aspect > target_aspect) scaled_h = (int)((double)target_w / src_aspect);
                            else scaled_w = (int)((double)target_h * src_aspect);
                            sws_ctx = sws_getContext(frame->width, frame->height, frame->format, scaled_w, scaled_h, AV_PIX_FMT_RGBA, SWS_FAST_BILINEAR, NULL, NULL, NULL);
                        }

                        // Mutes MKV playback rendering during active recording to prevent UI frame collision
                        // with the live V4L2 camera feed. The decoder remains synced silently in the background.
                        if (!atomic_load_explicit(&engine_is_recording, memory_order_acquire)) {
                            size_t p_w_idx = atomic_load_explicit(&preview_queue.write_index, memory_order_relaxed);
                            size_t p_r_idx = atomic_load_explicit(&preview_queue.read_index, memory_order_acquire);
                            if (p_w_idx - p_r_idx >= preview_queue.capacity) continue;

                            uint8_t *rgba_buf = mkv_rgba_pool[p_w_idx & 31];
                            memset(rgba_buf, 0, target_w * target_h * 4);
                            double src_aspect = (double)frame->width / (double)frame->height;
                            double target_aspect = (double)target_w / (double)target_h;
                            int scaled_w = target_w;
                            int scaled_h = target_h;
                            if (src_aspect > target_aspect) scaled_h = (int)((double)target_w / src_aspect);
                            else scaled_w = (int)((double)target_h * src_aspect);
                            int x_offset = (target_w - scaled_w) / 2;
                            int y_offset = (target_h - scaled_h) / 2;
                            uint8_t *dest_data[4] = { rgba_buf + (y_offset * target_w + x_offset) * 4, NULL, NULL, NULL };
                            int dest_linesize[4] = { target_w * 4, 0, 0, 0 };

                            sws_scale(sws_ctx, (const uint8_t * const *)frame->data, frame->linesize, 0, frame->height, dest_data, dest_linesize);
                            push_preview_frame(&preview_queue, rgba_buf, target_w, target_h);
                        }
                    }
                }
            }
            av_packet_unref(pkt);
        } else {
            size_t standby_pos = atomic_load_explicit(&playback_pos, memory_order_acquire);

            while (atomic_load_explicit(&mkv_video_keep_running, memory_order_acquire)) {
                size_t current_pos = atomic_load_explicit(&playback_pos, memory_order_acquire);
                int rate = atomic_load_explicit(&active_sample_rate, memory_order_acquire);
                if (rate <= 0) rate = 48000;

                // Dynamically maintain handoff while in standby
                bool inside_video = false;
                if (is_live_take) {
                    extern size_t video_start_frame;
                    extern size_t video_end_frame;
                    if (current_pos >= video_start_frame && current_pos <= video_end_frame) inside_video = true;
                } else {
                    extern size_t get_last_audible_frame(void);
                    size_t last_f = get_last_audible_frame();
                    if (last_f == 0) last_f = atomic_load_explicit(&backing_track_frames, memory_order_acquire);
                    if (current_pos <= last_f) inside_video = true;
                }
                atomic_store_explicit(&is_mkv_mode, inside_video, memory_order_release);

                // Detect seek by looking for playhead discontinuity (>100ms jump)
                if (current_pos < standby_pos || current_pos > standby_pos + (rate / 2)) {
                    PreviewPayload stale_payload;
                    while (pop_preview_frame(&preview_queue, &stale_payload)) {}

                    float current_speed = atomic_load_explicit(&playback_speed, memory_order_relaxed);
                    double new_local_time = ((double)current_pos / rate) - offset_sec;
                    double target_mkv_time = new_local_time * (current_speed > 0.0f ? current_speed : 1.0f);

                    int v_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
                    av_seek_frame(fmt_ctx, v_idx, (int64_t)(target_mkv_time / av_q2d(time_base)), AVSEEK_FLAG_BACKWARD);
                    avcodec_flush_buffers(v_ctx);

                    last_audio_time = new_local_time;
                    break;
                }
                standby_pos = current_pos;
                usleep(10000);
            }
        }
    }

    atomic_store_explicit(&is_mkv_mode, false, memory_order_release);
    avcodec_free_context(&v_ctx);
    avformat_close_input(&fmt_ctx);

    if (sws_ctx) sws_freeContext(sws_ctx);
    av_packet_free(&pkt);
    av_frame_free(&frame);

    free(filepath);
    return NULL;
}

/**
 * @brief Background thread to safely allocate the canvas.
 * @note Safety [Critical]: The RT thread must be alive and ticking before `init_empty_loop_canvas`
 * is allowed to attempt the `await_rt_thread_detach` lock-free handshake.
 */
static void init_canvas_thread(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable) {
    (void)source_object; (void)cancellable;
    int duration_sec = GPOINTER_TO_INT(task_data);

    // Give PipeWire an initial 500ms grace period to negotiate the hardware graph
    usleep(500000);

    int retries = 10;
    int status = -1;
    while (retries > 0) {
        extern int init_empty_loop_canvas(int);
        status = init_empty_loop_canvas(duration_sec);
        if (status == 0 || status == -2) break; // Success or Hard RAM limit reached

        usleep(500000); // Patiently wait 500ms and try again
        retries--;
    }
    g_task_return_int(task, status);
}

static void init_canvas_ready(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    (void)source_object; (void)user_data;
    GError *error = NULL;
    int status = g_task_propagate_int(G_TASK(res), &error);

    if (status == 0) {
        extern void update_multitrack_status_ui(void);
        update_multitrack_status_ui();
        gtk_widget_set_sensitive(btn_play, FALSE);
        gtk_widget_set_sensitive(btn_record, TRUE);

        mark_session_saved();
        extern void update_zoom_button_label_to_length(void);
        update_zoom_button_label_to_length();

        extern void invalidate_waveform_caches(void);
        invalidate_waveform_caches();
        if (waveform_area_bt) gtk_widget_queue_draw(waveform_area_bt);
        if (waveform_area_input) gtk_widget_queue_draw(waveform_area_input);

        if (lbl_status) log_and_notify(1, "Status: Ready");

    } else {
        if (lbl_status) {
            if (status == -2) {
                log_and_notify(1, "<span foreground='#ff4444'><b>Status: Insufficient RAM for blank canvas</b></span>");
            } else {
                log_and_notify(1, "<span foreground='#ff4444'><b>Status: Audio Engine Timeout (Failed to allocate canvas)</b></span>");
            }
        }
    }
}

void adjust_blank_canvas(void) {
    if (is_session_dirty()) return;

    if (lbl_status) log_and_notify(1, "Status: Starting Audio Engine...");

    int canvas_limit_min = ui_state.config.default_canvas_limit_min > 0 ? ui_state.config.default_canvas_limit_min : 5;
    int duration_sec = canvas_limit_min * 60;

    // Launch the non-blocking background allocation
    GTask *task = g_task_new(NULL, NULL, init_canvas_ready, NULL);
    g_task_set_task_data(task, GINT_TO_POINTER(duration_sec), NULL);
    g_task_run_in_thread(task, init_canvas_thread);
    g_object_unref(task);
}

static void start_recording_execution(void) {
    size_t current_bt_frames = atomic_load_explicit(&backing_track_frames, memory_order_acquire);
    size_t current_pos = atomic_load_explicit(&playback_pos, memory_order_acquire);

    if (current_bt_frames > 0 && current_pos >= current_bt_frames) {
        atomic_store_explicit(&playback_pos, 0, memory_order_release);
        seek_backing_track(0.0);
    }

    if (btn_save_mux) gtk_widget_remove_css_class(btn_save_mux, "needs-save");
    if (btn_save_session) gtk_widget_remove_css_class(btn_save_session, "needs-save");

    mark_session_dirty();

    if (current_bt_frames == 0) {
        int canvas_limit_min = ui_state.config.default_canvas_limit_min > 0 ? ui_state.config.default_canvas_limit_min : 5;

        if (init_empty_loop_canvas(canvas_limit_min * 60) != 0) {
            log_and_notify(1, "Status: Insufficient RAM for blank canvas.");
            return;
        }

        char track_lbl[128];
        snprintf(track_lbl, sizeof(track_lbl), "Track: Blank Loop Canvas [%02d:00 limit]", canvas_limit_min);
        gtk_label_set_text(GTK_LABEL(lbl_track), track_lbl);
        update_multitrack_status_ui();
        update_zoom_button_label_to_length();
    }

    time_t t = time(NULL);
    struct tm tm = *localtime(&t);

    char base_name[256] = "Untracked";
    if (strlen(ui_state.selected_track_path) > 0) {
        char *tmp = g_path_get_basename(ui_state.selected_track_path);
        char *dot = strrchr(tmp, '.');
        if (dot) *dot = '\0';
        snprintf(base_name, sizeof(base_name), "%s", tmp);
        g_free(tmp);
    }

    if (strlen(current_raw_path) > 0) {
        remove(current_raw_path);
    }

    clear_video_stash_async();

    snprintf(final_save_path, sizeof(final_save_path), "%s/%04d%02d%02d-%02d%02d%02d-%s-RAW.mkv",
             ui_state.config.recordings_dir, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec, base_name);

    snprintf(current_raw_path, sizeof(current_raw_path), "/tmp/qjams_raw_%04d%02d%02d_%02d%02d%02d.mkv",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
    int current_samplerate = client ? jack_get_sample_rate(client) : 48000;

    if (await_rt_thread_detach()) {
        int rec_track = atomic_load_explicit(&current_recording_track, memory_order_acquire);
        backup_track_to_ledger(rec_track);

        extern size_t pristine_frames;
        if (rec_track >= 0 && rec_track < MAX_TRACKS && master_tracks[rec_track].active_buffer && pristine_frames > 0) {
            memset(master_tracks[rec_track].active_buffer, 0, pristine_frames * 2 * sizeof(float));
        }

        resume_rt_thread();
    }

    // Unifed Encoder Gate: Only launch FFmpeg if the UI 'Arm Video' toggle is active
    if (atomic_load_explicit(&video_is_armed, memory_order_acquire)) {
        extern void mark_current_transaction_as_video(void);
        mark_current_transaction_as_video();

        video_start_frame = atomic_load_explicit(&playback_pos, memory_order_acquire);
        video_end_frame = 0;

        if (init_and_start_encoder(current_raw_path, capture_width, capture_height, current_samplerate, &video_queue) != 0) {
            log_and_notify(1, "Status: Encoder Failed");
            return;
        }
    }

    atomic_store_explicit(&engine_is_armed, true, memory_order_release);
    atomic_store_explicit(&engine_is_recording, true, memory_order_release);

    extern void trigger_global_ui_refresh(void);
    trigger_global_ui_refresh();

    if (!atomic_load_explicit(&engine_is_playing, memory_order_acquire)) {
        atomic_store_explicit(&engine_is_playing, true, memory_order_release);
    }

    // IN start_recording_execution() replace the button setup with this:
    log_and_notify(1, "Status: Recording");

    bool is_video = atomic_load_explicit(&video_is_armed, memory_order_acquire);

    if (is_video) {
        gtk_button_set_label(GTK_BUTTON(btn_record), "🎜");
        gtk_widget_set_sensitive(btn_record, FALSE);
        if (btn_record_video) {
            gtk_button_set_label(GTK_BUTTON(btn_record_video), "⏸");
            gtk_widget_set_tooltip_text(btn_record_video, "Pause Video Recording");
            gtk_widget_set_sensitive(btn_record_video, TRUE);
        }
    } else {
        gtk_button_set_label(GTK_BUTTON(btn_record), "⏸");
        gtk_widget_set_tooltip_text(btn_record, "Pause Audio Recording");
        gtk_widget_set_sensitive(btn_record, TRUE);
        if (btn_record_video) {
            gtk_button_set_label(GTK_BUTTON(btn_record_video), "🖵");
            gtk_widget_set_sensitive(btn_record_video, FALSE);
        }
    }

    gtk_widget_set_sensitive(btn_load, FALSE);
    gtk_widget_set_sensitive(btn_settings, FALSE);
    gtk_widget_set_sensitive(btn_prev, FALSE);
    gtk_widget_set_sensitive(btn_next, FALSE);
    gtk_widget_set_sensitive(btn_speed, FALSE);
    gtk_widget_set_sensitive(btn_save_mux, FALSE);
    gtk_widget_set_sensitive(btn_play, FALSE);
    gtk_widget_set_sensitive(btn_stop, TRUE);

    if (btn_play) gtk_widget_grab_focus(btn_play);
}

// countdown timer for inside canvas loop
bool is_countdown_active(void) {
    return countdown_timer_id != 0;
}

static gboolean countdown_tick(gpointer user_data) {
    (void)user_data;
    EngineCommand cmd = { .type = CMD_PLAY_CLICK, .target_track = 0 };
    extern SPSC_Command_Queue cmd_queue;
    extern int push_command(SPSC_Command_Queue*, EngineCommand);

    if (countdown_val > 0) {
        char buf[128];
        snprintf(buf, sizeof(buf), "<span size='48000' weight='heavy'>%d</span>", countdown_val);
        gtk_label_set_markup(GTK_LABEL(countdown_label), buf);

        push_command(&cmd_queue, cmd);

        countdown_val--;
        return G_SOURCE_CONTINUE;
    } else if (countdown_val == 0) {
        gtk_label_set_markup(GTK_LABEL(countdown_label), "<span size='48000' weight='heavy' foreground='#ff4444'>GO!</span>");

        push_command(&cmd_queue, cmd);

        countdown_val--;
        return G_SOURCE_CONTINUE;
    } else {
        start_recording_execution();
        countdown_timer_id = 0;

        if (countdown_window) {
            gtk_window_destroy(GTK_WINDOW(countdown_window));
            countdown_window = NULL;
        }

        return G_SOURCE_REMOVE;
    }
}

static void on_countdown_destroyed(GtkWidget *widget, gpointer user_data) {
    (void)widget; (void)user_data;
    if (countdown_timer_id != 0) {
        g_source_remove(countdown_timer_id);
        countdown_timer_id = 0;
    }
    countdown_window = NULL;
    countdown_label = NULL;

    if (!atomic_load_explicit(&engine_is_playing, memory_order_acquire) && gtk_widget_is_sensitive(btn_stop)) {
        log_and_notify(1, "Status: Recording Aborted");
        gtk_widget_set_sensitive(btn_record, TRUE);
        if (btn_record_video) gtk_widget_set_sensitive(btn_record_video, TRUE); // FIX: Restore video button state
        gtk_widget_set_sensitive(btn_play, TRUE);
        gtk_widget_set_sensitive(btn_stop, FALSE);
    }
}

static gboolean on_countdown_key_pressed(GtkEventControllerKey *controller, guint keyval, guint keycode, GdkModifierType state, gpointer user_data) {
    (void)controller; (void)keycode; (void)state; (void)user_data;
    if (keyval == GDK_KEY_Escape) {
        if (countdown_window) {
            gtk_window_destroy(GTK_WINDOW(countdown_window));
            countdown_window = NULL;
        }
        return TRUE;
    }
    return FALSE;
}

void on_start_clicked(GtkButton *button, gpointer user_data) {
    (void)button; (void)user_data;
    if (countdown_timer_id != 0) return;

    // --- AUTO-ADVANCE REGRESSION FIX ---
    // Prevent accidental overwrites of the foundational backing track (Track 1 / index 0).
    // If Track 1 is armed and has audio, and Track 2 is empty, automatically advance to Track 2.
    int rec_track = atomic_load_explicit(&current_recording_track, memory_order_acquire);
    if (rec_track == 0) {
        bool t0_has_audio = atomic_load_explicit(&master_tracks[0].has_audio, memory_order_acquire);
        bool t1_has_audio = atomic_load_explicit(&master_tracks[1].has_audio, memory_order_acquire);

        if (t0_has_audio && !t1_has_audio) {
            atomic_store_explicit(&current_recording_track, 1, memory_order_release);
            atomic_store_explicit(&selected_tracks_mask, (1 << 1), memory_order_release);

            int active_count = atomic_load_explicit(&active_track_count, memory_order_acquire);
            if (active_count < 2) {
                atomic_store_explicit(&active_track_count, 2, memory_order_release);
            }

            update_multitrack_status_ui();
        }
    }

    if (atomic_load_explicit(&engine_is_playing, memory_order_acquire)) {
        start_recording_execution();
        return;
    }

    gtk_widget_set_sensitive(btn_record, FALSE);
    extern GtkWidget *btn_record_video;
    if (btn_record_video) gtk_widget_set_sensitive(btn_record_video, FALSE);
    gtk_widget_set_sensitive(btn_play, FALSE);
    gtk_widget_set_sensitive(btn_prev, FALSE);
    gtk_widget_set_sensitive(btn_next, FALSE);
    gtk_widget_set_sensitive(btn_stop, TRUE);
    log_and_notify(1, "Status: Get Ready...");

    countdown_window = gtk_window_new();
    gtk_window_set_transient_for(GTK_WINDOW(countdown_window), GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(btn_record))));
    gtk_window_set_decorated(GTK_WINDOW(countdown_window), FALSE);
    gtk_window_set_modal(GTK_WINDOW(countdown_window), TRUE);

    GtkEventController *countdown_key_ctrl = gtk_event_controller_key_new();
    g_signal_connect(countdown_key_ctrl, "key-pressed", G_CALLBACK(on_countdown_key_pressed), NULL);
    gtk_widget_add_controller(countdown_window, countdown_key_ctrl);

    g_signal_connect(countdown_window, "destroy", G_CALLBACK(on_countdown_destroyed), NULL);

    countdown_label = gtk_label_new(NULL);
    gtk_widget_set_margin_start(countdown_label, 80);
    gtk_widget_set_margin_end(countdown_label, 80);
    gtk_widget_set_margin_top(countdown_label, 50);
    gtk_widget_set_margin_bottom(countdown_label, 50);
    gtk_window_set_child(GTK_WINDOW(countdown_window), countdown_label);

    countdown_val = 3;
    char buf[128];
    snprintf(buf, sizeof(buf), "<span size='48000' weight='heavy'>%d</span>", countdown_val);
    gtk_label_set_markup(GTK_LABEL(countdown_label), buf);
    countdown_val--;

    gtk_window_present(GTK_WINDOW(countdown_window));
    countdown_timer_id = g_timeout_add(1000, countdown_tick, NULL);
}

void on_stop_clicked(GtkButton *button, gpointer user_data) {
    (void)button; (void)user_data;

    bool was_recording = atomic_load_explicit(&engine_is_recording, memory_order_acquire);
    extern _Atomic bool is_loop_wrapping;
    bool wrapping = atomic_exchange_explicit(&is_loop_wrapping, false, memory_order_acquire);

    if (countdown_timer_id != 0) {
        if (countdown_window) gtk_window_destroy(GTK_WINDOW(countdown_window));
        gtk_label_set_text(GTK_LABEL(lbl_status), "Status: Recording Aborted");
        gtk_widget_set_sensitive(btn_record, TRUE);
        extern GtkWidget *btn_record_video;
        if (btn_record_video) gtk_widget_set_sensitive(btn_record_video, TRUE);
        gtk_widget_set_sensitive(btn_play, TRUE);
        gtk_widget_set_sensitive(btn_stop, FALSE);
        atomic_store_explicit(&engine_is_playing, false, memory_order_release);
        atomic_store_explicit(&engine_is_recording, false, memory_order_release);
        return;
    }

    if (!wrapping) {
        clear_loop_points();
    }
    bool detached = await_rt_thread_detach();

    atomic_store_explicit(&engine_is_playing, false, memory_order_release);
    atomic_store_explicit(&engine_is_recording, false, memory_order_release);
    atomic_store_explicit(&engine_is_armed, false, memory_order_release);
    atomic_store_explicit(&engine_is_paused, false, memory_order_release);

    if (atomic_load_explicit(&mkv_video_keep_running, memory_order_acquire)) {
        atomic_store_explicit(&mkv_video_keep_running, false, memory_order_release);
        pthread_join(mkv_video_thread, NULL);
    }

    size_t current_pos = atomic_load_explicit(&playback_pos, memory_order_acquire);

    if (pristine_frames > 0 && current_pos > pristine_frames) {
        current_pos = pristine_frames;
        atomic_store_explicit(&playback_pos, current_pos, memory_order_release);
    }

    if (detached) {
        if (was_recording && current_pos > 0) {
            if (atomic_load_explicit(&video_is_armed, memory_order_acquire)) {
                video_end_frame = current_pos;
            }

            bool is_blank_canvas = (strlen(ui_state.selected_track_path) == 0);
            int current_rec = atomic_load_explicit(&current_recording_track, memory_order_acquire);
            int active_tracks = atomic_load_explicit(&active_track_count, memory_order_acquire);

            atomic_store_explicit(&master_tracks[current_rec].has_audio, true, memory_order_release);

            atomic_store_explicit(&selected_tracks_mask, (1 << current_rec), memory_order_release);

            if (current_rec == 0 && active_tracks <= 1 && is_blank_canvas) {
                atomic_store_explicit(&backing_track_frames, current_pos, memory_order_release);
                pristine_frames = current_pos;
                seek_backing_track(0.0);
            }

            if (current_rec == 0 && pristine_bt_buf && master_tracks[0].active_buffer) {
                memcpy(pristine_bt_buf, master_tracks[0].active_buffer, pristine_frames * 2 * sizeof(float));
            }

            if (!wrapping) {
                int next_rec = current_rec + 1;
                if (next_rec < MAX_TRACKS) {
                    atomic_store_explicit(&current_recording_track, next_rec, memory_order_release);
                    int new_active = atomic_load_explicit(&active_track_count, memory_order_acquire);
                    if (next_rec >= new_active) {
                        atomic_store_explicit(&active_track_count, next_rec + 1, memory_order_release);
                    }
                }
            }

            size_t fade_len = (current_pos < 256) ? current_pos : 256;
            if (fade_len > 0) {
                for (size_t f = 0; f < fade_len; f++) {
                    float multiplier = (float)(fade_len - 1 - f) / (float)(fade_len - 1);
                    size_t idx = current_pos - fade_len + f;

                    if (current_rec >= 0 && current_rec < MAX_TRACKS && master_tracks[current_rec].active_buffer) {
                        master_tracks[current_rec].active_buffer[idx * 2] *= multiplier;
                        master_tracks[current_rec].active_buffer[idx * 2 + 1] *= multiplier;
                    }
                }
            }

            update_zoom_button_label_to_length();
            extern void invalidate_waveform_caches(void);
            invalidate_waveform_caches();
        }
        resume_rt_thread();
    }

    update_multitrack_status_ui();
    extern void invalidate_waveform_caches(void);
    invalidate_waveform_caches();
    if (waveform_area_bt) gtk_widget_queue_draw(waveform_area_bt);
    if (waveform_area_input) gtk_widget_queue_draw(waveform_area_input);

    stop_encoder();

    if (is_session_dirty() && !was_recording) {
        if (atomic_load_explicit(&mkv_video_keep_running, memory_order_acquire)) {
            atomic_store_explicit(&mkv_video_keep_running, false, memory_order_release);
            pthread_join(mkv_video_thread, NULL);
        }

        // Launch dynamic video preview natively if an MKV was recorded into the temp directory
        if (access(current_raw_path, F_OK) == 0) {
            atomic_store_explicit(&mkv_video_keep_running, true, memory_order_release);
            pthread_create(&mkv_video_thread, NULL, mkv_video_loop, g_strdup(current_raw_path));
        }
    }

    log_and_notify(1, "Status: Ready");
    gtk_button_set_icon_name(GTK_BUTTON(btn_play), "");
    gtk_button_set_label(GTK_BUTTON(btn_play), "▶");
    gtk_widget_set_tooltip_text(btn_play, "Play (Space)");

    gtk_button_set_label(GTK_BUTTON(btn_record), "🎜 ");
    gtk_widget_set_tooltip_text(btn_record, "Record Audio (R)");
    if (btn_record_video) {
        gtk_button_set_label(GTK_BUTTON(btn_record_video), "🖵");
        gtk_widget_set_tooltip_text(btn_record_video, "Record Audio + Video");
    }

    gtk_widget_set_sensitive(btn_load, TRUE);
    gtk_widget_set_sensitive(btn_settings, TRUE);
    gtk_widget_set_sensitive(btn_prev, TRUE);
    gtk_widget_set_sensitive(btn_next, TRUE);
    gtk_widget_set_sensitive(btn_speed, TRUE);

    bool any_audio = false;
    for (int i = 0; i < MAX_TRACKS; i++) {
        if (atomic_load_explicit(&master_tracks[i].has_audio, memory_order_acquire)) {
            any_audio = true;
            break;
        }
    }
    gtk_widget_set_sensitive(btn_play, any_audio);

    // RESTORE BOTH BUTTONS
    gtk_widget_set_sensitive(btn_record, TRUE);
    if (btn_record_video) gtk_widget_set_sensitive(btn_record_video, TRUE);

    gtk_widget_set_sensitive(btn_save_mux, TRUE);
    gtk_widget_set_sensitive(btn_stop, FALSE);

    if (was_recording) {
        // SAFETY: Ensure new recordings trigger the quit-warning dialog
        mark_session_dirty();

        if (btn_save_mux) {
            gtk_widget_remove_css_class(btn_save_mux, "needs-save");
            gtk_widget_add_css_class(btn_save_mux, "needs-save");
        }
        if (btn_save_session) {
            gtk_widget_remove_css_class(btn_save_session, "needs-save");
            gtk_widget_add_css_class(btn_save_session, "needs-save");
        }
    }

    if (btn_play) gtk_widget_grab_focus(btn_play);
}

static void clear_stash_thread(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable) {
    (void)source_object; (void)cancellable; (void)task;
    char *path = (char *)task_data;
    remove(path);
}

void clear_video_stash_async(void) {
    if (strlen(stashed_raw_path) > 0) {
        GTask *task = g_task_new(NULL, NULL, NULL, NULL);
        g_task_set_task_data(task, g_strdup(stashed_raw_path), g_free);
        g_task_run_in_thread(task, clear_stash_thread);
        g_object_unref(task);
        stashed_raw_path[0] = '\0';
    }
}

void abort_active_video_preview(bool stash) {
    if (atomic_load_explicit(&mkv_video_keep_running, memory_order_acquire)) {
        atomic_store_explicit(&mkv_video_keep_running, false, memory_order_release);
        pthread_join(mkv_video_thread, NULL);
    }

    atomic_store_explicit(&is_mkv_mode, false, memory_order_release); // LET V4L2 RUN

    extern SPSC_Preview_Queue preview_queue;
    PreviewPayload stale_payload;
    while (pop_preview_frame(&preview_queue, &stale_payload)) {}

    if (strlen(current_raw_path) > 0) {
        if (stash) {
            snprintf(stashed_raw_path, sizeof(stashed_raw_path), "%s.stash", current_raw_path);
            rename(current_raw_path, stashed_raw_path);
        } else {
            remove(current_raw_path);
        }
        current_raw_path[0] = '\0';
    }
}

void restore_stashed_video_preview(void) {
    if (strlen(stashed_raw_path) > 0 && access(stashed_raw_path, F_OK) == 0) {
        char restored_path[1024];
        strncpy(restored_path, stashed_raw_path, sizeof(restored_path) - 1);
        char *ext = strstr(restored_path, ".stash");
        if (ext) *ext = '\0';

        rename(stashed_raw_path, restored_path);
        strncpy(current_raw_path, restored_path, sizeof(current_raw_path) - 1);
        stashed_raw_path[0] = '\0';

        atomic_store_explicit(&mkv_video_keep_running, true, memory_order_release);
        pthread_create(&mkv_video_thread, NULL, mkv_video_loop, g_strdup(current_raw_path));
    }
}

void execute_global_canvas_reset(void) {
    extern void purge_autosave_file(void);
    purge_autosave_file();

    on_stop_clicked(NULL, NULL);
    clear_loop_points();

    abort_active_video_preview(false);

    ui_state.selected_track_path[0] = '\0';
    extern void update_playlist_toggle_state(void);
    update_playlist_toggle_state();

    // Reset playback speed natively
    extern guint speed_debounce_id;
    extern float pending_speed;
    if (speed_debounce_id != 0) {
        g_source_remove(speed_debounce_id);
        speed_debounce_id = 0;
    }
    pending_speed = 1.0f;
    set_playback_speed(1.0f);
    extern GtkWidget *btn_speed;
    if (btn_speed) gtk_button_set_label(GTK_BUTTON(btn_speed), "Speed: 1.0x");

    // Reset UI multipliers
    for (int i = 0; i < MAX_TRACKS; i++) {
        set_multitrack_layer_gain(i, 1.0f);
    }

    // SYNCHRONOUS MEMORY WIPE: Bypasses the 500ms async boot delay for instant, 1-step clearing
    int canvas_limit_min = ui_state.config.default_canvas_limit_min > 0 ? ui_state.config.default_canvas_limit_min : 5;
    extern int init_empty_loop_canvas(int);
    init_empty_loop_canvas(canvas_limit_min * 60);

    extern void reset_multitrack_ui_states(void);
    reset_multitrack_ui_states();
    extern void refresh_multitrack_tracks_ui(void);
    refresh_multitrack_tracks_ui();

    extern void trigger_global_ui_refresh(void);
    trigger_global_ui_refresh();

    extern GtkWidget *btn_play;
    extern GtkWidget *btn_record;
    if (btn_play) gtk_widget_set_sensitive(btn_play, FALSE);
    if (btn_record) gtk_widget_set_sensitive(btn_record, TRUE);

    // Apply clean session state after all UI and Engine metadata resets are complete
    mark_session_saved();
    ui_state.is_existing_session = false;
}

static void reset_canvas_thread(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable) {
    (void)source_object; (void)cancellable;
    int duration_sec = GPOINTER_TO_INT(task_data);

    extern int init_empty_loop_canvas(int);
    int status = init_empty_loop_canvas(duration_sec);

    g_task_return_int(task, status);
}

static void reset_canvas_ready(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    (void)source_object; (void)user_data;
    GError *error = NULL;
    int status = g_task_propagate_int(G_TASK(res), &error);

    reset_multitrack_ui_states();
    refresh_multitrack_tracks_ui();

    extern void trigger_global_ui_refresh(void);
    trigger_global_ui_refresh();

    extern GtkWidget *btn_play;
    extern GtkWidget *btn_record;
    if (btn_play) gtk_widget_set_sensitive(btn_play, FALSE);
    if (btn_record) gtk_widget_set_sensitive(btn_record, TRUE);

    GtkWindow *win = gtk_application_get_active_window(GTK_APPLICATION(g_application_get_default()));
    if (win) {
        GtkWidget *master_box = gtk_window_get_child(win);
        if (master_box) gtk_widget_set_sensitive(master_box, TRUE);
    }

    extern GtkWidget *main_spinner;
    gtk_spinner_stop(GTK_SPINNER(main_spinner));
    set_app_waiting_state(false);

    if (status == 0) {
        mark_session_saved();
        ui_state.is_existing_session = false;
        log_and_notify(1, "Status: Recording Canvas Reset");
    } else if (status == -2) {
        log_and_notify(1, "<span foreground='#ff4444'><b>Status: Insufficient RAM for blank canvas</b></span>");
    }
}

static void on_global_reset_confirm(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    (void)user_data;
    GtkAlertDialog *alert = GTK_ALERT_DIALOG(source_object);
    int response = gtk_alert_dialog_choose_finish(alert, res, NULL);

    if (response == 0) { // Confirmed
        GtkWindow *win = gtk_application_get_active_window(GTK_APPLICATION(g_application_get_default()));
        if (win) {
            GtkWidget *master_box = gtk_window_get_child(win);
            if (master_box) gtk_widget_set_sensitive(master_box, FALSE);
        }

        extern GtkWidget *main_spinner;
        gtk_spinner_start(GTK_SPINNER(main_spinner));
        set_app_waiting_state(true);
        log_and_notify(1, "Status: Wiping canvas memory...");

        extern void purge_autosave_file(void);
        purge_autosave_file();

        on_stop_clicked(NULL, NULL);
        clear_loop_points();

        abort_active_video_preview(false);

        ui_state.selected_track_path[0] = '\0';
        extern void update_playlist_toggle_state(void);
        update_playlist_toggle_state();

        // Reset playback speed natively
        extern guint speed_debounce_id;
        extern float pending_speed;
        if (speed_debounce_id != 0) {
            g_source_remove(speed_debounce_id);
            speed_debounce_id = 0;
        }
        pending_speed = 1.0f;
        set_playback_speed(1.0f);
        extern GtkWidget *btn_speed;
        if (btn_speed) gtk_button_set_label(GTK_BUTTON(btn_speed), "Speed: 1.0x");

        // Reset UI multipliers
        for (int i = 0; i < MAX_TRACKS; i++) {
            set_multitrack_layer_gain(i, 1.0f);
        }

        // Launch the background allocation thread
        int canvas_limit_min = ui_state.config.default_canvas_limit_min > 0 ? ui_state.config.default_canvas_limit_min : 5;
        GTask *task = g_task_new(NULL, NULL, reset_canvas_ready, NULL);
        g_task_set_task_data(task, GINT_TO_POINTER(canvas_limit_min * 60), NULL);
        g_task_run_in_thread(task, reset_canvas_thread);
        g_object_unref(task);
    }
}

void on_global_reset_clicked(GtkButton *button, gpointer user_data) {
    (void)user_data;
    if (atomic_load_explicit(&engine_is_recording, memory_order_acquire)) return;

    GtkAlertDialog *alert = gtk_alert_dialog_new("Reset Recording Canvas?");
    gtk_alert_dialog_set_detail(alert, "This will completely clear your recorded canvas, remove any loaded tracks, and start from scratch. Are you sure?");
    const char *buttons[] = { "Reset Canvas", "Cancel", NULL };
    gtk_alert_dialog_set_buttons(alert, buttons);
    gtk_alert_dialog_set_cancel_button(alert, 1);
    gtk_alert_dialog_set_default_button(alert, 0);

    GtkWindow *parent = GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(button)));
    gtk_alert_dialog_choose(alert, parent, NULL, on_global_reset_confirm, NULL);
    g_object_unref(alert);
}

void on_play_clicked(GtkButton *button, gpointer user_data) {
    (void)button; (void)user_data;

    bool is_playing = atomic_load_explicit(&engine_is_playing, memory_order_acquire);
    bool is_paused = atomic_load_explicit(&engine_is_paused, memory_order_acquire);
    bool is_recording = atomic_load_explicit(&engine_is_recording, memory_order_acquire);

    // Trap: The play button does nothing if we are actively recording
    if (is_recording) return;

    bool any_audio = false;
    for (int i = 0; i < MAX_TRACKS; i++) {
        if (atomic_load_explicit(&master_tracks[i].has_audio, memory_order_acquire)) {
            any_audio = true;
            break;
        }
    }
    if (!any_audio && !is_playing && !is_paused) return;

    if (!is_playing && !is_paused) {
        size_t current_pos = atomic_load_explicit(&playback_pos, memory_order_acquire);
        size_t bt_frames = atomic_load_explicit(&backing_track_frames, memory_order_acquire);
        if (bt_frames > 0 && current_pos >= bt_frames) seek_backing_track(0.0);

        // Route either the active temp recording, or the imported MKV track
        char *video_to_play = NULL;
        if (access(current_raw_path, F_OK) == 0) {
            video_to_play = g_strdup(current_raw_path);
        } else if (g_str_has_suffix(ui_state.selected_track_path, ".mkv") && access(ui_state.selected_track_path, F_OK) == 0) {
            video_to_play = g_strdup(ui_state.selected_track_path);
        }

        if (video_to_play) {
            if (atomic_load_explicit(&mkv_video_keep_running, memory_order_acquire)) {
                atomic_store_explicit(&mkv_video_keep_running, false, memory_order_release);
                pthread_join(mkv_video_thread, NULL);
            }
            atomic_store_explicit(&mkv_video_keep_running, true, memory_order_release);
            pthread_create(&mkv_video_thread, NULL, mkv_video_loop, video_to_play);
        }

        atomic_store_explicit(&engine_is_playing, true, memory_order_release);
        log_and_notify(1, "Status: PLAYING");
        gtk_button_set_icon_name(GTK_BUTTON(btn_play), "");
        gtk_button_set_label(GTK_BUTTON(btn_play), "⏸");
        gtk_widget_set_tooltip_text(btn_play, "Pause");

        gtk_widget_set_sensitive(btn_load, FALSE);
        gtk_widget_set_sensitive(btn_record, FALSE);
        extern GtkWidget *btn_record_video;
        if (btn_record_video) gtk_widget_set_sensitive(btn_record_video, FALSE);
        gtk_widget_set_sensitive(btn_stop, TRUE);
        gtk_widget_set_sensitive(btn_save_mux, FALSE);
    } else if (is_playing) {
        bool new_pause_state = !is_paused;
        atomic_store_explicit(&engine_is_paused, new_pause_state, memory_order_release);

        if (new_pause_state) {
            log_and_notify(1, "Status: PAUSED");
            gtk_button_set_icon_name(GTK_BUTTON(btn_play), "media-playback-start-symbolic");
            gtk_widget_set_tooltip_text(btn_play, "Resume (Space)");
        } else {
            log_and_notify(1, "Status: PLAYING");
            gtk_button_set_icon_name(GTK_BUTTON(btn_play), "media-playback-pause-symbolic");
            gtk_widget_set_tooltip_text(btn_play, "Pause (Space)");
        }
    }
}

static void bind_meter(GtkWidget *meter, gpointer peak_var_ptr) {
    extern void on_draw_vu_meter(GtkDrawingArea*, cairo_t*, int, int, gpointer);
    extern gboolean on_waveform_tick(GtkWidget*, GdkFrameClock*, gpointer);
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
        if (g_strcmp0(ui_state.config.audio_profiles[i].device_name, ui_state.config.audio_device) == 0) {
            ui_state.config.audio_profiles[i].gain_multiplier = multiplier;
            found = 1;
            break;
        }
    }
    if (!found && ui_state.config.num_audio_profiles < MAX_SAVED_DEVICES) {
        strncpy(ui_state.config.audio_profiles[ui_state.config.num_audio_profiles].device_name, ui_state.config.audio_device, 127);
        ui_state.config.audio_profiles[ui_state.config.num_audio_profiles].gain_multiplier = multiplier;
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
    if (!lbl_track) return;

    size_t frames = atomic_load_explicit(&backing_track_frames, memory_order_acquire);
    size_t pos = atomic_load_explicit(&playback_pos, memory_order_acquire);
    int rate = atomic_load_explicit(&active_sample_rate, memory_order_acquire);
    if (rate <= 0) rate = 48000;

    int cur_secs = pos / rate;

    static int cached_total_secs = 0;
    static time_t last_scan_time = 0;
    time_t now = time(NULL);

    if (frames > 0 && now != last_scan_time) {
        extern void trigger_audible_scan_async(void);
        trigger_audible_scan_async();
        last_scan_time = now;
    }

    extern _Atomic size_t cached_audible_frames;
    size_t audible_frames = atomic_load_explicit(&cached_audible_frames, memory_order_acquire);
    cached_total_secs = (audible_frames > 0) ? (audible_frames / rate) : (frames / rate);

    int total_secs = (frames > 0) ? cached_total_secs : 0;
    if (cur_secs > total_secs) total_secs = cur_secs; // Prevent time-travel text while actively recording

    char base_name[128] = "None Selected";

    int rec_track = atomic_load_explicit(&current_recording_track, memory_order_acquire);
    extern void get_multitrack_track_name(int, char*);
    get_multitrack_track_name(rec_track, base_name);

    char lbl_buf[256];
    if (frames > 0) {
        snprintf(lbl_buf, sizeof(lbl_buf), "Track: %s [%02d:%02d / %02d:%02d]",
                 base_name, cur_secs / 60, cur_secs % 60, total_secs / 60, total_secs % 60);
    } else {
        int limit = ui_state.config.default_canvas_limit_min > 0 ? ui_state.config.default_canvas_limit_min : 5;
        snprintf(lbl_buf, sizeof(lbl_buf), "Track: Blank Loop Canvas [%02d:00 limit]", limit);
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
    (void)button; (void)user_data;
    if (help_window) {
        gtk_window_present(GTK_WINDOW(help_window));
        return;
    }

    help_window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(help_window), "Keyboard Shortcuts");
    gtk_window_set_default_size(GTK_WINDOW(help_window), 290, 660);
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
    gtk_label_set_markup(GTK_LABEL(lbl_text),
                         "<span foreground='#888888' size='small' weight='bold'>PLAYBACK &amp; RECORDING</span>\n"
                         "<b>Space</b> : Play / Pause\n"
                         "<b>R / S</b> : Record / Stop\n"
                         "<b>V</b> : Record Video\n"
                         "<b>P / N</b> (or <b>PgUp/PgDn</b>) : Prev / Next Track\n"
                         "<b>Left / Right</b> : Seek Timeline\n"
                         "<b>Home / End</b> : Seek Start / End\n"
                         "<b>C</b> : Toggle Speed\n\n"
                         "<span foreground='#888888' size='small' weight='bold'>NAVIGATION &amp; LOOPING</span>\n"
                         "<b>Ctrl+Drag</b> : Pan Timeline\n"
                         "<b>Ctrl+Scroll</b> : Zoom Timeline\n"
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
                         "<b>Ctrl+V</b> : Paste (Absolute Routing)\n"
                         "<b>Ctrl+Shift+V</b> : Global Paste (All Tracks)\n"
                         "<b>Del</b> : Delete Selection (Clear)\n"
                         "<b>Shift+Del</b> : Global Ripple Delete\n"
                         "<b>F</b> : Smart V-Fade Selection\n"
                         "<b>X</b> : Ripple Crossfade (Blend)\n"
                         "<b>Ctrl+Z</b> : Undo Edit\n"
                         "<b>Ctrl+Shift+Z / Ctrl+Y</b> : Redo Edit\n"
                         "<b>Right-Click Red Dot</b> : Remove Video Take\n"
    );

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), lbl_text);
    gtk_box_append(GTK_BOX(vbox), scroll);
    gtk_window_set_child(GTK_WINDOW(help_window), vbox);

    gtk_window_present(GTK_WINDOW(help_window));
}

static void on_telemetry_clicked(GtkButton *button, gpointer user_data) {
    (void)user_data;
    if (telemetry_popover) {
        gtk_widget_unparent(telemetry_popover);
    }

    EngineTelemetry stats = {0};
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
    gtk_label_set_markup(GTK_LABEL(lbl_header1), "<span foreground='#888888' size='small' weight='bold'>AUDIO DSP</span>");
    gtk_widget_set_halign(lbl_header1, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), lbl_header1, 0, row++, 2, 1);

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Hardware Rate:"), 0, row, 1, 1);
    char sr_buf[32]; snprintf(sr_buf, sizeof(sr_buf), "<b>%d Hz</b>", stats.sample_rate);
    GtkWidget *lbl_sr = gtk_label_new(NULL); gtk_label_set_markup(GTK_LABEL(lbl_sr), sr_buf);
    gtk_widget_set_halign(lbl_sr, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), lbl_sr, 1, row++, 1, 1);

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("RT Dropouts:"), 0, row, 1, 1);
    char drop_buf[32]; snprintf(drop_buf, sizeof(drop_buf), "<b>%u</b>", stats.dropped_frames);
    GtkWidget *lbl_drop = gtk_label_new(NULL); gtk_label_set_markup(GTK_LABEL(lbl_drop), drop_buf);
    gtk_widget_set_halign(lbl_drop, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), lbl_drop, 1, row++, 1, 1);

    GtkWidget *lbl_header2 = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lbl_header2), "\n<span foreground='#888888' size='small' weight='bold'>MEMORY &amp; VIDEO</span>");
    gtk_widget_set_halign(lbl_header2, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), lbl_header2, 0, row++, 2, 1);

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Undo Ledger:"), 0, row, 1, 1);
    char ledger_buf[64]; snprintf(ledger_buf, sizeof(ledger_buf), "<b>%d undos, %d redos</b> (Max: %d)", stats.ledger_undos, stats.ledger_redos, stats.ledger_capacity);
    GtkWidget *lbl_ledger = gtk_label_new(NULL); gtk_label_set_markup(GTK_LABEL(lbl_ledger), ledger_buf);
    gtk_widget_set_halign(lbl_ledger, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), lbl_ledger, 1, row++, 1, 1);

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("System RAM:"), 0, row, 1, 1);
    char total_buf[32]; snprintf(total_buf, sizeof(total_buf), "<b>%.1f GB</b>", (double)stats.total_ram_bytes / (1024.0 * 1024.0 * 1024.0));
    GtkWidget *lbl_total = gtk_label_new(NULL); gtk_label_set_markup(GTK_LABEL(lbl_total), total_buf);
    gtk_widget_set_halign(lbl_total, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), lbl_total, 1, row++, 1, 1);

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("QJams Allocated:"), 0, row, 1, 1);
    char qj_buf[32]; snprintf(qj_buf, sizeof(qj_buf), "<b>%.1f GB</b>", (double)stats.qjams_rss_bytes / (1024.0 * 1024.0 * 1024.0));
    GtkWidget *lbl_qj = gtk_label_new(NULL); gtk_label_set_markup(GTK_LABEL(lbl_qj), qj_buf);
    gtk_widget_set_halign(lbl_qj, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), lbl_qj, 1, row++, 1, 1);

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Available RAM:"), 0, row, 1, 1);
    char avail_buf[32]; snprintf(avail_buf, sizeof(avail_buf), "<b>%.1f GB</b>", (double)stats.available_ram_bytes / (1024.0 * 1024.0 * 1024.0));
    GtkWidget *lbl_avail = gtk_label_new(NULL); gtk_label_set_markup(GTK_LABEL(lbl_avail), avail_buf);
    gtk_widget_set_halign(lbl_avail, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), lbl_avail, 1, row++, 1, 1);

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Video Queue Delta:"), 0, row, 1, 1);
    char vq_buf[32]; snprintf(vq_buf, sizeof(vq_buf), "<b>%zu frames</b>", v_delta);
    GtkWidget *lbl_vq = gtk_label_new(NULL); gtk_label_set_markup(GTK_LABEL(lbl_vq), vq_buf);
    gtk_widget_set_halign(lbl_vq, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), lbl_vq, 1, row++, 1, 1);

    telemetry_popover = gtk_popover_new();
    gtk_popover_set_child(GTK_POPOVER(telemetry_popover), grid);
    gtk_widget_set_parent(telemetry_popover, GTK_WIDGET(button));
    gtk_popover_popup(GTK_POPOVER(telemetry_popover));
}

static void toggle_recording_pause(void) {
    bool is_paused = atomic_load_explicit(&engine_is_paused, memory_order_acquire);
    bool new_pause_state = !is_paused;
    atomic_store_explicit(&engine_is_paused, new_pause_state, memory_order_release);
    bool is_video = atomic_load_explicit(&video_is_armed, memory_order_acquire);

    if (new_pause_state) {
        log_and_notify(1, "Status: RECORDING PAUSED");
        if (is_video) {
            if (btn_record_video) gtk_button_set_label(GTK_BUTTON(btn_record_video), "🖵");
        } else {
            gtk_button_set_label(GTK_BUTTON(btn_record), "🎜");
        }
    } else {
        log_and_notify(1, "Status: Recording");
        if (is_video) {
            if (btn_record_video) gtk_button_set_label(GTK_BUTTON(btn_record_video), "⏸");
        } else {
            gtk_button_set_label(GTK_BUTTON(btn_record), "⏸");
        }
    }
}

static void on_record_audio_clicked(GtkButton *button, gpointer user_data) {
    if (atomic_load_explicit(&engine_is_recording, memory_order_acquire)) {
        toggle_recording_pause();
        return;
    }
    atomic_store_explicit(&video_is_armed, false, memory_order_release);
    on_start_clicked(button, user_data);
}

static void on_video_overwrite_response(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    GtkAlertDialog *alert = GTK_ALERT_DIALOG(source_object);
    int response = gtk_alert_dialog_choose_finish(alert, res, NULL);

    if (response == 0) { // Confirmed Overwrite
        atomic_store_explicit(&video_is_armed, true, memory_order_release);
        on_start_clicked(GTK_BUTTON(user_data), NULL);
    }
}

static void on_record_video_clicked(GtkButton *button, gpointer user_data) {
    if (atomic_load_explicit(&engine_is_recording, memory_order_acquire)) {
        toggle_recording_pause();
        return;
    }

    extern _Atomic bool suppress_video_overwrite;
    if (!atomic_load_explicit(&suppress_video_overwrite, memory_order_acquire)) {
        extern char current_raw_path[1024];
        if (strlen(current_raw_path) > 0 && access(current_raw_path, F_OK) == 0) {
            GtkAlertDialog *alert = gtk_alert_dialog_new("Overwrite Existing Video Take?");
            gtk_alert_dialog_set_detail(alert, "A video take already exists in this session. Recording video again will discard your previous video take.\n\nDo you want to overwrite it, or cancel to Export/Mux your current mix first?");

            const char *buttons[] = { "Overwrite", "Cancel", NULL };
            gtk_alert_dialog_set_buttons(alert, buttons);
            gtk_alert_dialog_set_cancel_button(alert, 1);
            gtk_alert_dialog_set_default_button(alert, 1);

            GtkWindow *parent = GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(button)));
            gtk_alert_dialog_choose(alert, parent, NULL, on_video_overwrite_response, button);
            g_object_unref(alert);
            return;
        }
    }

    atomic_store_explicit(&video_is_armed, true, memory_order_release);
    atomic_store_explicit(&suppress_video_overwrite, false, memory_order_release);
    on_start_clicked(button, user_data);
}

void ui_dashboard_init(GtkBuilder *b_dash, GtkBuilder *b_wave) {
    // SAFETY [Critical]: Initialize status widgets first so the hardware routing logic can safely emit toasts
    lbl_status = GTK_WIDGET(gtk_builder_get_object(b_dash, "lbl_status"));
    lbl_hw_toast = GTK_WIDGET(gtk_builder_get_object(b_dash, "lbl_hw_toast"));
    main_spinner = GTK_WIDGET(gtk_builder_get_object(b_dash, "main_spinner"));

    lbl_input_device = GTK_WIDGET(gtk_builder_get_object(b_dash, "lbl_input_device"));

    input_gain_spinner = GTK_WIDGET(gtk_builder_get_object(b_dash, "input_gain_spinner"));
    float loaded_db = (ui_state.config.input_gain_multiplier <= 0.001f) ? -24.0f : 20.0f * log10f(ui_state.config.input_gain_multiplier);
    GtkAdjustment *input_adj = gtk_adjustment_new(loaded_db, -24.0, 24.0, 1.0, 5.0, 0.0);
    gtk_spin_button_set_adjustment(GTK_SPIN_BUTTON(input_gain_spinner), input_adj);
    gtk_spin_button_set_climb_rate(GTK_SPIN_BUTTON(input_gain_spinner), 1.0);
    g_signal_connect(input_gain_spinner, "value-changed", G_CALLBACK(on_input_gain_changed), NULL);

    GtkWidget *bt_gain_spinner = GTK_WIDGET(gtk_builder_get_object(b_dash, "bt_gain_spinner"));
    float loaded_bt_db = (ui_state.config.bt_gain_multiplier <= 0.001f) ? -24.0f : 20.0f * log10f(ui_state.config.bt_gain_multiplier);
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

    btn_reset = GTK_WIDGET(gtk_builder_get_object(b_dash, "btn_reset"));
    g_signal_connect(btn_reset, "clicked", G_CALLBACK(on_global_reset_clicked), NULL);

    btn_prev = GTK_WIDGET(gtk_builder_get_object(b_dash, "btn_prev"));
    g_signal_connect(btn_prev, "clicked", G_CALLBACK(on_prev_track_clicked), NULL);
    btn_next = GTK_WIDGET(gtk_builder_get_object(b_dash, "btn_next"));
    g_signal_connect(btn_next, "clicked", G_CALLBACK(on_next_track_clicked), NULL);

    btn_speed = GTK_WIDGET(gtk_builder_get_object(b_dash, "btn_speed"));
    g_signal_connect(btn_speed, "clicked", G_CALLBACK(on_speed_clicked), NULL);

    btn_zoom = GTK_WIDGET(gtk_builder_get_object(b_dash, "btn_zoom"));
    gtk_widget_set_tooltip_text(btn_zoom, "Set track to recording length");
    g_signal_connect(btn_zoom, "clicked", G_CALLBACK(on_crop_clicked), NULL);

    btn_playlist_toggle = GTK_WIDGET(gtk_builder_get_object(b_dash, "btn_playlist_toggle"));
    g_signal_connect(btn_playlist_toggle, "clicked", G_CALLBACK(on_playlist_toggle_clicked), NULL);

    GtkWidget *btn_shortcuts = GTK_WIDGET(gtk_builder_get_object(b_dash, "btn_shortcuts"));
    if (btn_shortcuts) {
        g_signal_connect(btn_shortcuts, "clicked", G_CALLBACK(on_shortcuts_clicked), NULL);
    }

    input_pref_drop = GTK_WIDGET(gtk_builder_get_object(b_dash, "input_pref_drop"));
    if (input_pref_drop) {
        g_signal_connect(input_pref_drop, "notify::selected", G_CALLBACK(on_pref_selection_changed), NULL);
        update_dashboard_pref_ui();
    }

    btn_telemetry = GTK_WIDGET(gtk_builder_get_object(b_dash, "btn_telemetry"));
    if (btn_telemetry) {
        g_signal_connect(btn_telemetry, "clicked", G_CALLBACK(on_telemetry_clicked), NULL);
    }

    waveform_area_bt = GTK_WIDGET(gtk_builder_get_object(b_wave, "waveform_area_bt"));
    GtkEventController *scroll_ctrl_bt = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
    g_signal_connect(scroll_ctrl_bt, "scroll", G_CALLBACK(on_waveform_scroll), NULL);
    gtk_widget_add_controller(waveform_area_bt, scroll_ctrl_bt);

    waveform_area_input = GTK_WIDGET(gtk_builder_get_object(b_wave, "waveform_area_input"));
    GtkEventController *scroll_ctrl_in = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
    g_signal_connect(scroll_ctrl_in, "scroll", G_CALLBACK(on_waveform_scroll), NULL);
    gtk_widget_add_controller(waveform_area_input, scroll_ctrl_in);
}

/**
 * @brief Cleans up globally allocated dashboard memory pools during application teardown.
 */
void ui_dashboard_shutdown(void) {
    if (atomic_load_explicit(&mkv_video_keep_running, memory_order_acquire)) {
        atomic_store_explicit(&mkv_video_keep_running, false, memory_order_release);
        pthread_join(mkv_video_thread, NULL);
    }

    for (int i = 0; i < 32; i++) {
        if (mkv_rgba_pool[i]) {
            free(mkv_rgba_pool[i]);
            mkv_rgba_pool[i] = NULL;
        }
    }
}
