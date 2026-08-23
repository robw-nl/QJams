#include "ui_dashboard.h"
#include "ui_globals.h"
#include "audio_engine.h"
#include "ui_waveforms.h"
#include "ui_playlist.h"
#include "ui_multitrack.h"
#include "encoder.h"
#include "scanner.h"
#include "video_engine.h"
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

_Atomic bool is_mkv_mode = false;
static pthread_t mkv_video_thread;
static _Atomic bool mkv_video_keep_running = false;
static uint8_t* mkv_rgba_pool[32] = {NULL};

char final_save_path[1024] = "";
char current_raw_path[1024] = "";

static guint speed_debounce_id = 0;
static float pending_speed = 1.0f;

static guint port_update_debounce_id = 0;

static GtkWidget *lbl_hw_toast = NULL;

static gboolean apply_port_update_deferred(gpointer user_data) {
    (void)user_data;
    update_dashboard_cycler_ui();
    port_update_debounce_id = 0;
    return G_SOURCE_REMOVE;
}

gboolean on_jack_port_registration_ui(gpointer data) {
    (void)data;
    if (port_update_debounce_id != 0) {
        g_source_remove(port_update_debounce_id);
    }
    // Wait 500ms for the device to fully register all its channels before reading the scanner
    port_update_debounce_id = g_timeout_add(500, apply_port_update_deferred, NULL);
    return G_SOURCE_REMOVE;
}

static guint countdown_timer_id = 0;
static int countdown_val = 3;
static GtkWidget *countdown_window = NULL;
static GtkWidget *countdown_label = NULL;

static GtkWidget *input_cycler_drop = NULL;
static GtkStringList *cycler_model = NULL;
static bool cycler_is_updating = false;

static void on_input_gain_changed(GtkSpinButton *spin_button, gpointer user_data); // Forward declaration

/**
 * @brief Refreshes the hardware connection status label.
 * Strictly shows only offline Primary and Fallback devices. Disappears when fully connected.
 */
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

        // Prevent duplicate checks if primary == fallback
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
        gtk_label_set_text(GTK_LABEL(lbl_hw_toast), toast_msg);
        gtk_widget_set_visible(lbl_hw_toast, TRUE);
    } else {
        gtk_widget_set_visible(lbl_hw_toast, FALSE);
    }
}

void update_dashboard_cycler_ui(void) {
    if (!input_cycler_drop) return;
    cycler_is_updating = true;

    if (cycler_model) {
        g_object_unref(cycler_model);
    }
    cycler_model = gtk_string_list_new(NULL);

    int active_idx = 0;
    int count = 0;

    // Perform a live hardware scan to verify which devices are currently powered on
    AudioDevice active_devs[MAX_AUDIO_DEVICES];
    int online_count = scan_audio_devices(client, active_devs, MAX_AUDIO_DEVICES);

    update_hardware_status_label();

    for (int i = 0; i < ui_state.config.num_audio_profiles; i++) {
        if (ui_state.config.audio_profiles[i].in_cycler) {

            // Verify the profiled device exists in the live hardware pool
            int is_online = 0;
            for (int j = 0; j < online_count; j++) {
                if (strcmp(ui_state.config.audio_profiles[i].device_name, active_devs[j].display_name) == 0) {
                    is_online = 1;
                    break;
                }
            }

            if (is_online) {
                gtk_string_list_append(cycler_model, ui_state.config.audio_profiles[i].device_name);
                if (strcmp(ui_state.config.audio_profiles[i].device_name, ui_state.config.audio_device) == 0) {
                    active_idx = count;
                }
                count++;
            }
        }
    }

    // Fallback: If no cycler devices are online, just show the current active device
    if (count == 0) {
        gtk_string_list_append(cycler_model, ui_state.config.audio_device);
        active_idx = 0;
    }

    gtk_drop_down_set_model(GTK_DROP_DOWN(input_cycler_drop), G_LIST_MODEL(cycler_model));
    gtk_drop_down_set_selected(GTK_DROP_DOWN(input_cycler_drop), active_idx);

    cycler_is_updating = false;
}

static void on_cycler_selection_changed(GObject *gobject, GParamSpec *pspec, gpointer user_data) {
    (void)pspec; (void)user_data;
    if (cycler_is_updating) return;

    guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(gobject));
    if (selected == GTK_INVALID_LIST_POSITION) return;

    const char *new_device = gtk_string_list_get_string(cycler_model, selected);
    if (!new_device) return;

    // 1. Hot-Patch the audio engine instantly
    patch_audio_ports(new_device);

    strncpy(ui_state.config.audio_device, new_device, sizeof(ui_state.config.audio_device) - 1);

    // 2. Update the Dashboard Text
    char new_label[256];
    snprintf(new_label, sizeof(new_label), "%s Gain (Pre-Buffer dB):", new_device);
    gtk_label_set_text(GTK_LABEL(lbl_input_device), new_label);

    // 3. Recall the saved gain profile for the new device
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

    // Temporarily block the spinner signal so updating the UI doesn't trigger a redundant config save
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

static gboolean apply_zoom_deferred(gpointer user_data) {
    (void)user_data;
    size_t current_pos = atomic_load_explicit(&playback_pos, memory_order_acquire);
    size_t total_frames = atomic_load_explicit(&backing_track_frames, memory_order_acquire);

    if (total_frames > 0 && waveform_area_bt) {
        int width = gtk_widget_get_width(waveform_area_bt);
        double virtual_width = width * zoom_multiplier;
        double playhead_virtual_x = ((double)current_pos / (double)total_frames) * virtual_width;

        gtk_adjustment_set_upper(waveform_adj, virtual_width);
        gtk_adjustment_set_page_size(waveform_adj, width);
        gtk_adjustment_set_step_increment(waveform_adj, 50.0);
        gtk_adjustment_set_page_increment(waveform_adj, width * 0.9);

        double new_scroll = playhead_virtual_x - (width / 2.0);
        if (new_scroll < 0) new_scroll = 0;
        if (new_scroll > virtual_width - width) new_scroll = virtual_width - width;

        gtk_adjustment_set_value(waveform_adj, new_scroll);
    }

    invalidate_waveform_caches();
    gtk_widget_queue_draw(waveform_area_bt);
    gtk_widget_queue_draw(waveform_area_input);
    return G_SOURCE_REMOVE;
}

