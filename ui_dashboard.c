#include "ui_dashboard.h"
#include "ui_globals.h"
#include "audio_engine.h"
#include "ui_waveforms.h"
#include "ui_playlist.h"
#include "ui_looper.h"
#include "encoder.h"
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
static guint zoom_debounce_id = 0;

static guint countdown_timer_id = 0;
static int countdown_val = 3;
static GtkWidget *countdown_window = NULL;
static GtkWidget *countdown_label = NULL;

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
    zoom_debounce_id = 0;
    return G_SOURCE_REMOVE;
}

static void on_zoom_clicked(GtkButton *button, gpointer user_data) {
    (void)button; (void)user_data;
    zoom_multiplier = 1.0;
    update_zoom_button_label_to_length();
    apply_zoom_deferred(NULL);
}

static gboolean on_waveform_scroll(GtkEventControllerScroll *controller, double dx, double dy, gpointer user_data) {
    (void)controller; (void)dx; (void)user_data;
    GdkModifierType state = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(controller));
    if ((state & GDK_CONTROL_MASK) == 0) return FALSE;

    if (dy > 0) zoom_multiplier /= 1.2;
    else if (dy < 0) zoom_multiplier *= 1.2;

    size_t frames = atomic_load_explicit(&backing_track_frames, memory_order_acquire);
    int rate = atomic_load_explicit(&active_sample_rate, memory_order_acquire);

    if (frames > 0 && rate > 0) {
        double total_sec = (double)frames / rate;
        double max_zoom = total_sec / 0.0005;
        if (max_zoom < 1.0) max_zoom = 1.0;

        if (zoom_multiplier < 1.0) zoom_multiplier = 1.0;
        if (zoom_multiplier > max_zoom) zoom_multiplier = max_zoom;

        if (zoom_multiplier == 1.0) {
            update_zoom_button_label_to_length();
        } else {
            double visible_sec = total_sec / zoom_multiplier;
            char zoom_str[32];
            if (visible_sec >= 60.0) snprintf(zoom_str, sizeof(zoom_str), "View: %dm %02ds", (int)visible_sec / 60, (int)visible_sec % 60);
            else if (visible_sec >= 1.0) snprintf(zoom_str, sizeof(zoom_str), "View: %.1fs", visible_sec);
            else snprintf(zoom_str, sizeof(zoom_str), "View: %.1fms", visible_sec * 1000.0);
            gtk_button_set_label(GTK_BUTTON(btn_zoom), zoom_str);
        }
    }

    if (zoom_debounce_id != 0) g_source_remove(zoom_debounce_id);
    zoom_debounce_id = g_timeout_add(8, apply_zoom_deferred, NULL);
    return TRUE;
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
    ui_state.session_is_dirty = false; // NEW: A cleared or fresh track is inherently clean
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

    int target_w = ui_state.config.video_preview_width;
    int target_h = ui_state.config.video_preview_height;

    while (atomic_load_explicit(&mkv_video_keep_running, memory_order_acquire)) {
        int current_rate = atomic_load_explicit(&active_sample_rate, memory_order_acquire);
        if (current_rate == 0) { usleep(10000); continue; }

        size_t pos = atomic_load_explicit(&playback_pos, memory_order_acquire);
        double current_audio_time = (double)pos / current_rate;

        // FIX: Synchronize the original MKV video time with the Stretched audio playback time
        if (fabs(current_audio_time - last_audio_time) > 0.25 && last_audio_time >= 0.0) {
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

                // FIX: Map standby loop seek target to original MKV video time
                if (new_audio_time < last_standby_time - 0.5) {
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
        g_free(basename);

        gtk_label_set_text(GTK_LABEL(lbl_track), ui_text);
        gtk_label_set_text(GTK_LABEL(lbl_status), "Status: Ready");
        gtk_widget_set_sensitive(btn_play, TRUE);

        // FIX: Always allow recording over loaded tracks, including MKVs
        bool is_mkv = g_str_has_suffix(ui_state.selected_track_path, ".mkv");
        bool is_looper = atomic_load_explicit(&is_looper_mode, memory_order_acquire);
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
        if (is_looper) update_looper_status_ui();
        update_zoom_button_label_to_length();
    } else if (result == -2) {
        gtk_label_set_markup(GTK_LABEL(lbl_status), "<span foreground='#ff4444'><b>Status: Track exceeds safe RAM limits</b></span>");
        gtk_label_set_text(GTK_LABEL(lbl_track), "Track: None Selected");
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

    if (sw_blank_canvas && gtk_switch_get_active(GTK_SWITCH(sw_blank_canvas))) {
        gtk_switch_set_active(GTK_SWITCH(sw_blank_canvas), FALSE);
    }

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
    int loop_min = ui_state.config.looper_duration_min > 0 ? ui_state.config.looper_duration_min : 5;

    if (is_looper) {
        // FIX: Auto-enable blank canvas if no track is loaded to keep the record button armed
        if (sw_blank_canvas && !gtk_switch_get_active(GTK_SWITCH(sw_blank_canvas))) {
            g_signal_handlers_block_by_func(sw_blank_canvas, G_CALLBACK(on_blank_canvas_toggled), NULL);
            gtk_switch_set_active(GTK_SWITCH(sw_blank_canvas), TRUE);
            g_signal_handlers_unblock_by_func(sw_blank_canvas, G_CALLBACK(on_blank_canvas_toggled), NULL);
        }

        if (init_empty_loop_canvas(loop_min * 60) == 0) {
            char track_lbl[128];
            snprintf(track_lbl, sizeof(track_lbl), "Track: Blank Loop Canvas [%02d:00]", loop_min);
            gtk_label_set_text(GTK_LABEL(lbl_track), track_lbl);
            gtk_widget_set_sensitive(btn_play, TRUE);
            gtk_widget_set_sensitive(btn_record, TRUE);
        }
    } else {
        // Normal mode ALWAYS receives a freestyle canvas if no track is loaded
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

    if (current_bt_frames > 0 && current_pos >= current_bt_frames) seek_backing_track(0.0);

    // FIX: Force removal of green CSS classes when initiating a brand new recording
    if (btn_save_mux) gtk_widget_remove_css_class(btn_save_mux, "needs-save");
    if (btn_save_session) gtk_widget_remove_css_class(btn_save_session, "needs-save");

    ui_state.session_is_dirty = true;

    if (current_bt_frames == 0) {
        bool is_looper = atomic_load_explicit(&is_looper_mode, memory_order_acquire);
        int free_min = ui_state.config.freestyle_duration_min > 0 ? ui_state.config.freestyle_duration_min : 15;
        int loop_min = ui_state.config.looper_duration_min > 0 ? ui_state.config.looper_duration_min : 5;
        int duration_sec = is_looper ? (loop_min * 60) : (free_min * 60);

        if (init_empty_loop_canvas(duration_sec) != 0) {
            gtk_label_set_text(GTK_LABEL(lbl_status), "Status: Insufficient RAM for blank canvas.");
            return;
        }

        if (is_looper) {
            char track_lbl[128];
            snprintf(track_lbl, sizeof(track_lbl), "Track: Blank Loop Canvas [%02d:00 limit]", loop_min);
            gtk_label_set_text(GTK_LABEL(lbl_track), track_lbl);

            if (sw_blank_canvas && !gtk_switch_get_active(GTK_SWITCH(sw_blank_canvas))) {
                g_signal_handlers_block_by_func(sw_blank_canvas, G_CALLBACK(on_blank_canvas_toggled), NULL);
                gtk_switch_set_active(GTK_SWITCH(sw_blank_canvas), TRUE);
                g_signal_handlers_unblock_by_func(sw_blank_canvas, G_CALLBACK(on_blank_canvas_toggled), NULL);
            }
            update_looper_status_ui();
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

    snprintf(final_save_path, sizeof(final_save_path), "%s/%04d%02d%02d-%02d%02d%02d-%s-RAW.mkv",
             ui_state.config.recordings_dir, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec, base_name);

    snprintf(current_raw_path, sizeof(current_raw_path), "/tmp/qjams_raw_%04d%02d%02d_%02d%02d%02d.mkv",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);

    int current_samplerate = client ? jack_get_sample_rate(client) : 48000;

    if (!atomic_load_explicit(&is_looper_mode, memory_order_acquire)) {
        if (init_and_start_encoder(current_raw_path, 1280, 720, current_samplerate, &video_queue) != 0) {
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
    gtk_widget_set_tooltip_text(btn_play, "Pause");

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
    } else if (countdown_val == -1) {
        if (countdown_window) {
            gtk_window_destroy(GTK_WINDOW(countdown_window));
            countdown_window = NULL;
        }
        countdown_val--;
        return G_SOURCE_CONTINUE;
    } else {
        start_recording_execution();
        countdown_timer_id = 0;
        return G_SOURCE_REMOVE;
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
        g_source_remove(countdown_timer_id);
        countdown_timer_id = 0;
        if (countdown_window) { gtk_window_destroy(GTK_WINDOW(countdown_window)); countdown_window = NULL; }
        gtk_label_set_text(GTK_LABEL(lbl_status), "Status: Recording Aborted");
        gtk_widget_set_sensitive(btn_record, TRUE);
        gtk_widget_set_sensitive(btn_play, TRUE);
        gtk_widget_set_sensitive(btn_stop, FALSE);

        atomic_store_explicit(&engine_is_playing, false, memory_order_release);
        atomic_store_explicit(&engine_is_recording, false, memory_order_release);
        return;
    }

    atomic_store_explicit(&engine_is_playing, false, memory_order_release);
    atomic_store_explicit(&engine_is_recording, false, memory_order_release);
    atomic_store_explicit(&engine_is_armed, false, memory_order_release);
    atomic_store_explicit(&engine_is_paused, false, memory_order_release);

    // --- STRICT CANVAS CROP LOGIC ---
    size_t current_pos = atomic_load_explicit(&playback_pos, memory_order_acquire);

    if (was_recording && current_pos > 0) {
        bool is_looper = atomic_load_explicit(&is_looper_mode, memory_order_acquire);
        bool is_blank_canvas = (strlen(ui_state.selected_track_path) == 0);

        if (is_looper) {
            int current_rec = atomic_load_explicit(&current_recording_layer, memory_order_acquire);
            if (current_rec == 0) {
                atomic_store_explicit(&backing_track_frames, current_pos, memory_order_release);
                pristine_frames = current_pos;

                if (pristine_bt_buf && loop_layers[0]) {
                    atomic_store_explicit(&request_track_free, true, memory_order_release);
                    int timeout = 500;
                    while (!atomic_load_explicit(&safe_to_free_track, memory_order_acquire) && timeout > 0) { usleep(1000); timeout--; }

                    memcpy(pristine_bt_buf, loop_layers[0], current_pos * 2 * sizeof(float));

                    atomic_store_explicit(&safe_to_free_track, false, memory_order_relaxed);
                    atomic_store_explicit(&request_track_free, false, memory_order_release);
                }

                atomic_store_explicit(&active_layer_count, 2, memory_order_release);
                atomic_store_explicit(&current_recording_layer, 1, memory_order_release);

                char ui_text[128];
                int current_rate = atomic_load_explicit(&active_sample_rate, memory_order_acquire);
                int total_secs = (current_rate > 0) ? (current_pos / current_rate) : 0;
                snprintf(ui_text, sizeof(ui_text), "Track: Custom Loop [%02d:%02d]", total_secs / 60, total_secs % 60);
                gtk_label_set_text(GTK_LABEL(lbl_track), ui_text);
                seek_backing_track(0.0);
            } else {
                int active = atomic_load_explicit(&active_layer_count, memory_order_acquire);
                if (current_rec >= active) atomic_store_explicit(&active_layer_count, current_rec + 1, memory_order_release);
                if (current_rec < MAX_LOOPS - 1) {
                    atomic_store_explicit(&current_recording_layer, current_rec + 1, memory_order_release);
                    int new_active = atomic_load_explicit(&active_layer_count, memory_order_acquire);
                    if (current_rec + 1 >= new_active) atomic_store_explicit(&active_layer_count, current_rec + 2, memory_order_release);
                }
            }
        } else if (is_blank_canvas) {
            // FREESTYLE MODE: Crop the 15-minute padding down to the exact recording limit
            atomic_store_explicit(&backing_track_frames, current_pos, memory_order_release);
            pristine_frames = current_pos;

            char ui_text[128];
            int current_rate = atomic_load_explicit(&active_sample_rate, memory_order_acquire);
            int total_secs = (current_rate > 0) ? (current_pos / current_rate) : 0;
            snprintf(ui_text, sizeof(ui_text), "Track: Freestyle Take [%02d:%02d]", total_secs / 60, total_secs % 60);
            gtk_label_set_text(GTK_LABEL(lbl_track), ui_text);
        }

        // Force the zoom boundaries to scale perfectly to the newly cropped track length
        update_zoom_button_label_to_length();
        invalidate_waveform_caches();
    }

    if (atomic_load_explicit(&is_looper_mode, memory_order_acquire)) {
        update_looper_status_ui();
        invalidate_waveform_caches();
        gtk_widget_queue_draw(waveform_area_bt);
        gtk_widget_queue_draw(waveform_area_input);
    }

    stop_encoder();

    // Restore live camera or original MKV backing track when ending playback of a recorded take
    if (ui_state.session_is_dirty && !atomic_load_explicit(&is_looper_mode, memory_order_acquire) && !was_recording) {
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

    if (!atomic_load_explicit(&is_looper_mode, memory_order_acquire)) {
        if (atomic_load_explicit(&loop_active, memory_order_acquire)) {
            double l_start = (double)atomic_load_explicit(&loop_start_frame, memory_order_relaxed) / (double)backing_track_frames;
            seek_backing_track(l_start);
        } else {
            seek_backing_track(0.0);
        }
    }

    gtk_label_set_text(GTK_LABEL(lbl_status), "Status: Stopped & Ready to Mux");
    gtk_button_set_icon_name(GTK_BUTTON(btn_play), "media-playback-start-symbolic");
    gtk_widget_set_tooltip_text(btn_play, "Play");

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
        if (btn_save_session && atomic_load_explicit(&is_looper_mode, memory_order_acquire)) {
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

        bool is_looper = atomic_load_explicit(&is_looper_mode, memory_order_acquire);

        // 4. HARD RESET: Force the looper engine to forget all overdubs before canvas allocation
        if (is_looper) {
            atomic_store_explicit(&active_layer_count, 2, memory_order_release);
            atomic_store_explicit(&current_recording_layer, 1, memory_order_release);
            for (int i = 0; i < 6; i++) {
                atomic_store_explicit(&layer_is_muted[i], false, memory_order_release);
            }
        }

        // 5. Deploy a fresh canvas based on the current mode (Looper vs Freestyle)
        adjust_blank_canvas_for_mode(is_looper);

        // 6. HARD RESET: Force the UI to visually collapse back to Layer 1
        reset_looper_ui_states();
        if (is_looper) {
            refresh_looper_layers_ui();
            update_looper_status_ui();
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
        if (ui_state.session_is_dirty && !atomic_load_explicit(&is_looper_mode, memory_order_acquire)) {
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
            gtk_widget_set_tooltip_text(btn_play, "Resume");
        } else {
            if (is_playing) gtk_label_set_text(GTK_LABEL(lbl_status), "Status: PLAYING");
            else gtk_label_set_text(GTK_LABEL(lbl_status), "Status: Recording");
            gtk_button_set_icon_name(GTK_BUTTON(btn_play), "media-playback-pause-symbolic");
            gtk_widget_set_tooltip_text(btn_play, "Pause");
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
    gtk_alert_dialog_set_default_button(alert, 1);

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
    for (int i = 0; i < ui_state.config.num_saved_input_gains; i++) {
        if (g_strcmp0(ui_state.config.input_gains[i].device_name, ui_state.config.audio_device) == 0) {
            ui_state.config.input_gains[i].gain_multiplier = multiplier;
            found = 1;
            break;
        }
    }
    if (!found && ui_state.config.num_saved_input_gains < MAX_SAVED_DEVICES) {
        strncpy(ui_state.config.input_gains[ui_state.config.num_saved_input_gains].device_name, ui_state.config.audio_device, 127);
        ui_state.config.input_gains[ui_state.config.num_saved_input_gains].gain_multiplier = multiplier;
        ui_state.config.num_saved_input_gains++;
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

    // FIX: Force visual redraw of the cached backing track when gain changes
    invalidate_waveform_caches();
    if (waveform_area_bt) gtk_widget_queue_draw(waveform_area_bt);
}

void ui_dashboard_init(GtkBuilder *builder) {
    lbl_input_device = GTK_WIDGET(gtk_builder_get_object(builder, "lbl_input_device"));
    char input_label_text[256];
    snprintf(input_label_text, sizeof(input_label_text), "%s Gain (dB):", ui_state.config.audio_device[0] ? ui_state.config.audio_device : "Input");
    gtk_label_set_text(GTK_LABEL(lbl_input_device), input_label_text);

    input_gain_spinner = GTK_WIDGET(gtk_builder_get_object(builder, "input_gain_spinner"));
    float loaded_db = (ui_state.config.input_gain_multiplier <= 0.001f) ? -24.0f : 20.0f * log10f(ui_state.config.input_gain_multiplier);
    GtkAdjustment *input_adj = gtk_adjustment_new(loaded_db, -24.0, 24.0, 1.0, 5.0, 0.0);
    gtk_spin_button_set_adjustment(GTK_SPIN_BUTTON(input_gain_spinner), input_adj);
    gtk_spin_button_set_climb_rate(GTK_SPIN_BUTTON(input_gain_spinner), 1.0);
    g_signal_connect(input_gain_spinner, "value-changed", G_CALLBACK(on_input_gain_changed), NULL);

    GtkWidget *bt_gain_spinner = GTK_WIDGET(gtk_builder_get_object(builder, "bt_gain_spinner"));
    float loaded_bt_db = (ui_state.config.bt_gain_multiplier <= 0.001f) ? -24.0f : 20.0f * log10f(ui_state.config.bt_gain_multiplier);
    GtkAdjustment *bt_adj = gtk_adjustment_new(loaded_bt_db, -24.0, 24.0, 1.0, 5.0, 0.0);
    gtk_spin_button_set_adjustment(GTK_SPIN_BUTTON(bt_gain_spinner), bt_adj);
    gtk_spin_button_set_climb_rate(GTK_SPIN_BUTTON(bt_gain_spinner), 1.0);
    g_signal_connect(bt_gain_spinner, "value-changed", G_CALLBACK(on_bt_gain_changed), NULL);

    GtkWidget *vu_input_l = GTK_WIDGET(gtk_builder_get_object(builder, "vu_input_l"));
    GtkWidget *vu_input_r = GTK_WIDGET(gtk_builder_get_object(builder, "vu_input_r"));
    GtkWidget *vu_bt_l = GTK_WIDGET(gtk_builder_get_object(builder, "vu_bt_l"));
    GtkWidget *vu_bt_r = GTK_WIDGET(gtk_builder_get_object(builder, "vu_bt_r"));
    bind_meter(vu_input_l, &vu_peak_input_l);
    bind_meter(vu_input_r, &vu_peak_input_r);
    bind_meter(vu_bt_l, &vu_peak_bt_l);
    bind_meter(vu_bt_r, &vu_peak_bt_r);

    lbl_track = GTK_WIDGET(gtk_builder_get_object(builder, "lbl_track"));
    gtk_label_set_ellipsize(GTK_LABEL(lbl_track), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(lbl_track, TRUE);
    gtk_widget_set_halign(lbl_track, GTK_ALIGN_START);

    btn_play = GTK_WIDGET(gtk_builder_get_object(builder, "btn_play"));
    g_signal_connect(btn_play, "clicked", G_CALLBACK(on_play_clicked), NULL);

    btn_record = GTK_WIDGET(gtk_builder_get_object(builder, "btn_record"));
    g_signal_connect(btn_record, "clicked", G_CALLBACK(on_start_clicked), NULL);

    btn_stop = GTK_WIDGET(gtk_builder_get_object(builder, "btn_stop"));
    g_signal_connect(btn_stop, "clicked", G_CALLBACK(on_stop_clicked), NULL);

    // NEW: Bind Reset button globally
    btn_reset = GTK_WIDGET(gtk_builder_get_object(builder, "btn_reset"));
    g_signal_connect(btn_reset, "clicked", G_CALLBACK(on_global_reset_clicked), NULL);

    btn_prev = GTK_WIDGET(gtk_builder_get_object(builder, "btn_prev"));
    g_signal_connect(btn_prev, "clicked", G_CALLBACK(on_prev_track_clicked), NULL);

    btn_next = GTK_WIDGET(gtk_builder_get_object(builder, "btn_next"));
    g_signal_connect(btn_next, "clicked", G_CALLBACK(on_next_track_clicked), NULL);

    btn_speed = GTK_WIDGET(gtk_builder_get_object(builder, "btn_speed"));
    g_signal_connect(btn_speed, "clicked", G_CALLBACK(on_speed_clicked), NULL);

    btn_zoom = GTK_WIDGET(gtk_builder_get_object(builder, "btn_zoom"));
    g_signal_connect(btn_zoom, "clicked", G_CALLBACK(on_zoom_clicked), NULL);

    btn_playlist_toggle = GTK_WIDGET(gtk_builder_get_object(builder, "btn_playlist_toggle"));
    g_signal_connect(btn_playlist_toggle, "clicked", G_CALLBACK(on_playlist_toggle_clicked), NULL);

    waveform_area_bt = GTK_WIDGET(gtk_builder_get_object(builder, "waveform_area_bt"));
    GtkEventController *scroll_ctrl_bt = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    g_signal_connect(scroll_ctrl_bt, "scroll", G_CALLBACK(on_waveform_scroll), NULL);
    gtk_widget_add_controller(waveform_area_bt, scroll_ctrl_bt);

    waveform_area_input = GTK_WIDGET(gtk_builder_get_object(builder, "waveform_area_input"));
    GtkEventController *scroll_ctrl_in = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    g_signal_connect(scroll_ctrl_in, "scroll", G_CALLBACK(on_waveform_scroll), NULL);
    gtk_widget_add_controller(waveform_area_input, scroll_ctrl_in);
}