static void on_zoom_clicked(GtkButton *button, gpointer user_data) {
    (void)button; (void)user_data;
    zoom_multiplier = 1.0;
    update_zoom_button_label_to_length();
    apply_zoom_deferred(NULL);
}

void prepare_engine_for_new_track(void) {
    on_stop_clicked(NULL, NULL);

    clear_loop_points(); // FIX: Ensure loop selection is wiped from the UI and engine

    if (btn_save_mux) gtk_widget_remove_css_class(btn_save_mux, "needs-save");
    if (btn_save_session) gtk_widget_remove_css_class(btn_save_session, "needs-save");

    if (speed_debounce_id != 0) {
        g_source_remove(speed_debounce_id);
        speed_debounce_id = 0;
    }
    pending_speed = 1.0f;
    set_playback_speed(1.0f);
    if (btn_speed) gtk_button_set_label(GTK_BUTTON(btn_speed), "Speed: 1.0x");

    pristine_frames = 0;
    backing_track_frames = 0;
    ui_state.session_is_dirty = false; // A cleared or fresh track is inherently clean
    update_zoom_button_label_to_length();
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
    if (avformat_open_input(&fmt_ctx, filepath, NULL, NULL) != 0) { free(filepath); return NULL; }
    avformat_find_stream_info(fmt_ctx, NULL);

    if (!mkv_rgba_pool[0]) {
        for (int i = 0; i < 32; i++) mkv_rgba_pool[i] = malloc(1280 * 720 * 4);
    }

    int v_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (v_idx < 0) { avformat_close_input(&fmt_ctx); free(filepath); return NULL; }

    AVCodecContext *v_ctx = avcodec_alloc_context3(NULL);
    avcodec_parameters_to_context(v_ctx, fmt_ctx->streams[v_idx]->codecpar);
    const AVCodec *v_codec = avcodec_find_decoder(v_ctx->codec_id);
    avcodec_open2(v_ctx, v_codec, NULL);

    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    struct SwsContext *sws_ctx = NULL;
    AVRational time_base = fmt_ctx->streams[v_idx]->time_base;
    double last_audio_time = -1.0;

    int target_w = 676;
    int target_h = 380;

    while (atomic_load_explicit(&mkv_video_keep_running, memory_order_acquire)) {
        int current_rate = atomic_load_explicit(&active_sample_rate, memory_order_acquire);
        if (current_rate == 0) { usleep(10000); continue; }

        size_t pos = atomic_load_explicit(&playback_pos, memory_order_acquire);
        double current_audio_time = (double)pos / current_rate;

        // Synchronize the original MKV video time with the Stretched audio playback time
        if (fabs(current_audio_time - last_audio_time) > 0.25 && last_audio_time >= 0.0) {

            // SHOWSTOPPER 4 FIX: Drain the preview queue instantly to prevent stale frame tearing during scrub delay
            PreviewPayload stale_payload;
            while (pop_preview_frame(&preview_queue, &stale_payload)) {}

            float current_speed = atomic_load_explicit(&playback_speed, memory_order_relaxed);
            double target_mkv_time = current_audio_time * (current_speed > 0.0f ? current_speed : 1.0f);
            av_seek_frame(fmt_ctx, v_idx, (int64_t)(target_mkv_time / av_q2d(time_base)), AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers(v_ctx);
        }
        last_audio_time = current_audio_time;

        if (av_read_frame(fmt_ctx, pkt) >= 0) {
            if (pkt->stream_index == v_idx) {
                if (avcodec_send_packet(v_ctx, pkt) == 0) {
                    while (avcodec_receive_frame(v_ctx, frame) == 0) {
                        double raw_frame_time = frame->pts * av_q2d(time_base);
                        double frame_time = 0.0;
                        double sync_last_audio = current_audio_time;

                        while (atomic_load_explicit(&mkv_video_keep_running, memory_order_acquire)) {
                            float current_speed = atomic_load_explicit(&playback_speed, memory_order_relaxed);
                            frame_time = raw_frame_time / (current_speed > 0.0f ? current_speed : 1.0f);
                            size_t pos = atomic_load_explicit(&playback_pos, memory_order_acquire);
                            current_audio_time = (double)pos / current_rate;
                            if (current_audio_time < sync_last_audio - 0.2) break;
                            sync_last_audio = current_audio_time;
                            double time_diff = frame_time - current_audio_time;
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

                        if (frame_time < current_audio_time - 0.1) continue;

                        if (!sws_ctx) {
                            double src_aspect = (double)frame->width / (double)frame->height;
                            double target_aspect = (double)target_w / (double)target_h;
                            int scaled_w = target_w;
                            int scaled_h = target_h;
                            if (src_aspect > target_aspect) scaled_h = (int)((double)target_w / src_aspect);
                            else scaled_w = (int)((double)target_h * src_aspect);
                            sws_ctx = sws_getContext(frame->width, frame->height, frame->format, scaled_w, scaled_h, AV_PIX_FMT_RGBA, SWS_FAST_BILINEAR, NULL, NULL, NULL);
                        }

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
            av_packet_unref(pkt);
        } else {
            double last_standby_time = current_audio_time;
            while (atomic_load_explicit(&mkv_video_keep_running, memory_order_acquire)) {
                size_t pos = atomic_load_explicit(&playback_pos, memory_order_acquire);
                int rate = atomic_load_explicit(&active_sample_rate, memory_order_acquire);
                float current_speed = atomic_load_explicit(&playback_speed, memory_order_relaxed);
                double new_audio_time = rate > 0 ? ((double)pos / rate) : 0.0;

                // Map standby loop seek target to original MKV video time
                if (new_audio_time < last_standby_time - 0.5) {

                    // SHOWSTOPPER 4 FIX: Drain the preview queue on loop resets
                    PreviewPayload stale_payload;
                    while (pop_preview_frame(&preview_queue, &stale_payload)) {}

                    double target_mkv_time = new_audio_time * (current_speed > 0.0f ? current_speed : 1.0f);
                    int v_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
                    av_seek_frame(fmt_ctx, v_idx, (int64_t)(target_mkv_time / av_q2d(time_base)), AVSEEK_FLAG_BACKWARD);
                    avcodec_flush_buffers(v_ctx);
                    break;
                }
                last_standby_time = new_audio_time;
                usleep(10000);
            }
        }
    }

    avcodec_free_context(&v_ctx);
    avformat_close_input(&fmt_ctx);

    if (sws_ctx) sws_freeContext(sws_ctx);
    av_packet_free(&pkt);
    av_frame_free(&frame);

    free(filepath);
    return NULL;
}

static void load_track_thread(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable) {
    (void)source_object; (void)cancellable;
    char *filepath = (char *)task_data;
    int result = load_backing_track(filepath, client);
    g_task_return_int(task, result);
}

static void load_track_ready(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    (void)source_object; (void)user_data;
    GError *error = NULL;
    int result = g_task_propagate_int(G_TASK(res), &error);

    ui_state.is_loading_track = false;
    gtk_spinner_stop(GTK_SPINNER(main_spinner)); // Halt the spinner

    if (result == 0) {
        int total_secs = backing_track_frames / jack_get_sample_rate(client);
        char ui_text[600];
        gchar *basename = g_path_get_basename(ui_state.selected_track_path);
        snprintf(ui_text, sizeof(ui_text), "Track: %s [%02d:%02d]", basename, total_secs / 60, total_secs % 60);

        // WIPE GHOST UI LAYERS AND SYNC BASE TRACK NAME ---
        reset_multitrack_ui_states();
        char display_name[256];
        strncpy(display_name, basename, sizeof(display_name) - 1);
        char *dot = strrchr(display_name, '.');
        if (dot) *dot = '\0';
        set_multitrack_track_name(0, display_name);

        gtk_label_set_text(GTK_LABEL(lbl_track), ui_text);
        g_free(basename);

        gtk_label_set_text(GTK_LABEL(lbl_status), "Status: Ready");
        gtk_widget_set_sensitive(btn_play, TRUE);

        // FIX: Always allow recording over loaded tracks, including MKVs
        bool is_mkv = g_str_has_suffix(ui_state.selected_track_path, ".mkv");
        bool is_looper = atomic_load_explicit(&is_multitrack_mode, memory_order_acquire);
        gtk_widget_set_sensitive(btn_record, TRUE);

        if (atomic_load_explicit(&mkv_video_keep_running, memory_order_acquire)) {
            atomic_store_explicit(&mkv_video_keep_running, false, memory_order_release);
            pthread_join(mkv_video_thread, NULL);
        }

        if (is_mkv && !is_looper) {
            atomic_store_explicit(&is_mkv_mode, true, memory_order_release);
            atomic_store_explicit(&mkv_video_keep_running, true, memory_order_release);
            pthread_create(&mkv_video_thread, NULL, mkv_video_loop, g_strdup(ui_state.selected_track_path));
        } else {
            atomic_store_explicit(&is_mkv_mode, false, memory_order_release);
        }

        update_playlist_toggle_state();
        if (is_looper) update_multitrack_status_ui();
        update_zoom_button_label_to_length();
    } else if (result == -2) {
        gtk_label_set_markup(GTK_LABEL(lbl_status), "<span foreground='#ff4444'><b>Status: Track exceeds safe RAM limits</b></span>");
        gtk_label_set_text(GTK_LABEL(lbl_track), "Track: None Selected");
    } else if (result == -3) {
        gtk_label_set_markup(GTK_LABEL(lbl_status), "<span foreground='#ff4444'><b>Status: Track Load Failed (Corrupt or Empty Media)</b></span>");
        gtk_label_set_text(GTK_LABEL(lbl_track), "Track: None Selected");
        ui_state.selected_track_path[0] = '\0';
        update_playlist_toggle_state();
    } else {
        if (access(ui_state.selected_track_path, F_OK) != 0) {
            gtk_label_set_markup(GTK_LABEL(lbl_status), "<span foreground='#ff4444'><b>Status: Track not found, removed from playlist</b></span>");
            remove_track_from_playlist(ui_state.selected_track_path);
        } else {
            gtk_label_set_markup(GTK_LABEL(lbl_status), "<span foreground='#ff4444'><b>Status: Media Load Failed</b></span>");
        }
        gtk_label_set_text(GTK_LABEL(lbl_track), "Track: None Selected");
        ui_state.selected_track_path[0] = '\0';
        update_playlist_toggle_state();
    }

    gtk_widget_set_sensitive(btn_load, TRUE);
    gtk_widget_set_sensitive(btn_prev, TRUE);
    gtk_widget_set_sensitive(btn_next, TRUE);
    gtk_widget_set_sensitive(btn_playlist_toggle, TRUE);

    invalidate_waveform_caches();

    gtk_widget_queue_draw(waveform_area_bt);
    gtk_widget_queue_draw(waveform_area_input);
}

void trigger_track_load(void) {
    prepare_engine_for_new_track();
    ui_state.is_loading_track = true;
    zoom_multiplier = 1.0;

    gtk_label_set_text(GTK_LABEL(lbl_status), "Status: Loading track into memory...");
    gtk_label_set_text(GTK_LABEL(lbl_track), "Track: Loading...");
    gtk_spinner_start(GTK_SPINNER(main_spinner)); // Start the spinner

    gtk_widget_set_sensitive(btn_load, FALSE);
    gtk_widget_set_sensitive(btn_prev, FALSE);
    gtk_widget_set_sensitive(btn_next, FALSE);
    gtk_widget_set_sensitive(btn_play, FALSE);
    gtk_widget_set_sensitive(btn_record, FALSE);
    gtk_widget_set_sensitive(btn_playlist_toggle, FALSE);

    gtk_widget_queue_draw(waveform_area_bt);
    gtk_widget_queue_draw(waveform_area_input);

    GTask *task = g_task_new(NULL, NULL, load_track_ready, NULL);
    g_task_set_task_data(task, g_strdup(ui_state.selected_track_path), g_free);
    g_task_run_in_thread(task, load_track_thread);
    g_object_unref(task);
}

/**
 * @brief Dynamically resizes an empty recording canvas when the application mode changes.
 * Prevents canvas length mismatches by verifying that no audio has been committed to the buffer before reallocating.
 * @param is_looper Current target state of the application mode.
 */
void adjust_blank_canvas_for_mode(bool is_looper) {
    if (strlen(ui_state.selected_track_path) > 0) return; // Track is loaded, leave it alone
    if (ui_state.session_is_dirty) return; // Canvas has recorded audio, preserve it

    int free_min = ui_state.config.freestyle_duration_min > 0 ? ui_state.config.freestyle_duration_min : 15;
    int loop_min = ui_state.config.multitrack_duration_min > 0 ? ui_state.config.multitrack_duration_min : 5;

    if (is_looper) {
        if (init_empty_loop_canvas(loop_min * 60) == 0) {
            char track_lbl[128];
            snprintf(track_lbl, sizeof(track_lbl), "Track: Blank Loop Canvas [%02d:00]", loop_min);
            gtk_label_set_text(GTK_LABEL(lbl_track), track_lbl);
            gtk_widget_set_sensitive(btn_play, TRUE);
            gtk_widget_set_sensitive(btn_record, TRUE);
        }
    } else {
        // Video mode ALWAYS receives a freestyle canvas if no track is loaded
        if (init_empty_loop_canvas(free_min * 60) == 0) {
            char track_lbl[128];
            snprintf(track_lbl, sizeof(track_lbl), "Track: Freestyle Ready [%02d:00]", free_min);
            gtk_label_set_text(GTK_LABEL(lbl_track), track_lbl);
            gtk_widget_set_sensitive(btn_play, TRUE);
            gtk_widget_set_sensitive(btn_record, TRUE);
        }
    }

    ui_state.session_is_dirty = false;
    update_zoom_button_label_to_length();
    invalidate_waveform_caches();
    if (waveform_area_bt) gtk_widget_queue_draw(waveform_area_bt);
    if (waveform_area_input) gtk_widget_queue_draw(waveform_area_input);
}

static void start_recording_execution(void) {
    size_t current_bt_frames = atomic_load_explicit(&backing_track_frames, memory_order_acquire);
    size_t current_pos = atomic_load_explicit(&playback_pos, memory_order_acquire);

    // FIX: Synchronously force the playhead to 0 if we are at the end.
    // This prevents the UI tick from reading a stale end-of-track position and instantly aborting the recording.
    if (current_bt_frames > 0 && current_pos >= current_bt_frames) {
        atomic_store_explicit(&playback_pos, 0, memory_order_release);
        seek_backing_track(0.0);
    }

    if (btn_save_mux) gtk_widget_remove_css_class(btn_save_mux, "needs-save");
    if (btn_save_session) gtk_widget_remove_css_class(btn_save_session, "needs-save");

    ui_state.session_is_dirty = true;

    if (current_bt_frames == 0) {
        bool is_looper = atomic_load_explicit(&is_multitrack_mode, memory_order_acquire);
        int free_min = ui_state.config.freestyle_duration_min > 0 ? ui_state.config.freestyle_duration_min : 15;
        int loop_min = ui_state.config.multitrack_duration_min > 0 ? ui_state.config.multitrack_duration_min : 5;
        int duration_sec = is_looper ? (loop_min * 60) : (free_min * 60);

        if (init_empty_loop_canvas(duration_sec) != 0) {
            gtk_label_set_text(GTK_LABEL(lbl_status), "Status: Insufficient RAM for blank canvas.");
            return;
        }

        if (is_looper) {
            char track_lbl[128];
            snprintf(track_lbl, sizeof(track_lbl), "Track: Blank Loop Canvas [%02d:00 limit]", loop_min);
            gtk_label_set_text(GTK_LABEL(lbl_track), track_lbl);
            update_multitrack_status_ui();
        } else {
            char track_lbl[128];
            snprintf(track_lbl, sizeof(track_lbl), "Track: Freestyle Recording [%02d:00 limit]", free_min);
            gtk_label_set_text(GTK_LABEL(lbl_track), track_lbl);
        }
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

    // Delete the previous take's raw video file from disk before creating a new one
    if (strlen(current_raw_path) > 0) {
        remove(current_raw_path);
    }

    snprintf(final_save_path, sizeof(final_save_path), "%s/%04d%02d%02d-%02d%02d%02d-%s-RAW.mkv",
             ui_state.config.recordings_dir, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec, base_name);

    snprintf(current_raw_path, sizeof(current_raw_path), "/tmp/qjams_raw_%04d%02d%02d_%02d%02d%02d.mkv",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
    int current_samplerate = client ? jack_get_sample_rate(client) : 48000;

    // ALWAYS backup the undo buffer for the active layer, even if Video Mode is rendering a performance
    if (await_rt_thread_detach()) {
        int rec_track = atomic_load_explicit(&current_recording_track, memory_order_acquire);
        if (rec_track >= 0 && rec_track < MAX_TRACKS && multitrack_tracks[rec_track] && undo_tracks[rec_track]) {
            memcpy(undo_tracks[rec_track], multitrack_tracks[rec_track], pristine_frames * 2 * sizeof(float));
        }
        resume_rt_thread();
    }

    // Start the Video encoder if we are in Video Mode
    if (!atomic_load_explicit(&is_multitrack_mode, memory_order_acquire)) {
        if (init_and_start_encoder(current_raw_path, capture_width, capture_height, current_samplerate, &video_queue) != 0) {
            gtk_label_set_text(GTK_LABEL(lbl_status), "Status: Encoder Failed");
            return;
        }
    }

    atomic_store_explicit(&engine_is_armed, true, memory_order_release);
    atomic_store_explicit(&engine_is_recording, true, memory_order_release);
    invalidate_waveform_caches();

    if (!atomic_load_explicit(&engine_is_playing, memory_order_acquire)) {
        atomic_store_explicit(&engine_is_playing, true, memory_order_release);
    }

    gtk_label_set_text(GTK_LABEL(lbl_status), "Status: Recording");
    gtk_button_set_icon_name(GTK_BUTTON(btn_play), "media-playback-pause-symbolic");
    gtk_widget_set_tooltip_text(btn_play, "Pause (Space / P)");

    gtk_widget_set_sensitive(btn_load, FALSE);
    gtk_widget_set_sensitive(btn_settings, FALSE);
    gtk_widget_set_sensitive(btn_prev, FALSE);
    gtk_widget_set_sensitive(btn_next, FALSE);
    gtk_widget_set_sensitive(btn_speed, FALSE);
    gtk_widget_set_sensitive(btn_save_mux, FALSE);
    gtk_widget_set_sensitive(btn_play, TRUE);
    gtk_widget_set_sensitive(btn_record, FALSE);
    gtk_widget_set_sensitive(btn_stop, TRUE);

    if (btn_play) gtk_widget_grab_focus(btn_play);
}

static gboolean countdown_tick(gpointer user_data) {
    (void)user_data;
    if (countdown_val > 0) {
        char buf[128];
        snprintf(buf, sizeof(buf), "<span size='48000' weight='heavy'>%d</span>", countdown_val);
        gtk_label_set_markup(GTK_LABEL(countdown_label), buf);
        countdown_val--;
        return G_SOURCE_CONTINUE;
    } else if (countdown_val == 0) {
        gtk_label_set_markup(GTK_LABEL(countdown_label), "<span size='48000' weight='heavy' foreground='#ff4444'>GO!</span>");
        countdown_val--;
        return G_SOURCE_CONTINUE;
    } else {
        // Start the recording engine FIRST, so that engine_is_playing becomes true.
        // This prevents the on_countdown_destroyed failsafe from aborting the record sequence.
        start_recording_execution();

        // Clear the timer ID so the destroy handler knows it completed naturally
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

    // Failsafe: If the window was destroyed mid-countdown by the WM (Alt+F4), abort recording
    if (!atomic_load_explicit(&engine_is_playing, memory_order_acquire) && gtk_widget_is_sensitive(btn_stop)) {
        gtk_label_set_text(GTK_LABEL(lbl_status), "Status: Recording Aborted");
        gtk_widget_set_sensitive(btn_record, TRUE);
        gtk_widget_set_sensitive(btn_play, TRUE);
        gtk_widget_set_sensitive(btn_stop, FALSE);
    }
}

void on_start_clicked(GtkButton *button, gpointer user_data) {
    (void)button; (void)user_data;
    if (countdown_timer_id != 0) return;

    if (atomic_load_explicit(&engine_is_playing, memory_order_acquire)) {
        start_recording_execution();
        return;
    }

    gtk_widget_set_sensitive(btn_record, FALSE);
    gtk_widget_set_sensitive(btn_play, FALSE);
    gtk_widget_set_sensitive(btn_stop, TRUE);
    gtk_label_set_text(GTK_LABEL(lbl_status), "Status: Get Ready...");

    countdown_window = gtk_window_new();
    gtk_window_set_transient_for(GTK_WINDOW(countdown_window), GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(btn_record))));
    gtk_window_set_decorated(GTK_WINDOW(countdown_window), FALSE);
    gtk_window_set_modal(GTK_WINDOW(countdown_window), TRUE);

    // Bind the timer cleanup strictly to the window's destruction
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

    if (countdown_timer_id != 0) {
        if (countdown_window) gtk_window_destroy(GTK_WINDOW(countdown_window));
        gtk_label_set_text(GTK_LABEL(lbl_status), "Status: Recording Aborted");
        gtk_widget_set_sensitive(btn_record, TRUE);
        gtk_widget_set_sensitive(btn_play, TRUE);
        gtk_widget_set_sensitive(btn_stop, FALSE);
        atomic_store_explicit(&engine_is_playing, false, memory_order_release);
        atomic_store_explicit(&engine_is_recording, false, memory_order_release);
        return;
    }

    clear_loop_points();

    // --- GLOBAL DSP BARRIER ---
    bool detached = await_rt_thread_detach();

    atomic_store_explicit(&engine_is_playing, false, memory_order_release);
    atomic_store_explicit(&engine_is_recording, false, memory_order_release);
    atomic_store_explicit(&engine_is_armed, false, memory_order_release);
    atomic_store_explicit(&engine_is_paused, false, memory_order_release);

    size_t current_pos = atomic_load_explicit(&playback_pos, memory_order_acquire);

    if (pristine_frames > 0 && current_pos > pristine_frames) {
        current_pos = pristine_frames;
        atomic_store_explicit(&playback_pos, current_pos, memory_order_release);
    }

    // SHOWSTOPPER 2 FIX: Only manipulate memory arrays if the RT thread safely detached
    if (detached) {
        if (was_recording && current_pos > 0) {
            bool is_looper = atomic_load_explicit(&is_multitrack_mode, memory_order_acquire);
            bool is_blank_canvas = (strlen(ui_state.selected_track_path) == 0);
            int current_rec = atomic_load_explicit(&current_recording_track, memory_order_acquire);
            int active_tracks = atomic_load_explicit(&active_track_count, memory_order_acquire);

            atomic_store_explicit(&track_has_audio[current_rec], true, memory_order_release);
            atomic_store_explicit(&track_is_soloed[current_rec], true, memory_order_release);

            if (current_rec == 0 && active_tracks <= 1 && is_blank_canvas) {
                atomic_store_explicit(&backing_track_frames, current_pos, memory_order_release);
                pristine_frames = current_pos;

                char ui_text[128];
                int current_rate = atomic_load_explicit(&active_sample_rate, memory_order_acquire);
                int total_secs = (current_rate > 0) ? (current_pos / current_rate) : 0;

                if (is_looper) {
                    snprintf(ui_text, sizeof(ui_text), "Track: Custom Loop [%02d:%02d]", total_secs / 60, total_secs % 60);
                    seek_backing_track(0.0);
                } else {
                    snprintf(ui_text, sizeof(ui_text), "Track: Freestyle Take [%02d:%02d]", total_secs / 60, total_secs % 60);
                }
                gtk_label_set_text(GTK_LABEL(lbl_track), ui_text);
            }

            if (current_rec == 0 && pristine_bt_buf && multitrack_tracks[0]) {
                memcpy(pristine_bt_buf, multitrack_tracks[0], pristine_frames * 2 * sizeof(float));
            }

            int next_rec = current_rec + 1;
            if (next_rec < MAX_TRACKS) {
                atomic_store_explicit(&current_recording_track, next_rec, memory_order_release);
                int new_active = atomic_load_explicit(&active_track_count, memory_order_acquire);
                if (next_rec >= new_active) {
                    atomic_store_explicit(&active_track_count, next_rec + 1, memory_order_release);
                }
            }

            size_t fade_len = (current_pos < 256) ? current_pos : 256;
            if (fade_len > 0) {
                int rec_track = atomic_load_explicit(&current_recording_track, memory_order_acquire);
                for (size_t f = 0; f < fade_len; f++) {
                    float multiplier = (float)(fade_len - 1 - f) / (float)(fade_len - 1);
                    size_t idx = current_pos - fade_len + f;

                    if (rec_track >= 0 && rec_track < MAX_TRACKS && multitrack_tracks[rec_track]) {
                        multitrack_tracks[rec_track][idx * 2] *= multiplier;
                        multitrack_tracks[rec_track][idx * 2 + 1] *= multiplier;
                    }
                }
            }

            update_zoom_button_label_to_length();
            invalidate_waveform_caches();
        }
        resume_rt_thread();
    } else {
        printf("CRITICAL: Skipped canvas crop and memory operations due to RT Detach Timeout.\n");
    }

    if (atomic_load_explicit(&is_multitrack_mode, memory_order_acquire)) {
        update_multitrack_status_ui();
        invalidate_waveform_caches();
        if (waveform_area_bt) gtk_widget_queue_draw(waveform_area_bt);
        if (waveform_area_input) gtk_widget_queue_draw(waveform_area_input);
    }

    stop_encoder();

    if (ui_state.session_is_dirty && !atomic_load_explicit(&is_multitrack_mode, memory_order_acquire) && !was_recording) {
        if (atomic_load_explicit(&mkv_video_keep_running, memory_order_acquire)) {
            atomic_store_explicit(&mkv_video_keep_running, false, memory_order_release);
            pthread_join(mkv_video_thread, NULL);
        }
        if (g_str_has_suffix(ui_state.selected_track_path, ".mkv")) {
            atomic_store_explicit(&is_mkv_mode, true, memory_order_release);
            atomic_store_explicit(&mkv_video_keep_running, true, memory_order_release);
            pthread_create(&mkv_video_thread, NULL, mkv_video_loop, g_strdup(ui_state.selected_track_path));
        } else {
            atomic_store_explicit(&is_mkv_mode, false, memory_order_release);
        }
    }

    if (!atomic_load_explicit(&is_multitrack_mode, memory_order_acquire)) {
        if (atomic_load_explicit(&loop_active, memory_order_acquire)) {
            double l_start = (double)atomic_load_explicit(&loop_start_frame, memory_order_relaxed) / (double)backing_track_frames;
            seek_backing_track(l_start);
        } else {
            seek_backing_track(0.0);
        }
    }

    gtk_label_set_text(GTK_LABEL(lbl_status), "Status: Stopped & Ready to Mux");
    gtk_button_set_icon_name(GTK_BUTTON(btn_play), "media-playback-start-symbolic");
    gtk_widget_set_tooltip_text(btn_play, "Play (Space / P)");

    gtk_widget_set_sensitive(btn_load, TRUE);
    gtk_widget_set_sensitive(btn_settings, TRUE);
    gtk_widget_set_sensitive(btn_prev, TRUE);
    gtk_widget_set_sensitive(btn_next, TRUE);
    gtk_widget_set_sensitive(btn_speed, TRUE);
    gtk_widget_set_sensitive(btn_play, TRUE);
    gtk_widget_set_sensitive(btn_record, TRUE);
    gtk_widget_set_sensitive(btn_save_mux, TRUE);
    gtk_widget_set_sensitive(btn_stop, FALSE);

    if (was_recording) {
        if (btn_save_mux) {
            gtk_widget_remove_css_class(btn_save_mux, "needs-save");
            gtk_widget_add_css_class(btn_save_mux, "needs-save");
        }
        if (btn_save_session && atomic_load_explicit(&is_multitrack_mode, memory_order_acquire)) {
            gtk_widget_remove_css_class(btn_save_session, "needs-save");
            gtk_widget_add_css_class(btn_save_session, "needs-save");
        }
    }

    if (btn_play) gtk_widget_grab_focus(btn_play);
}

/**
 * @brief Executes a full hard reset of the audio and video workspace, wiping all
 * engine memory buffers, resetting all UI layer states, and clearing Cairo canvas caches.
 */
/**
 * @brief UI callback for the global reset button. Prompts the user before wiping the canvas.
 */
static void on_global_reset_confirm(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    (void)user_data;
    GtkAlertDialog *alert = GTK_ALERT_DIALOG(source_object);
    int response = gtk_alert_dialog_choose_finish(alert, res, NULL);

    if (response == 0) { // Confirmed
        on_stop_clicked(NULL, NULL);
        clear_loop_points();

        // 1. Kill any active MKV video thread from a loaded backing track or preview
        if (atomic_load_explicit(&mkv_video_keep_running, memory_order_acquire)) {
            atomic_store_explicit(&mkv_video_keep_running, false, memory_order_release);
            pthread_join(mkv_video_thread, NULL);
        }
        atomic_store_explicit(&is_mkv_mode, false, memory_order_release);

        // 2. Eject the loaded track
        ui_state.selected_track_path[0] = '\0';
        update_playlist_toggle_state();

        // 3. Clear global engine state and dirty flags
        prepare_engine_for_new_track();
        ui_state.session_is_dirty = false;

        bool is_looper = atomic_load_explicit(&is_multitrack_mode, memory_order_acquire);

        // 4. HARD RESET: Force the looper engine to forget all overdubs before canvas allocation
        if (is_looper) {
            atomic_store_explicit(&active_track_count, 2, memory_order_release);
            atomic_store_explicit(&current_recording_track, 1, memory_order_release);
        }

        /** Reset all layer clip gains back to 0 dB (1.0 multiplier) on a fresh wipe */
        for (int i = 0; i < MAX_TRACKS; i++) {
            set_multitrack_layer_gain(i, 1.0f);
        }

        // 5. Deploy a fresh canvas based on the current mode (Looper vs Freestyle)
        adjust_blank_canvas_for_mode(is_looper);

        // 6. HARD RESET: Force the UI to visually collapse back to Layer 1
        reset_multitrack_ui_states();
        if (is_looper) {
            refresh_multitrack_tracks_ui();
            update_multitrack_status_ui();
        }

        // 7. HARD RESET: Nuke Cairo caches and force a screen redraw to wipe old waveforms
        invalidate_waveform_caches();
        if (waveform_area_bt) gtk_widget_queue_draw(waveform_area_bt);
        if (waveform_area_input) gtk_widget_queue_draw(waveform_area_input);

        gtk_label_set_text(GTK_LABEL(lbl_status), "Status: Recording Canvas Reset");
    }
}

void on_play_clicked(GtkButton *button, gpointer user_data) {
    (void)button; (void)user_data;

    bool is_playing = atomic_load_explicit(&engine_is_playing, memory_order_acquire);
    bool is_paused = atomic_load_explicit(&engine_is_paused, memory_order_acquire);
    bool is_recording = atomic_load_explicit(&engine_is_recording, memory_order_acquire);

    if (!is_playing && !is_paused && !is_recording) {
        size_t current_pos = atomic_load_explicit(&playback_pos, memory_order_acquire);
        size_t bt_frames = atomic_load_explicit(&backing_track_frames, memory_order_acquire);
        if (bt_frames > 0 && current_pos >= bt_frames) seek_backing_track(0.0);

        // Preview the recorded video file during playback of a dirty session
        if (ui_state.session_is_dirty && !atomic_load_explicit(&is_multitrack_mode, memory_order_acquire)) {
            if (atomic_load_explicit(&mkv_video_keep_running, memory_order_acquire)) {
                atomic_store_explicit(&mkv_video_keep_running, false, memory_order_release);
                pthread_join(mkv_video_thread, NULL);
            }
            if (access(current_raw_path, F_OK) == 0) {
                atomic_store_explicit(&is_mkv_mode, true, memory_order_release);
                atomic_store_explicit(&mkv_video_keep_running, true, memory_order_release);
                pthread_create(&mkv_video_thread, NULL, mkv_video_loop, g_strdup(current_raw_path));
            }
        }

        atomic_store_explicit(&engine_is_playing, true, memory_order_release);
        gtk_label_set_text(GTK_LABEL(lbl_status), "Status: PLAYING");
        gtk_button_set_icon_name(GTK_BUTTON(btn_play), "media-playback-pause-symbolic");
        gtk_widget_set_tooltip_text(btn_play, "Pause");

        gtk_widget_set_sensitive(btn_load, FALSE);
        gtk_widget_set_sensitive(btn_record, FALSE);
        gtk_widget_set_sensitive(btn_stop, TRUE);
        gtk_widget_set_sensitive(btn_save_mux, FALSE);
    } else if (is_playing || is_recording) {
        bool new_pause_state = !is_paused;
        atomic_store_explicit(&engine_is_paused, new_pause_state, memory_order_release);

        if (new_pause_state) {
            gtk_label_set_text(GTK_LABEL(lbl_status), "Status: PAUSED");
            gtk_button_set_icon_name(GTK_BUTTON(btn_play), "media-playback-start-symbolic");
            gtk_widget_set_tooltip_text(btn_play, "Resume (Space / P)");
        } else {
            if (is_playing) gtk_label_set_text(GTK_LABEL(lbl_status), "Status: PLAYING");
            else gtk_label_set_text(GTK_LABEL(lbl_status), "Status: Recording");
            gtk_button_set_icon_name(GTK_BUTTON(btn_play), "media-playback-pause-symbolic");
            gtk_widget_set_tooltip_text(btn_play, "Pause (Space / P)");
        }
    }
}

/**
 * @brief UI callback for the global reset button. Prompts the user before wiping the canvas.
 */
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
        ui_state.config.audio_profiles[ui_state.config.num_audio_profiles].in_cycler = 0;
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

static void on_master_cut_pressed(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data) {
    (void)n_press; (void)x; (void)y; (void)user_data;
    GdkModifierType state = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));
    bool has_ctrl = (state & GDK_CONTROL_MASK) != 0;
    bool has_shift = (state & GDK_SHIFT_MASK) != 0;

    if (has_shift) {
        master_blend_fade();
    } else if (has_ctrl) {
        master_smart_fade();
    } else {
        master_cut_selection();
    }

    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);

    // Refresh GUI states to account for newly reverted/blank tracks
    if (atomic_load_explicit(&is_multitrack_mode, memory_order_acquire)) {
        refresh_multitrack_tracks_ui();
    }

    invalidate_waveform_caches();
    if (waveform_area_bt) gtk_widget_queue_draw(waveform_area_bt);
    if (waveform_area_input) gtk_widget_queue_draw(waveform_area_input);
}

static void on_master_undo_pressed(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data) {
    (void)n_press; (void)x; (void)y; (void)user_data;

    master_undo_edits();

    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);

    // Refresh GUI states to account for restored tracks
    if (atomic_load_explicit(&is_multitrack_mode, memory_order_acquire)) {
        refresh_multitrack_tracks_ui();
    }

    invalidate_waveform_caches();
    if (waveform_area_bt) gtk_widget_queue_draw(waveform_area_bt);
    if (waveform_area_input) gtk_widget_queue_draw(waveform_area_input);
}

void ui_dashboard_init(GtkBuilder *b_dash, GtkBuilder *b_wave) {
    lbl_input_device = GTK_WIDGET(gtk_builder_get_object(b_dash, "lbl_input_device"));
    char input_label_text[256];
    snprintf(input_label_text, sizeof(input_label_text), "%s Gain (Pre-Buffer dB):", ui_state.config.audio_device[0] ? ui_state.config.audio_device : "Input");
    gtk_label_set_text(GTK_LABEL(lbl_input_device), input_label_text);

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
    g_signal_connect(btn_record, "clicked", G_CALLBACK(on_start_clicked), NULL);

    btn_stop = GTK_WIDGET(gtk_builder_get_object(b_dash, "btn_stop"));
    g_signal_connect(btn_stop, "clicked", G_CALLBACK(on_stop_clicked), NULL);

    btn_reset = GTK_WIDGET(gtk_builder_get_object(b_dash, "btn_reset"));
    g_signal_connect(btn_reset, "clicked", G_CALLBACK(on_global_reset_clicked), NULL);

    GtkWidget *btn_master_cut = GTK_WIDGET(gtk_builder_get_object(b_dash, "btn_master_cut"));
    if (btn_master_cut) {
        GtkGesture *master_cut_click = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(master_cut_click), GDK_BUTTON_PRIMARY);
        g_signal_connect(master_cut_click, "pressed", G_CALLBACK(on_master_cut_pressed), NULL);
        gtk_widget_add_controller(btn_master_cut, GTK_EVENT_CONTROLLER(master_cut_click));
    }

    GtkWidget *btn_master_undo = GTK_WIDGET(gtk_builder_get_object(b_dash, "btn_master_undo"));
    if (btn_master_undo) {
        GtkGesture *master_undo_click = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(master_undo_click), GDK_BUTTON_PRIMARY);
        g_signal_connect(master_undo_click, "pressed", G_CALLBACK(on_master_undo_pressed), NULL);
        gtk_widget_add_controller(btn_master_undo, GTK_EVENT_CONTROLLER(master_undo_click));
    }

    btn_prev = GTK_WIDGET(gtk_builder_get_object(b_dash, "btn_prev"));
    g_signal_connect(btn_prev, "clicked", G_CALLBACK(on_prev_track_clicked), NULL);
    btn_next = GTK_WIDGET(gtk_builder_get_object(b_dash, "btn_next"));
    g_signal_connect(btn_next, "clicked", G_CALLBACK(on_next_track_clicked), NULL);

    btn_speed = GTK_WIDGET(gtk_builder_get_object(b_dash, "btn_speed"));
    g_signal_connect(btn_speed, "clicked", G_CALLBACK(on_speed_clicked), NULL);

    btn_zoom = GTK_WIDGET(gtk_builder_get_object(b_dash, "btn_zoom"));
    g_signal_connect(btn_zoom, "clicked", G_CALLBACK(on_zoom_clicked), NULL);

    btn_playlist_toggle = GTK_WIDGET(gtk_builder_get_object(b_dash, "btn_playlist_toggle"));
    g_signal_connect(btn_playlist_toggle, "clicked", G_CALLBACK(on_playlist_toggle_clicked), NULL);

    input_cycler_drop = GTK_WIDGET(gtk_builder_get_object(b_dash, "input_cycler_drop"));
    if (input_cycler_drop) {
        g_signal_connect(input_cycler_drop, "notify::selected", G_CALLBACK(on_cycler_selection_changed), NULL);
        update_dashboard_cycler_ui();
    }

    lbl_hw_toast = GTK_WIDGET(gtk_builder_get_object(b_dash, "lbl_hw_toast"));
    if (lbl_hw_toast) {
        gtk_widget_set_valign(lbl_hw_toast, GTK_ALIGN_START);
    }

    // Waveform mapping routes exclusively through b_wave
    waveform_area_bt = GTK_WIDGET(gtk_builder_get_object(b_wave, "waveform_area_bt"));
    GtkEventController *scroll_ctrl_bt = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
    g_signal_connect(scroll_ctrl_bt, "scroll", G_CALLBACK(on_waveform_scroll), NULL);
    gtk_widget_add_controller(waveform_area_bt, scroll_ctrl_bt);

    waveform_area_input = GTK_WIDGET(gtk_builder_get_object(b_wave, "waveform_area_input"));
    GtkEventController *scroll_ctrl_in = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
    g_signal_connect(scroll_ctrl_in, "scroll", G_CALLBACK(on_waveform_scroll), NULL);
    gtk_widget_add_controller(waveform_area_input, scroll_ctrl_in);
}
