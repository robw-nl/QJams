#include "ui_waveforms.h"
#include "audio_core.h"
#include "audio_internal.h"
#include "audio_memory.h"
#include "encoder.h"
#include "media_io.h"
#include "ui_dashboard.h"
#include "ui_globals.h"
#include "ui_multitrack.h"
#include "ui_transport.h"
#include "video_engine.h"
#include <math.h>

static double drag_start_fraction = -1.0;
static double current_drag_fraction = -1.0;
static bool is_dragging = false;
static bool is_panning = false;
static double drag_start_scroll_x = 0.0;

#define MAX_UI_WIDTH 8192

static int cached_bt_width = 0;
static int cached_bt_height = 0;
static int cached_active_tracks = 1;
static size_t cached_bt_frames = 0;

// Dynamic BT Sweep caches
static float sweep_min_l[MAX_UI_WIDTH], sweep_max_l[MAX_UI_WIDTH];
static float sweep_min_r[MAX_UI_WIDTH], sweep_max_r[MAX_UI_WIDTH];
static int sweep_parity[MAX_UI_WIDTH];

static int cached_input_width = 0;
static int cached_input_height = 0;
static float input_min_l[MAX_UI_WIDTH], input_max_l[MAX_UI_WIDTH];
static float input_min_r[MAX_UI_WIDTH], input_max_r[MAX_UI_WIDTH];
static int input_parity[MAX_UI_WIDTH];

// Off-screen Cairo cache surfaces
static cairo_surface_t *surface_bt = NULL;
static cairo_surface_t *surface_input_grid = NULL;

extern _Atomic bool request_track_free;
extern _Atomic bool ui_scanner_is_busy;

static _Atomic bool bt_render_active = false;

// --- Hover & Snap State ---
double shared_hover_x = -1.0;
bool shared_is_hovering = false;
size_t shared_snap_frame = (size_t)-1;

void on_canvas_hover_motion(gpointer controller, double x, double y, gpointer user_data) {
    (void)y;
    (void)user_data;
    shared_hover_x = x;
    shared_is_hovering = true;
    gtk_widget_queue_draw(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller)));
}

void on_canvas_hover_enter(gpointer controller, double x, double y, gpointer user_data) {
    (void)y;
    (void)user_data;
    shared_hover_x = x;
    shared_is_hovering = true;
    gtk_widget_queue_draw(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller)));
}

void on_canvas_hover_leave(gpointer controller, gpointer user_data) {
    (void)user_data;
    shared_hover_x = -1.0;
    shared_is_hovering = false;
    shared_snap_frame = (size_t)-1;
    gtk_widget_queue_draw(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller)));
}

// Add forward declarations so the compiler knows these functions exist regardless of placement
// order
static void draw_envelope_waveform(cairo_t *cr, int width, int height, const float *min_buf,
                                   const float *max_buf, const int *parity_buf, float gain,
                                   double y_center, int track_idx, double alpha, bool start_parity);
static void draw_sample_waveform(cairo_t *cr, int width, int height, const float *audio_buf,
                                 const uint8_t *parity_map, size_t total_frames, double scroll_x,
                                 double frames_per_pixel, float gain, double y_center,
                                 int channel_offset, int track_idx, double alpha,
                                 bool start_parity);
static void draw_playhead(cairo_t *cr, int width, int height, size_t current_pos,
                          size_t total_frames);
void draw_waveform_grid(cairo_t *cr, int width, int height, size_t total_frames);

static void set_track_color_shaded(cairo_t *cr, int track_idx, double alpha, bool shaded) {
    double r = 0.8, g = 0.8, b = 0.8;
    switch (track_idx) {
        case 0:
            r = 0.0;
            g = 0.9;
            b = 0.7;
            break; // Base: Vibrant Electric Teal (Replaces Gray)
        case 1:
            r = 0.0;
            g = 0.6;
            b = 1.0;
            break; // Overdub 1: Deep Sky Blue
        case 2:
            r = 1.0;
            g = 0.2;
            b = 0.8;
            break; // Overdub 2: Neon Magenta
        case 3:
            r = 0.5;
            g = 1.0;
            b = 0.1;
            break; // Overdub 3: Lime Green
        case 4:
            r = 1.0;
            g = 0.8;
            b = 0.0;
            break; // Overdub 4: Bright Yellow
        case 5:
            r = 0.7;
            g = 0.3;
            b = 1.0;
            break; // Overdub 5: Electric Purple
        case 6:
            r = 1.0;
            g = 0.5;
            b = 0.0;
            break; // Overdub 6: Neon Orange
        case 7:
            r = 0.0;
            g = 1.0;
            b = 0.5;
            break; // Overdub 7: Spring Green
        case 8:
            r = 0.9;
            g = 0.6;
            b = 0.8;
            break; // Overdub 8: Soft Pink
        case 9:
            r = 1.0;
            g = 0.3;
            b = 0.3;
            break; // Overdub 9: Coral Red
        case 10:
            r = 0.2;
            g = 0.5;
            b = 1.0;
            break; // Overdub 10: Royal Blue
        case 11:
            r = 0.9;
            g = 0.9;
            b = 0.2;
            break; // Overdub 11: Lemon
    }
    if (shaded) {
        r *= 0.7;
        g *= 0.7;
        b *= 0.7; // Darken the block by 50% for maximum visibility
    }
    cairo_set_source_rgba(cr, r, g, b, alpha);
}

/**
 * @brief Assigns a highly visible neon color for each multitrack layer.
 */
static void set_track_color(cairo_t *cr, int track_idx, double alpha) {
    set_track_color_shaded(cr, track_idx, alpha, false);
}

/**
 * @brief Scans backward from target_frame to find the active clip parity color prior to a silent
 * gap.
 * @param p_map Pointer to the track's parity map buffer.
 * @param max_frames Total valid frames in p_map.
 * @param target_frame The starting frame index for the backward scan.
 * @param out_parity Output boolean receiving the resolved shade parity.
 */
static void get_initial_gap_parity(const uint8_t *p_map, size_t max_frames, size_t target_frame,
                                   bool *out_parity) {
    *out_parity = false;

    if (!p_map || max_frames == 0)
        return;

    if (target_frame >= max_frames) {
        target_frame = max_frames - 1;
    }

    for (size_t f = target_frame;; f--) {
        if (p_map[f] != 0) {
            *out_parity = (p_map[f] == 2);
            return;
        }
        if (f == 0)
            break;
    }
}

/**
 * @brief Cairo draw callback to render the layer identification color dot.
 * @param area The drawing area widget.
 * @param cr The Cairo rendering context.
 * @param width The width of the dot area.
 * @param height The height of the dot area.
 * @param user_data The layer index cast to a gpointer.
 * @return void
 */
void on_draw_track_color_dot(GtkDrawingArea *area, cairo_t *cr, int width, int height,
                             gpointer user_data) {
    (void)area;
    int track_idx = GPOINTER_TO_INT(user_data);
    set_track_color(cr, track_idx, 1.0);

    bool has_audio =
        atomic_load_explicit(&master_tracks[track_idx].has_audio, memory_order_acquire);

    double radius = (width < height ? width : height) / 2.0 - 1.0;
    double cx = width / 2.0;
    double cy = height / 2.0;

    if (has_audio) {
        cairo_arc(cr, cx, cy, radius, 0, 2 * M_PI);
        cairo_fill(cr);
    } else {
        // Draw an upward pointing triangle for empty allocated tracks
        cairo_move_to(cr, cx, cy - radius);
        cairo_line_to(cr, cx + radius, cy + radius);
        cairo_line_to(cr, cx - radius, cy + radius);
        cairo_close_path(cr);
        cairo_fill(cr);
    }
}

/**
 * @brief Converts a linear amplitude value into a normalized VU meter scale value (0.0 to 1.0).
 * @param amp Linear amplitude.
 * @return Normalized float value.
 */
static float amp_to_vu(float amp) {
    if (amp <= 0.001f)
        return 0.0f;
    float db = 20.0f * log10f(amp);
    if (db < -60.0f)
        return 0.0f;
    float vu = (db + 60.0f) / 60.0f;
    return (vu > 1.0f) ? 1.0f : vu;
}

/**
 * @brief Scans a block of frames to find the absolute minimum and maximum amplitude values for UI
 * rendering.
 * @note Strict sequential scanning is enforced to ensure high-frequency transients (like metronomes
 * or snare hits) are perfectly captured without aliasing or dropping spikes.
 */
static inline void scan_waveform_extents(const float *src_buf, const uint8_t *parity_map,
                                         size_t start_frame, size_t end_frame, float *min_l,
                                         float *max_l, float *min_r, float *max_r,
                                         int *parity_flips) {
    float lmin = 0.0f, lmax = 0.0f, rmin = 0.0f, rmax = 0.0f;
    int flips = 0;

    if (start_frame < end_frame) {
        lmin = src_buf[start_frame * 2];
        lmax = lmin;
        rmin = src_buf[start_frame * 2 + 1];
        rmax = rmin;
    }

    if (parity_map) {
        uint8_t prev = parity_map[start_frame];
        for (size_t f = start_frame + 1; f < end_frame; f++) {
            if (parity_map[f] != prev && parity_map[f] != 0) {
                flips++;
                prev = parity_map[f];
            }
        }
    }

    for (size_t f = start_frame; f < end_frame; f++) {
        float vl = src_buf[f * 2];
        float vr = src_buf[f * 2 + 1];
        if (vl < lmin)
            lmin = vl;
        if (vl > lmax)
            lmax = vl;
        if (vr < rmin)
            rmin = vr;
        if (vr > rmax)
            rmax = vr;
    }

    *min_l = lmin;
    *max_l = lmax;
    *min_r = rmin;
    *max_r = rmax;
    *parity_flips = flips;
}

/**
 * @brief Draws the background timeline grid and timestamp labels for the waveform area.
 * Dynamically scales the tick intervals based on the visible viewport duration rather than the
 * total track length, ensuring continuous granularity at deep zoom levels. Uses a DAW-standard
 * time-aware threshold ladder for absolute time.
 * @param cr The Cairo rendering context.
 * @param width The width of the drawing area.
 * @param height The height of the drawing area.
 * @param total_frames The total length of the loaded track for calculating bounds.
 */
void draw_waveform_grid(cairo_t *cr, int width, int height, size_t total_frames) {
    int sr = atomic_load_explicit(&get_transport_state()->active_sample_rate, memory_order_acquire);
    double sample_rate = (sr > 0) ? (double)sr : 48000.0;

    cairo_set_source_rgba(cr, 0.14, 0.14, 0.16, 1.0);
    cairo_paint(cr);

    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.4);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, 0, height / 2.0);
    cairo_line_to(cr, width, height / 2.0);
    cairo_stroke(cr);

    double total_seconds = (total_frames > 0)
                               ? ((double)total_frames / sample_rate)
                               : ((double)(ui_state.config.default_canvas_limit_min > 0
                                               ? ui_state.config.default_canvas_limit_min
                                               : 5) *
                                  60.0);

    if (total_seconds <= 0.0)
        return;

    double scroll_x = waveform_adj ? gtk_adjustment_get_value(waveform_adj) : 0.0;

    double virtual_width = width * zoom_multiplier;
    double visible_seconds = total_seconds / zoom_multiplier;

    double target_pixels_between_labels = 150.0;
    double target_text_step = visible_seconds * (target_pixels_between_labels / (double)width);

    double exponent = floor(log10(target_text_step));
    double fraction = target_text_step / pow(10.0, exponent);

    double nice_fraction;
    if (fraction < 1.5)
        nice_fraction = 1.0;
    else if (fraction < 3.5)
        nice_fraction = 2.0;
    else if (fraction < 7.5)
        nice_fraction = 5.0;
    else
        nice_fraction = 10.0;

    double text_step = nice_fraction * pow(10.0, exponent);

    double min_step = 1.0 / sample_rate;
    if (text_step < min_step)
        text_step = min_step;

    double tick_step = text_step / 4.0;

    double start_sec = (scroll_x / virtual_width) * total_seconds;
    double end_sec = start_sec + visible_seconds;

    double current_tick_sec = floor(start_sec / tick_step) * tick_step;
    double current_text_sec = floor(start_sec / text_step) * text_step;

    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.08);
    cairo_set_line_width(cr, 1.0);
    cairo_new_path(cr);

    while (current_tick_sec <= end_sec) {
        double absolute_x = (current_tick_sec / total_seconds) * virtual_width;
        double x = absolute_x - scroll_x;

        if (x >= 0 && x <= width) {
            cairo_move_to(cr, x, 0);
            cairo_line_to(cr, x, height);
        }
        current_tick_sec += tick_step;
    }
    cairo_stroke(cr);

    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_from_string("Sans 9");
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);

    while (current_text_sec <= end_sec) {
        double absolute_x = (current_text_sec / total_seconds) * virtual_width;
        double x = absolute_x - scroll_x;

        if (x >= 0 && x <= width) {
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.35);
            cairo_move_to(cr, x, 0);
            cairo_line_to(cr, x, height);
            cairo_stroke(cr);

            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.8);
            char time_str[64];

            if (text_step >= 1.0) {
                int mins = (int)current_text_sec / 60;
                int secs = (int)current_text_sec % 60;
                snprintf(time_str, sizeof(time_str), "%d:%02d", mins, secs);
            } else if (text_step >= 0.1) {
                snprintf(time_str, sizeof(time_str), "%.1fs", current_text_sec);
            } else if (text_step >= 0.01) {
                snprintf(time_str, sizeof(time_str), "%.2fs", current_text_sec);
            } else if (text_step >= 0.001) {
                snprintf(time_str, sizeof(time_str), "%.3fs", current_text_sec);
            } else {
                snprintf(time_str, sizeof(time_str), "%.5fs", current_text_sec);
            }

            pango_layout_set_text(layout, time_str, -1);
            cairo_move_to(cr, x + 4, 4);
            pango_cairo_show_layout(cr, layout);
        }
        current_text_sec += text_step;
    }

    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.5);
    pango_layout_set_text(layout, "L", -1);
    cairo_move_to(cr, 5, 20);
    pango_cairo_show_layout(cr, layout);

    pango_layout_set_text(layout, "R", -1);
    cairo_move_to(cr, 5, (height / 2.0) + 6);
    pango_cairo_show_layout(cr, layout);

    g_object_unref(layout);
}

/**
 * @brief Applies the standard multi-color stop linear gradient to a VU meter cairo context.
 * @param cr The Cairo rendering context.
 * @param width The total width of the widget.
 */
static void apply_vu_gradient(cairo_t *cr, int width) {
    cairo_pattern_t *pat = cairo_pattern_create_linear(0.0, 0.0, width, 0.0);
    cairo_pattern_add_color_stop_rgb(pat, 0.0, 0.18, 0.80, 0.44);
    cairo_pattern_add_color_stop_rgb(pat, 0.899, 0.18, 0.80, 0.44);
    cairo_pattern_add_color_stop_rgb(pat, 0.90, 0.94, 0.76, 0.05);
    cairo_pattern_add_color_stop_rgb(pat, 0.979, 0.94, 0.76, 0.05);
    cairo_pattern_add_color_stop_rgb(pat, 0.98, 0.90, 0.29, 0.23);
    cairo_pattern_add_color_stop_rgb(pat, 1.0, 0.90, 0.29, 0.23);
    cairo_set_source(cr, pat);
    cairo_pattern_destroy(pat);
}

/**
 * @brief ENVELOPE MODE: Draws the minimum/maximum amplitude bounds for zoomed-out waveform views.
 */
static void draw_envelope_waveform(cairo_t *cr, int width, int height, const float *min_buf,
                                   const float *max_buf, const int *parity_buf, float gain,
                                   double y_center, int track_idx, double alpha,
                                   bool start_parity) {
    int max_x = (width < MAX_UI_WIDTH) ? width : MAX_UI_WIDTH;
    double scale_h = (height / 4.0) * 0.9;

    cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);

    bool current_parity = start_parity;
    set_track_color_shaded(cr, track_idx, alpha, current_parity);
    cairo_new_path(cr);

    for (int x = 0; x < max_x; x++) {
        if (parity_buf[x] % 2 != 0) {
            cairo_stroke(cr);
            current_parity = !current_parity;
            set_track_color_shaded(cr, track_idx, alpha, current_parity);
            cairo_new_path(cr);
        }

        float min_val = min_buf[x] * gain;
        float max_val = max_buf[x] * gain;

        double y_top = y_center - (max_val * scale_h);
        double y_bot = y_center - (min_val * scale_h);

        cairo_move_to(cr, x, y_top);

        if (y_top == y_bot) {
            cairo_line_to(cr, x + 1, y_bot);
        } else {
            cairo_line_to(cr, x, y_bot);
        }
    }

    cairo_stroke(cr);
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_DEFAULT);
}

/**
 * @brief OSCILLOSCOPE MODE: Draws precise sample interpolation and lollipop markers for deep zooms.
 */
static void draw_sample_waveform(cairo_t *cr, int width, int height, const float *audio_buf,
                                 const uint8_t *parity_map, size_t total_frames, double scroll_x,
                                 double frames_per_pixel, float gain, double y_center,
                                 int channel_offset, int track_idx, double alpha,
                                 bool start_parity) {
    double scale_h = (height / 4.0) * 0.9;
    double pixels_per_frame = 1.0 / frames_per_pixel;

    size_t start_frame = (size_t)(scroll_x * frames_per_pixel);
    size_t end_frame = (size_t)((scroll_x + width) * frames_per_pixel) + 2;

    if (start_frame >= total_frames)
        return;
    if (end_frame > total_frames)
        end_frame = total_frames;

    cairo_set_antialias(cr, CAIRO_ANTIALIAS_DEFAULT);

    bool current_parity = start_parity;
    uint8_t prev_parity_val = parity_map ? parity_map[start_frame] : 0;

    set_track_color_shaded(cr, track_idx, alpha, current_parity);
    cairo_new_path(cr);
    cairo_move_to(cr, (start_frame / frames_per_pixel) - scroll_x,
                  y_center - (audio_buf[start_frame * 2 + channel_offset] * gain * scale_h));

    for (size_t i = start_frame + 1; i < end_frame; i++) {
        if (parity_map && parity_map[i] != prev_parity_val && parity_map[i] != 0) {
            cairo_stroke(cr);
            current_parity = !current_parity;
            set_track_color_shaded(cr, track_idx, alpha, current_parity);
            cairo_new_path(cr);
            cairo_move_to(cr, ((i - 1) / frames_per_pixel) - scroll_x,
                          y_center - (audio_buf[(i - 1) * 2 + channel_offset] * gain * scale_h));
            prev_parity_val = parity_map[i];
        }

        cairo_line_to(cr, (i / frames_per_pixel) - scroll_x,
                      y_center - (audio_buf[i * 2 + channel_offset] * gain * scale_h));
    }
    cairo_stroke(cr);

    if (pixels_per_frame > 8.0) {
        cairo_set_line_width(cr, 0.5);
        current_parity = start_parity;
        prev_parity_val = parity_map ? parity_map[start_frame] : 0;

        for (size_t i = start_frame; i < end_frame; i++) {
            if (parity_map && parity_map[i] != prev_parity_val && parity_map[i] != 0) {
                current_parity = !current_parity;
                prev_parity_val = parity_map[i];
            }

            set_track_color_shaded(cr, track_idx, alpha, current_parity);
            double x = (i / frames_per_pixel) - scroll_x;
            double y = y_center - (audio_buf[i * 2 + channel_offset] * gain * scale_h);

            cairo_move_to(cr, x, y_center);
            cairo_line_to(cr, x, y);
            cairo_stroke(cr);
        }

        current_parity = start_parity;
        prev_parity_val = parity_map ? parity_map[start_frame] : 0;

        for (size_t i = start_frame; i < end_frame; i++) {
            if (parity_map && parity_map[i] != prev_parity_val && parity_map[i] != 0) {
                current_parity = !current_parity;
                prev_parity_val = parity_map[i];
            }

            set_track_color_shaded(cr, track_idx, alpha, current_parity);
            double x = (i / frames_per_pixel) - scroll_x;
            double y = y_center - (audio_buf[i * 2 + channel_offset] * gain * scale_h);

            cairo_new_path(cr);
            cairo_move_to(cr, x + 2.5, y);
            cairo_arc(cr, x, y, 2.5, 0, 2 * M_PI);
            cairo_fill(cr);
        }
    }
}

/**
 * @brief Draws the playhead indicator over the waveform.
 * @param cr The Cairo context.
 * @param width The width of the area.
 * @param height The height of the area.
 * @param current_pos Current frame position.
 * @param total_frames Total track frames.
 * @return void
 */
static void draw_playhead(cairo_t *cr, int width, int height, size_t current_pos,
                          size_t total_frames) {
    if (current_pos > 0 && total_frames > 0) {
        int bt_width = gtk_widget_get_width(waveform_area_bt);
        double virtual_width = bt_width * zoom_multiplier;
        double scroll_x = gtk_adjustment_get_value(waveform_adj);
        double playhead_x =
            (((double)current_pos / (double)total_frames) * virtual_width) - scroll_x;

        if (playhead_x >= 0 && playhead_x <= width) {
            cairo_set_source_rgb(cr, 1.0, 0.8, 0.0);
            cairo_set_line_width(cr, 2.0);
            cairo_move_to(cr, playhead_x, 0);
            cairo_line_to(cr, playhead_x, height);
            cairo_stroke(cr);
        }
    }
}

typedef struct {
    int width;
    int height;
    double scroll_x;
    double zoom;
    size_t bt_frames_len;
    size_t pristine_frames_len;
    int active_tracks;
    bool track_audible[QJ_MAX_TRACKS];
    float track_gains[QJ_MAX_TRACKS];
    float *track_ptrs[QJ_MAX_TRACKS];
    uint8_t *parity_ptrs[QJ_MAX_TRACKS];
} BTRenderJob;

/**
 * @brief Callback invoked on the GTK main loop when the background waveform surface render finishes.
 *        Always queues a redraw if the cache was invalidated while the render task was in flight.
 */
static void on_bt_surface_ready(GObject *source, GAsyncResult *res, gpointer data) {
    (void)source;
    (void)data;
    cairo_surface_t *new_surf = g_task_propagate_pointer(G_TASK(res), NULL);
    if (new_surf) {
        if (surface_bt)
            cairo_surface_destroy(surface_bt);
        surface_bt = new_surf;
    }
    atomic_store_explicit(&bt_render_active, false, memory_order_release);
    if (waveform_area_bt && (new_surf || cached_bt_width == 0))
        gtk_widget_queue_draw(waveform_area_bt);
}

static void bt_render_thread(GTask *task, gpointer source, gpointer task_data,
                             GCancellable *cancel) {
    (void)source;
    (void)cancel;
    BTRenderJob *job = task_data;

    if (atomic_load_explicit(&engine_is_shutting_down, memory_order_acquire) ||
        atomic_load_explicit(&request_track_free, memory_order_acquire)) {
        atomic_store_explicit(&ui_scanner_is_busy, false, memory_order_release);
        g_task_return_pointer(task, NULL, NULL);
        return;
    }

    cairo_surface_t *surf =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, job->width, job->height);
    cairo_t *cr = cairo_create(surf);

    draw_waveform_grid(cr, job->width, job->height, job->bt_frames_len);

    double virtual_width = job->width * job->zoom;
    double raw_frames_per_pixel = (double)job->pristine_frames_len / virtual_width;
    if (raw_frames_per_pixel == 0)
        raw_frames_per_pixel = 1.0;

    for (int l = 0; l < job->active_tracks; l++) {
        float *active_buf = job->track_ptrs[l];
        uint8_t *parity_buf_ptr = job->parity_ptrs[l];
        if (!active_buf || !job->track_audible[l])
            continue;

        bool current_parity = false;
        size_t visible_start_frame = (size_t)(job->scroll_x * raw_frames_per_pixel);
        if (visible_start_frame > job->pristine_frames_len)
            visible_start_frame = job->pristine_frames_len;

        get_initial_gap_parity(parity_buf_ptr, job->pristine_frames_len, visible_start_frame,
                               &current_parity);

        if (raw_frames_per_pixel >= 1.0) {
            float min_l[MAX_UI_WIDTH] = {0}, max_l[MAX_UI_WIDTH] = {0};
            float min_r[MAX_UI_WIDTH] = {0}, max_r[MAX_UI_WIDTH] = {0};
            int parity_buf[MAX_UI_WIDTH] = {0};

            for (int x = 0; x < job->width && x < MAX_UI_WIDTH; x++) {
                if (atomic_load_explicit(&request_track_free, memory_order_relaxed)) {
                    atomic_store_explicit(&ui_scanner_is_busy, false, memory_order_release);
                    cairo_destroy(cr);
                    cairo_surface_destroy(surf);
                    g_task_return_pointer(task, NULL, NULL);
                    return;
                }

                size_t start_frame = (size_t)((job->scroll_x + x) * raw_frames_per_pixel);
                size_t end_frame = (size_t)((job->scroll_x + x + 1) * raw_frames_per_pixel);

                if (end_frame <= start_frame)
                    end_frame = start_frame + 1;
                if (start_frame >= job->pristine_frames_len)
                    continue;
                if (end_frame > job->pristine_frames_len)
                    end_frame = job->pristine_frames_len;

                scan_waveform_extents(active_buf, parity_buf_ptr, start_frame, end_frame, &min_l[x],
                                      &max_l[x], &min_r[x], &max_r[x], &parity_buf[x]);
            }

            cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
            cairo_set_line_width(cr, 1.0);
            draw_envelope_waveform(cr, job->width, job->height, min_l, max_l, parity_buf,
                                   job->track_gains[l], job->height / 4.0, l, 0.65, current_parity);
            draw_envelope_waveform(cr, job->width, job->height, min_r, max_r, parity_buf,
                                   job->track_gains[l], 3.0 * job->height / 4.0, l, 0.65,
                                   current_parity);
        } else {
            if (atomic_load_explicit(&request_track_free, memory_order_relaxed)) {
                atomic_store_explicit(&ui_scanner_is_busy, false, memory_order_release);
                cairo_destroy(cr);
                cairo_surface_destroy(surf);
                g_task_return_pointer(task, NULL, NULL);
                return;
            }
            cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
            cairo_set_line_width(cr, 1.5);
            draw_sample_waveform(cr, job->width, job->height, active_buf, parity_buf_ptr,
                                 job->pristine_frames_len, job->scroll_x, raw_frames_per_pixel,
                                 job->track_gains[l], job->height / 4.0, 0, l, 0.65,
                                 current_parity);
            draw_sample_waveform(cr, job->width, job->height, active_buf, parity_buf_ptr,
                                 job->pristine_frames_len, job->scroll_x, raw_frames_per_pixel,
                                 job->track_gains[l], 3.0 * job->height / 4.0, 1, l, 0.65,
                                 current_parity);
        }
    }

    atomic_store_explicit(&ui_scanner_is_busy, false, memory_order_release);
    cairo_destroy(cr);
    g_task_return_pointer(task, surf, (GDestroyNotify)cairo_surface_destroy);
}

/**
 * @brief Renders the backing track and multi-track layers onto the top waveform canvas.
 *        Uses an asynchronous background Cairo surface cache for inactive tracks and
 *        a read-locked live overlay for the active recording track and snap cursor.
 */
void on_draw_bt_waveform(GtkDrawingArea *area, cairo_t *cr, int width, int height,
                         gpointer user_data) {
    (void)area;
    (void)user_data;
    size_t bt_frames_len =
        atomic_load_explicit(&get_transport_state()->backing_track_frames, memory_order_acquire);
    if (atomic_load_explicit(&pristine_frames, memory_order_acquire) == 0 || bt_frames_len == 0) {
        draw_waveform_grid(cr, width, height, 0);
        return;
    }
    int bt_width = gtk_widget_get_width(waveform_area_bt);
    double virtual_width = bt_width * zoom_multiplier;
    double scroll_x = gtk_adjustment_get_value(waveform_adj);
    size_t current_pos =
        atomic_load_explicit(&get_transport_state()->playback_pos, memory_order_acquire);
    int active_tracks = atomic_load_explicit(&active_track_count, memory_order_relaxed);
    int rec_track = atomic_load_explicit(&current_recording_track, memory_order_relaxed);
    static double cached_bt_scroll_x = -1.0;
    static double cached_bt_zoom = -1.0;
    static float *cached_track_ptrs[QJ_MAX_TRACKS] = {NULL};
    static bool cached_audible[QJ_MAX_TRACKS] = {false};
    bool any_solo_active = false;
    bool track_audible[QJ_MAX_TRACKS] = {false};
    for (int l = 0; l < QJ_MAX_TRACKS; l++) {
        if (atomic_load_explicit(&master_tracks[l].is_soloed, memory_order_relaxed)) {
            any_solo_active = true;
            break;
        }
    }
    bool pointers_changed = false;
    for (int i = 0; i < QJ_MAX_TRACKS; i++) {
        track_audible[i] = (!any_solo_active || atomic_load_explicit(&master_tracks[i].is_soloed,
                                                                     memory_order_relaxed)) &&
                           atomic_load_explicit(&master_tracks[i].has_audio, memory_order_acquire);
        if (master_tracks[i].active_buffer != cached_track_ptrs[i]) {
            pointers_changed = true;
            cached_track_ptrs[i] = master_tracks[i].active_buffer;
        }
        if (track_audible[i] != cached_audible[i]) {
            pointers_changed = true;
            cached_audible[i] = track_audible[i];
        }
    }
    if (width != cached_bt_width || height != cached_bt_height ||
        active_tracks != cached_active_tracks || scroll_x != cached_bt_scroll_x ||
        zoom_multiplier != cached_bt_zoom || bt_frames_len != cached_bt_frames ||
        pointers_changed) {
        if (!atomic_load_explicit(&request_track_free, memory_order_acquire) &&
            !atomic_exchange_explicit(&bt_render_active, true, memory_order_acquire)) {
            atomic_store_explicit(&ui_scanner_is_busy, true, memory_order_seq_cst);
            if (atomic_load_explicit(&request_track_free, memory_order_seq_cst) ||
                pristine_frames == 0) {
                atomic_store_explicit(&ui_scanner_is_busy, false, memory_order_release);
                atomic_store_explicit(&bt_render_active, false, memory_order_release);
            } else {
                BTRenderJob *job = g_new0(BTRenderJob, 1);
                job->width = width;
                job->height = height;
                job->scroll_x = scroll_x;
                job->zoom = zoom_multiplier;
                job->bt_frames_len = bt_frames_len;
                job->pristine_frames_len = pristine_frames;
                job->active_tracks =
                    (active_tracks > QJ_MAX_TRACKS) ? QJ_MAX_TRACKS : active_tracks;
                for (int l = 0; l < job->active_tracks; l++) {
                    job->track_ptrs[l] = master_tracks[l].active_buffer;
                    job->parity_ptrs[l] = master_tracks[l].parity_map;
                    job->track_audible[l] = track_audible[l];
                    job->track_gains[l] =
                        atomic_load_explicit(&master_tracks[l].gain, memory_order_relaxed);
                }
                GTask *task = g_task_new(NULL, NULL, on_bt_surface_ready, NULL);
                g_task_set_task_data(task, job, g_free);
                g_task_run_in_thread(task, bt_render_thread);
                g_object_unref(task);
                cached_bt_width = width;
                cached_bt_height = height;
                cached_active_tracks = active_tracks;
                cached_bt_scroll_x = scroll_x;
                cached_bt_zoom = zoom_multiplier;
                cached_bt_frames = bt_frames_len;
            }
        }
    }
    if (surface_bt) {
        cairo_set_source_surface(cr, surface_bt, 0, 0);
        cairo_paint(cr);
    }
    if (active_tracks >= 1 &&
        atomic_load_explicit(&get_transport_state()->is_recording, memory_order_relaxed) &&
        rec_track >= 0 && rec_track < QJ_MAX_TRACKS && try_acquire_track_read()) {
        float *rec_buf = master_tracks[rec_track].active_buffer;
        size_t alloc_f = master_tracks[rec_track].allocated_frames;
        double raw_frames_per_pixel = (double)pristine_frames / virtual_width;
        if (raw_frames_per_pixel == 0)
            raw_frames_per_pixel = 1.0;
        float current_speed =
            atomic_load_explicit(&get_transport_state()->playback_speed, memory_order_relaxed);
        size_t raw_current_pos = (size_t)(current_pos * current_speed);
        if (raw_current_pos > alloc_f)
            raw_current_pos = alloc_f;
        bool current_parity = false;
        size_t visible_start_frame = (size_t)(scroll_x * raw_frames_per_pixel);
        if (visible_start_frame > raw_current_pos)
            visible_start_frame = raw_current_pos;
        get_initial_gap_parity(master_tracks[rec_track].parity_map, alloc_f, visible_start_frame,
                               &current_parity);
        if (raw_frames_per_pixel >= 1.0 && rec_buf) {
            memset(sweep_min_l, 0, sizeof(sweep_min_l));
            memset(sweep_max_l, 0, sizeof(sweep_max_l));
            memset(sweep_min_r, 0, sizeof(sweep_min_r));
            memset(sweep_max_r, 0, sizeof(sweep_max_r));
            memset(sweep_parity, 0, sizeof(sweep_parity));
            for (int x = 0; x < width && x < MAX_UI_WIDTH; x++) {
                size_t start_frame = (size_t)((scroll_x + x) * raw_frames_per_pixel);
                size_t end_frame = (size_t)((scroll_x + x + 1) * raw_frames_per_pixel);
                if (start_frame >= raw_current_pos)
                    break;
                if (end_frame > raw_current_pos)
                    end_frame = raw_current_pos;
                scan_waveform_extents(rec_buf, master_tracks[rec_track].parity_map, start_frame,
                                      end_frame, &sweep_min_l[x], &sweep_max_l[x], &sweep_min_r[x],
                                      &sweep_max_r[x], &sweep_parity[x]);
            }
            cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
            cairo_set_line_width(cr, 1.0);
            draw_envelope_waveform(cr, width, height, sweep_min_l, sweep_max_l, sweep_parity, 1.0f,
                                   height / 4.0, rec_track, 0.5, current_parity);
            draw_envelope_waveform(cr, width, height, sweep_min_r, sweep_max_r, sweep_parity, 1.0f,
                                   3.0 * height / 4.0, rec_track, 0.5, current_parity);
        } else if (rec_buf) {
            cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
            cairo_set_line_width(cr, 1.5);
            draw_sample_waveform(cr, width, height, rec_buf, master_tracks[rec_track].parity_map,
                                 raw_current_pos, scroll_x, raw_frames_per_pixel, 1.0f,
                                 height / 4.0, 0, rec_track, 0.5, current_parity);
            draw_sample_waveform(cr, width, height, rec_buf, master_tracks[rec_track].parity_map,
                                 raw_current_pos, scroll_x, raw_frames_per_pixel, 1.0f,
                                 3.0 * height / 4.0, 1, rec_track, 0.5, current_parity);
        }
        release_track_read();
    }
    if (atomic_load_explicit(&loop_active, memory_order_acquire)) {
        size_t l_start = atomic_load_explicit(&loop_start_frame, memory_order_relaxed);
        size_t l_end = atomic_load_explicit(&loop_end_frame, memory_order_relaxed);
        double start_px = (((double)l_start / (double)bt_frames_len) * virtual_width) - scroll_x;
        double end_px = (((double)l_end / (double)bt_frames_len) * virtual_width) - scroll_x;
        cairo_set_source_rgba(cr, 0.3, 0.7, 1.0, 0.25);
        cairo_rectangle(cr, round(start_px), 0, round(end_px) - round(start_px), height);
        cairo_fill(cr);
    } else if (is_dragging && drag_start_fraction >= 0.0) {
        double start_px = (drag_start_fraction * virtual_width) - scroll_x;
        double end_px = (current_drag_fraction * virtual_width) - scroll_x;
        if (start_px > end_px) {
            double tmp = start_px;
            start_px = end_px;
            end_px = tmp;
        }
        cairo_set_source_rgba(cr, 0.3, 0.7, 1.0, 0.25);
        cairo_rectangle(cr, round(start_px), 0, round(end_px) - round(start_px), height);
        cairo_fill(cr);
    }
    if (video_end_frame > video_start_frame) {
        if (bt_frames_len > 0) {
            cairo_set_source_rgba(cr, 0.85, 0.1, 0.1, 0.9);
            cairo_set_line_width(cr, 2.0);
            double start_px =
                (((double)video_start_frame / (double)bt_frames_len) * virtual_width) - scroll_x;
            double end_px =
                (((double)video_end_frame / (double)bt_frames_len) * virtual_width) - scroll_x;
            cairo_move_to(cr, start_px, height / 2.0);
            cairo_line_to(cr, end_px, height / 2.0);
            cairo_stroke(cr);
            cairo_arc(cr, start_px < 0 ? 0 : start_px, height / 2.0, 4.0, 0, 2 * M_PI);
            cairo_fill(cr);
            cairo_arc(cr, end_px > width ? width : end_px, height / 2.0, 4.0, 0, 2 * M_PI);
            cairo_fill(cr);
        }
    }
    bool is_recording =
        atomic_load_explicit(&get_transport_state()->is_recording, memory_order_acquire);
    bool is_video_armed = atomic_load_explicit(&video_is_armed, memory_order_acquire);
    if (is_recording && is_video_armed) {
        if (bt_frames_len > 0 && current_pos > live_video_start_frame) {
            cairo_set_source_rgba(cr, 1.0, 0.4, 0.4, 0.9);
            cairo_set_line_width(cr, 2.0);
            double start_px =
                (((double)live_video_start_frame / (double)bt_frames_len) * virtual_width) -
                scroll_x;
            double end_px =
                (((double)current_pos / (double)bt_frames_len) * virtual_width) - scroll_x;
            cairo_move_to(cr, start_px, (height / 2.0) - 8);
            cairo_line_to(cr, end_px, (height / 2.0) - 8);
            cairo_stroke(cr);
            cairo_arc(cr, start_px < 0 ? 0 : start_px, (height / 2.0) - 8, 4.0, 0, 2 * M_PI);
            cairo_fill(cr);
            cairo_arc(cr, end_px > width ? width : end_px, (height / 2.0) - 8, 4.0, 0, 2 * M_PI);
            cairo_fill(cr);
        }
    }
    draw_playhead(cr, width, height, current_pos, bt_frames_len);

    float hover_peak_amp = 0.0f;
    bool peak_valid = false;
    bool is_zoomed_out = false;
    uint32_t active_mask = 1;

    if (shared_is_hovering && shared_hover_x >= 0.0 && bt_frames_len > 0 &&
        try_acquire_track_read()) {
        size_t local_pristine = atomic_load_explicit(&pristine_frames, memory_order_acquire);
        double raw_frames_per_pixel = (double)local_pristine / virtual_width;
        size_t hover_frame = (size_t)((shared_hover_x + scroll_x) * raw_frames_per_pixel);
        size_t best_snap = (size_t)-1;
        size_t best_dist = (size_t)(15.0 * raw_frames_per_pixel) + 1;
        size_t radius_frames = best_dist - 1;
        size_t start_scan = (hover_frame > radius_frames) ? hover_frame - radius_frames : 0;
        size_t end_scan = hover_frame + radius_frames;
        if (end_scan > local_pristine)
            end_scan = local_pristine;
        bool any_solo = false;
        for (int t = 0; t < QJ_MAX_TRACKS; t++) {
            if (atomic_load_explicit(&master_tracks[t].is_soloed, memory_order_relaxed)) {
                any_solo = true;
                break;
            }
        }
        bool is_in_gap = true;
        for (int t = 0; t < QJ_MAX_TRACKS; t++) {
            if (!master_tracks[t].active_buffer ||
                !atomic_load_explicit(&master_tracks[t].has_audio, memory_order_acquire))
                continue;
            if (any_solo &&
                !atomic_load_explicit(&master_tracks[t].is_soloed, memory_order_relaxed))
                continue;
            uint8_t *p_map = master_tracks[t].parity_map;
            size_t alloc_f = master_tracks[t].allocated_frames;
            if (!p_map || alloc_f == 0)
                continue;
            size_t track_end_scan = (end_scan <= alloc_f) ? end_scan : alloc_f;
            if (hover_frame < alloc_f && p_map[hover_frame] != 0) {
                is_in_gap = false;
            }
            uint8_t prev_parity =
                (start_scan > 0 && start_scan <= alloc_f) ? p_map[start_scan - 1] : 0;
            for (size_t f = start_scan; f < track_end_scan; f++) {
                uint8_t current_parity = p_map[f];
                if (current_parity != prev_parity) {
                    size_t dist = (f > hover_frame) ? f - hover_frame : hover_frame - f;
                    if (dist < best_dist) {
                        best_dist = dist;
                        best_snap = f;
                    }
                    prev_parity = current_parity;
                }
            }
            if (track_end_scan == local_pristine && local_pristine > 0) {
                if (prev_parity != 0) {
                    size_t dist = (local_pristine > hover_frame) ? local_pristine - hover_frame
                                                                 : hover_frame - local_pristine;
                    if (dist < best_dist) {
                        best_dist = dist;
                        best_snap = local_pristine;
                    }
                }
            }
        }

        /**
         * @brief Calculate instantaneous peak amplitude under the cursor for the focused track.
         * Bypassed if zoom level is too far out to prevent UI thread freezing.
         */
        active_mask = atomic_load_explicit(&selected_tracks_mask, memory_order_acquire);

        if (raw_frames_per_pixel <= 1024.0) {
            size_t scan_start = hover_frame;
            size_t scan_end = hover_frame + (size_t)raw_frames_per_pixel;
            if (scan_end <= scan_start)
                scan_end = scan_start + 1;

            float mix_max = 0.0f;

            for (size_t f = scan_start; f < scan_end; f++) {
                float sum_l = 0.0f;
                float sum_r = 0.0f;

                for (int t = 0; t < QJ_MAX_TRACKS; t++) {
                    if (!(active_mask & (1 << t)))
                        continue;
                    if (!master_tracks[t].active_buffer)
                        continue;
                    if (f < master_tracks[t].allocated_frames) {
                        float gain =
                            atomic_load_explicit(&master_tracks[t].gain, memory_order_relaxed);
                        sum_l += master_tracks[t].active_buffer[f * 2] * gain;
                        sum_r += master_tracks[t].active_buffer[f * 2 + 1] * gain;
                    }
                }

                float abs_l = fabsf(sum_l);
                float abs_r = fabsf(sum_r);
                if (abs_l > mix_max)
                    mix_max = abs_l;
                if (abs_r > mix_max)
                    mix_max = abs_r;
            }

            hover_peak_amp = mix_max;
            peak_valid = true;
        } else {
            is_zoomed_out = true;
        }

        release_track_read();
        shared_snap_frame = best_snap;
        double draw_x = shared_hover_x;
        if (shared_snap_frame != (size_t)-1) {
            draw_x =
                (((double)shared_snap_frame / (double)local_pristine) * virtual_width) - scroll_x;
            cairo_set_source_rgba(cr, 0.0, 1.0, 1.0, 0.9);
            cairo_set_line_width(cr, 2.0);
        } else if (is_in_gap && hover_frame < local_pristine) {
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.8);
            cairo_set_line_width(cr, 1.5);
        } else {
            cairo_set_source_rgba(cr, 1.0, 0.2, 0.2, 0.8);
            cairo_set_line_width(cr, 1.5);
        }
        cairo_move_to(cr, draw_x, 0);
        cairo_line_to(cr, draw_x, height);
        cairo_stroke(cr);
        int sr =
            atomic_load_explicit(&get_transport_state()->active_sample_rate, memory_order_acquire);
        double sample_rate = (sr > 0) ? (double)sr : 48000.0;
        double total_sec = (double)bt_frames_len / sample_rate;
        double marker_frac = (virtual_width > 0.0) ? ((draw_x + scroll_x) / virtual_width) : 0.0;
        if (shared_snap_frame != (size_t)-1 && local_pristine > 0) {
            marker_frac = (double)shared_snap_frame / (double)local_pristine;
        }
        if (marker_frac < 0.0)
            marker_frac = 0.0;
        if (marker_frac > 1.0)
            marker_frac = 1.0;
        double t_sec = marker_frac * total_sec;
        int t_mins = (int)(t_sec / 60.0);
        double t_rem = fmod(t_sec, 60.0);

        char badge_text[128];
        char db_str[32] = "";

        if (peak_valid) {
            if (hover_peak_amp <= 0.0000158f) {
                snprintf(db_str, sizeof(db_str), " | -inf dB");
            } else {
                snprintf(db_str, sizeof(db_str), " | %+05.1f dB", 20.0f * log10f(hover_peak_amp));
            }
        } else if (is_zoomed_out) {
            snprintf(db_str, sizeof(db_str), " | [Zoom In]");
        }

        if (is_dragging && drag_start_fraction >= 0.0) {
            double d_sec = fabs(current_drag_fraction - drag_start_fraction) * total_sec;
            int d_mins = (int)(d_sec / 60.0);
            double d_rem = fmod(d_sec, 60.0);
            snprintf(badge_text, sizeof(badge_text), "t+%02d:%04.1f (\xCE\x94 %02d:%04.1f)%s",
                     t_mins, t_rem, d_mins, d_rem, db_str);
        } else {
            snprintf(badge_text, sizeof(badge_text), "t+%02d:%04.1f%s", t_mins, t_rem, db_str);
        }

        PangoLayout *layout = pango_cairo_create_layout(cr);
        PangoFontDescription *desc = pango_font_description_from_string("Sans Bold 8.5");
        pango_layout_set_font_description(layout, desc);
        pango_font_description_free(desc);
        pango_layout_set_text(layout, badge_text, -1);
        int text_w = 0, text_h = 0;
        pango_layout_get_pixel_size(layout, &text_w, &text_h);
        double pad_x = 6.0;
        double pad_y = 3.0;
        double box_w = text_w + (pad_x * 2.0);
        double box_h = text_h + (pad_y * 2.0);
        double box_x = draw_x - box_w - 8.0;
        if (box_x < 4.0) {
            box_x = draw_x + 8.0;
        }
        double box_y = 20.0;

        // Color the background box based on selection state
        if (__builtin_popcount(active_mask) == 1) {
            int rec_track = atomic_load_explicit(&current_recording_track, memory_order_relaxed);
            // Use true for 'shaded' to ensure white text stays readable against bright neon tracks
            set_track_color_shaded(cr, rec_track, 0.65, true);
        } else {
            cairo_set_source_rgba(cr, 0.08, 0.08, 0.10, 0.65);
        }

        cairo_rectangle(cr, box_x, box_y, box_w, box_h);
        cairo_fill(cr);
        if (shared_snap_frame != (size_t)-1) {
            cairo_set_source_rgba(cr, 0.4, 1.0, 1.0, 0.95);
        } else if (is_in_gap && hover_frame < local_pristine) {
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.90);
        } else {
            cairo_set_source_rgba(cr, 1.0, 0.6, 0.6, 0.95);
        }
        cairo_move_to(cr, box_x + pad_x, box_y + pad_y);
        pango_cairo_show_layout(cr, layout);
        g_object_unref(layout);
    }
}

/**
 * @brief Renders the live recording input waveform onto the bottom waveform canvas.
 *        Acquires a shared read lock on master_tracks to prevent UAF during canvas resize.
 */
void on_draw_input_waveform(GtkDrawingArea *area, cairo_t *cr, int width, int height,
                            gpointer user_data) {
    (void)area;
    (void)user_data;
    bool is_recording =
        atomic_load_explicit(&get_transport_state()->is_recording, memory_order_acquire);
    int rec_track = atomic_load_explicit(&current_recording_track, memory_order_relaxed);
    if (rec_track < 0 || rec_track >= QJ_MAX_TRACKS)
        rec_track = 0;
    float *active_input_buf = master_tracks[rec_track].active_buffer;
    size_t bt_frames =
        atomic_load_explicit(&get_transport_state()->backing_track_frames, memory_order_acquire);
    if (!active_input_buf || bt_frames == 0) {
        draw_waveform_grid(cr, width, height, 0);
        return;
    }
    int bt_width = gtk_widget_get_width(waveform_area_bt);
    double virtual_width = bt_width * zoom_multiplier;
    double scroll_x = gtk_adjustment_get_value(waveform_adj);
    double raw_frames_per_pixel = (double)pristine_frames / virtual_width;
    if (raw_frames_per_pixel == 0)
        raw_frames_per_pixel = 1.0;
    size_t current_pos =
        atomic_load_explicit(&get_transport_state()->playback_pos, memory_order_acquire);
    float current_speed =
        atomic_load_explicit(&get_transport_state()->playback_speed, memory_order_relaxed);
    size_t raw_current_pos = (size_t)(current_pos * current_speed);
    static double cached_input_scroll_x = -1.0;
    static double cached_input_zoom = -1.0;
    static size_t cached_input_bt_frames = 0;
    if (width != cached_input_width || height != cached_input_height ||
        scroll_x != cached_input_scroll_x || zoom_multiplier != cached_input_zoom ||
        bt_frames != cached_input_bt_frames) {
        if (surface_input_grid) {
            cairo_surface_destroy(surface_input_grid);
            surface_input_grid = NULL;
        }
        surface_input_grid = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
        cairo_t *surface_cr = cairo_create(surface_input_grid);
        draw_waveform_grid(surface_cr, width, height, bt_frames);
        cairo_destroy(surface_cr);
        cached_input_width = width;
        cached_input_height = height;
        cached_input_scroll_x = scroll_x;
        cached_input_zoom = zoom_multiplier;
        cached_input_bt_frames = bt_frames;
    }
    if (surface_input_grid) {
        cairo_set_source_surface(cr, surface_input_grid, 0, 0);
        cairo_paint(cr);
    }
    if (is_recording && try_acquire_track_read()) {
        active_input_buf = master_tracks[rec_track].active_buffer;
        size_t alloc_f = master_tracks[rec_track].allocated_frames;
        if (raw_current_pos > alloc_f)
            raw_current_pos = alloc_f;
        bool current_parity = false;
        size_t visible_start_frame = (size_t)(scroll_x * raw_frames_per_pixel);
        if (visible_start_frame > raw_current_pos)
            visible_start_frame = raw_current_pos;
        get_initial_gap_parity(master_tracks[rec_track].parity_map, alloc_f, visible_start_frame,
                               &current_parity);
        if (raw_frames_per_pixel >= 1.0 && active_input_buf) {
            memset(input_min_l, 0, sizeof(input_min_l));
            memset(input_max_l, 0, sizeof(input_max_l));
            memset(input_min_r, 0, sizeof(input_min_r));
            memset(input_max_r, 0, sizeof(input_max_r));
            memset(input_parity, 0, sizeof(input_parity));
            for (int x = 0; x < width && x < MAX_UI_WIDTH; x++) {
                size_t start_frame = (size_t)((scroll_x + x) * raw_frames_per_pixel);
                size_t end_frame = (size_t)((scroll_x + x + 1) * raw_frames_per_pixel);
                if (end_frame <= start_frame)
                    end_frame = start_frame + 1;
                if (start_frame >= raw_current_pos)
                    break;
                if (end_frame > raw_current_pos)
                    end_frame = raw_current_pos;
                scan_waveform_extents(active_input_buf, master_tracks[rec_track].parity_map,
                                      start_frame, end_frame, &input_min_l[x], &input_max_l[x],
                                      &input_min_r[x], &input_max_r[x], &input_parity[x]);
            }
            cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
            cairo_set_line_width(cr, 1.0);
            draw_envelope_waveform(cr, width, height, input_min_l, input_max_l, input_parity, 1.0f,
                                   height / 4.0, rec_track, 0.5, current_parity);
            draw_envelope_waveform(cr, width, height, input_min_r, input_max_r, input_parity, 1.0f,
                                   3.0 * height / 4.0, rec_track, 0.5, current_parity);
        } else if (active_input_buf) {
            cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
            cairo_set_line_width(cr, 1.5);
            draw_sample_waveform(cr, width, height, active_input_buf,
                                 master_tracks[rec_track].parity_map, raw_current_pos, scroll_x,
                                 raw_frames_per_pixel, 1.0f, height / 4.0, 0, rec_track, 0.5,
                                 current_parity);
            draw_sample_waveform(cr, width, height, active_input_buf,
                                 master_tracks[rec_track].parity_map, raw_current_pos, scroll_x,
                                 raw_frames_per_pixel, 1.0f, 3.0 * height / 4.0, 1, rec_track, 0.5,
                                 current_parity);
        }
        release_track_read();
    }
    if (atomic_load_explicit(&loop_active, memory_order_acquire)) {
        size_t l_start = atomic_load_explicit(&loop_start_frame, memory_order_relaxed);
        size_t l_end = atomic_load_explicit(&loop_end_frame, memory_order_relaxed);
        double start_px = (((double)l_start / (double)bt_frames) * virtual_width) - scroll_x;
        double end_px = (((double)l_end / (double)bt_frames) * virtual_width) - scroll_x;
        cairo_set_source_rgba(cr, 0.3, 0.7, 1.0, 0.25);
        cairo_rectangle(cr, round(start_px), 0, round(end_px) - round(start_px), height);
        cairo_fill(cr);
    } else if (is_dragging && drag_start_fraction >= 0.0) {
        double start_px = (drag_start_fraction * virtual_width) - scroll_x;
        double end_px = (current_drag_fraction * virtual_width) - scroll_x;
        if (start_px > end_px) {
            double tmp = start_px;
            start_px = end_px;
            end_px = tmp;
        }
        cairo_set_source_rgba(cr, 0.3, 0.7, 1.0, 0.25);
        cairo_rectangle(cr, round(start_px), 0, round(end_px) - round(start_px), height);
        cairo_fill(cr);
    }
    if (atomic_load_explicit(&get_transport_state()->is_recording, memory_order_acquire) &&
        atomic_load_explicit(&video_is_armed, memory_order_acquire)) {
        if (bt_frames > 0 && current_pos > live_video_start_frame) {
            double start_px =
                (((double)live_video_start_frame / (double)bt_frames) * virtual_width) - scroll_x;
            double end_px = (((double)current_pos / (double)bt_frames) * virtual_width) - scroll_x;
            cairo_set_source_rgba(cr, 0.85, 0.1, 0.1, 0.9);
            cairo_set_line_width(cr, 2.0);
            cairo_move_to(cr, start_px, height / 2.0);
            cairo_line_to(cr, end_px, height / 2.0);
            cairo_stroke(cr);
            cairo_arc(cr, start_px, height / 2.0, 4.0, 0, 2 * M_PI);
            cairo_fill(cr);
            cairo_arc(cr, end_px, height / 2.0, 4.0, 0, 2 * M_PI);
            cairo_fill(cr);
        }
    }
    draw_playhead(cr, width, height, current_pos, bt_frames);
}

/**
 * @brief Cairo draw callback for a generic horizontal VU meter.
 * @param area The drawing area widget.
 * @param cr The Cairo rendering context.
 * @param width The width of the meter.
 * @param height The height of the meter.
 * @param user_data Pointer to the atomic float providing peak amplitude data.
 * @return void
 */
void on_draw_vu_meter(GtkDrawingArea *area, cairo_t *cr, int width, int height,
                      gpointer user_data) {
    _Atomic float *peak_ptr = (_Atomic float *)user_data;
    float amp = atomic_load_explicit(peak_ptr, memory_order_relaxed);
    float vu = amp_to_vu(amp);

    int bar_width = width - 35; // Reserve the last 35px strictly for the dB text

    // 1. Draw empty bar background
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0); // 1.0 = white, 0.0 = black
    cairo_rectangle(cr, 0, 0, bar_width, height);
    cairo_fill(cr);

    // 2. Draw active signal gradient
    if (vu > 0.0f) {
        apply_vu_gradient(cr, bar_width);
        cairo_rectangle(cr, 0, 0, bar_width * vu, height);
        cairo_fill(cr);
    }

    // 3. Draw border around the VU bar
    cairo_set_source_rgb(cr, 0.75, 0.75, 0.75);
    cairo_set_line_width(cr, 1.0);
    cairo_rectangle(cr, 0.5, 0.5, bar_width - 1.0, height - 1.0);
    cairo_stroke(cr);

    // 4. Calculate numeric dB value with a 200ms throttle for readability
    gint64 now = g_get_monotonic_time();
    gint64 *last_time_ptr = g_object_get_data(G_OBJECT(area), "last_db_time");
    float *last_db_ptr = g_object_get_data(G_OBJECT(area), "last_db_val");

    // Safety [Low]: Safe initialization of widget-specific tracking data
    if (!last_time_ptr) {
        last_time_ptr = g_new(gint64, 1);
        last_db_ptr = g_new(float, 1);
        *last_time_ptr = 0;
        *last_db_ptr = -96.0f;
        g_object_set_data_full(G_OBJECT(area), "last_db_time", last_time_ptr, g_free);
        g_object_set_data_full(G_OBJECT(area), "last_db_val", last_db_ptr, g_free);
    }

    float current_db = (amp <= 0.0001f) ? -96.0f : 20.0f * log10f(amp);

    // Update text at most 3 times a second (~333ms) UNLESS we clip, which updates instantly
    if (amp >= 0.99f || now - *last_time_ptr > 333333) {
        *last_db_ptr = current_db;
        *last_time_ptr = now;
    }

    char db_str[16];
    if (*last_db_ptr <= -95.0f) {
        snprintf(db_str, sizeof(db_str), "---");
    } else if (*last_db_ptr >= 0.0f) {
        snprintf(db_str, sizeof(db_str), "+%.0f", *last_db_ptr);
    } else {
        snprintf(db_str, sizeof(db_str), "%.0f", *last_db_ptr);
    }

    // 5. Render text via Pango
    PangoLayout *layout = pango_cairo_create_layout(cr);

    // Bump the font size from 8 to 9 points
    PangoFontDescription *desc = pango_font_description_from_string("Sans Bold 9");

    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);
    pango_layout_set_text(layout, db_str, -1);

    // Right-align the text against the absolute edge of the widget
    int text_w, text_h;
    pango_layout_get_pixel_size(layout, &text_w, &text_h);
    int text_x = width - text_w;
    int text_y = (height - text_h) / 2;

    // Color text red if clipping, dark gray otherwise
    if (amp >= 0.99f) {
        cairo_set_source_rgb(cr, 0.8, 0.0, 0.0);
    } else {
        cairo_set_source_rgb(cr, 0.3, 0.3, 0.3);
    }

    cairo_move_to(cr, text_x, text_y);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
}

/**
 * @brief Cairo draw callback to render the layer identification color dot.
 * Intelligently sleeps massive canvas repaints when the engine is idle.
 * Memory state variables are tracked independently per waveform to prevent race conditions
 * and missed redraws when multiple canvases share the same tick rate.
 * @param widget The widget to queue for redraw.
 * @param frame_clock The GDK frame clock.
 * @param user_data Optional user data.
 * @return gboolean G_SOURCE_CONTINUE
 */
gboolean on_waveform_tick(GtkWidget *widget, GdkFrameClock *frame_clock, gpointer user_data) {
    (void)frame_clock;
    (void)user_data;

    // 1. VU meters are cheap and must redraw unconditionally so they naturally decay to 0
    if (widget != waveform_area_bt && widget != waveform_area_input) {
        gtk_widget_queue_draw(widget);
        return G_SOURCE_CONTINUE;
    }

    // TELEMETRY: Extract RT Audio Dropouts safely on the UI thread
    uint32_t dropped = atomic_exchange_explicit(&rt_dropped_audio_frames, 0, memory_order_relaxed);
    if (dropped > 0) {
        log_and_notify(0, "[RT-EVENT] Warning: Dropped %u audio frames (Ringbuffer Overrun)\n",
                       dropped);
    }

    // Check for silent background encoder failure
    if (atomic_exchange_explicit(&encoder_disk_error, false, memory_order_acquire)) {
        if (btn_stop && gtk_widget_is_sensitive(btn_stop)) {
            g_signal_emit_by_name(btn_stop, "clicked");
            log_and_notify(1, "<span foreground='#ff4444'><b>Status: RECORDING ABORTED (Disk Full "
                              "/ I/O Error)</b></span>");
        }
    }

    if (atomic_exchange_explicit(&loop_wrap_pending, false, memory_order_acquire)) {
        // Signal on_stop_clicked to retain loop points and bypass track auto-advance
        atomic_store_explicit(&is_loop_wrapping, true, memory_order_release);

        // 1. Cache loop points before the "Stop" command accesses them
        size_t l_start = atomic_load_explicit(&loop_start_frame, memory_order_acquire);
        size_t l_end = atomic_load_explicit(&loop_end_frame, memory_order_acquire);
        size_t frames = atomic_load_explicit(&get_transport_state()->backing_track_frames,
                                             memory_order_acquire);

        // 2. Simulate Stop (Commits the take to the Undo ledger but stays on current track)
        if (btn_stop && gtk_widget_is_sensitive(btn_stop)) {
            g_signal_emit_by_name(btn_stop, "clicked");
        }

        // 3. Rewind the playhead back to the start of the loop safely
        if (frames > 0 && l_end > l_start) {
            atomic_store_explicit(&get_transport_state()->playback_pos, l_start,
                                  memory_order_release);
            double start_frac = (double)l_start / (double)frames;
            seek_backing_track(start_frac);
        }

        // 4. Suppress the video overwrite nag dialogue for automatic passes
        atomic_store_explicit(&suppress_video_overwrite, true, memory_order_release);

        // 5. Restart recording to trigger the 3-second countdown
        // For overdubs on loop wrap, always fall back to audio-only to preserve the base video
        // take.
        if (btn_record) {
            g_signal_emit_by_name(btn_record, "clicked");
        }
    }

    // 2. Track state changes for the massive waveform canvases independently
    static size_t last_pos_bt = SIZE_MAX;
    static size_t last_pos_input = SIZE_MAX;
    static void *last_tick_layers[QJ_MAX_TRACKS] = {NULL};
    static size_t last_frames_bt = SIZE_MAX;
    static size_t last_frames_input = SIZE_MAX;

    size_t current_pos =
        atomic_load_explicit(&get_transport_state()->playback_pos, memory_order_acquire);
    bool is_active =
        atomic_load_explicit(&get_transport_state()->is_playing, memory_order_acquire) ||
        atomic_load_explicit(&get_transport_state()->is_recording, memory_order_acquire);

    bool layers_swapped = false;

    // Update dynamic track time label
    refresh_track_time_display();

    for (int i = 0; i < QJ_MAX_TRACKS; i++) {
        if (master_tracks[i].active_buffer != last_tick_layers[i]) {
            layers_swapped = true;
            last_tick_layers[i] = master_tracks[i].active_buffer;
        }
    }

    size_t bt_frames =
        atomic_load_explicit(&get_transport_state()->backing_track_frames, memory_order_acquire);

    // PAGE TURN LOGIC (Only execute once per tick on the top canvas)
    if (bt_frames > 0 && widget == waveform_area_bt) {
        int width = gtk_widget_get_width(waveform_area_bt);
        double virtual_width = width * zoom_multiplier;

        // Dynamically update bounds if the window resized
        if (gtk_adjustment_get_upper(waveform_adj) != virtual_width) {
            gtk_adjustment_set_upper(waveform_adj, virtual_width);
            gtk_adjustment_set_page_size(waveform_adj, width);
            gtk_adjustment_set_step_increment(waveform_adj, 50.0);
            gtk_adjustment_set_page_increment(waveform_adj, width * 0.9);
            if (gtk_adjustment_get_value(waveform_adj) > virtual_width - width) {
                gtk_adjustment_set_value(waveform_adj,
                                         (virtual_width > width) ? (virtual_width - width) : 0.0);
            }
        }
        if (is_active || current_pos != last_pos_bt) {
            double playhead_virtual_x = ((double)current_pos / (double)bt_frames) * virtual_width;
            double scroll_val = gtk_adjustment_get_value(waveform_adj);

            if (playhead_virtual_x > scroll_val + width * 0.95) {
                gtk_adjustment_set_value(waveform_adj, playhead_virtual_x - width * 0.05);
            } else if (playhead_virtual_x < scroll_val) {
                double new_val = playhead_virtual_x - width * 0.05;
                gtk_adjustment_set_value(waveform_adj, new_val < 0 ? 0 : new_val);
            }
        }
    }

    // Gate redraws: Only paint if playing, dragging, seeking (pos change), or loading a new track
    if (widget == waveform_area_bt) {
        if (bt_frames != last_frames_bt) {
            update_zoom_button_label_to_length();
        }

        if (is_active || is_dragging || current_pos != last_pos_bt || layers_swapped ||
            bt_frames != last_frames_bt) {
            gtk_widget_queue_draw(widget);
        last_pos_bt = current_pos;
        last_frames_bt = bt_frames;

        // Auto-stop logic
        if (is_active && bt_frames > 0 && current_pos >= bt_frames) {
            if (btn_stop && gtk_widget_is_sensitive(btn_stop)) {
                g_signal_emit_by_name(btn_stop, "clicked");
                atomic_store_explicit(&seek_flag, false, memory_order_release);
                atomic_store_explicit(&get_transport_state()->playback_pos, 0,
                                      memory_order_release);
                atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);
                if (waveform_adj)
                    gtk_adjustment_set_value(waveform_adj, 0.0);
                refresh_track_time_display();
            }
        }
            }
    } else if (widget == waveform_area_input) {
        if (is_active || is_dragging || current_pos != last_pos_input || layers_swapped ||
            bt_frames != last_frames_input) {
            gtk_widget_queue_draw(widget);
            last_pos_input = current_pos;
            last_frames_input = bt_frames;
        }
    }

    return G_SOURCE_CONTINUE;
}

/**
 * @brief Forces the UI to discard its cached surfaces and float arrays, prompting a full rescan of
 * the audio memory on the next tick.
 * @return void
 */
void invalidate_waveform_caches(void) {
    cached_input_width = 0;
    cached_bt_width = 0;
    cached_bt_frames = 0;
}

void on_waveform_click_pressed(GtkGestureClick *gesture, int n_press, double x, double y,
                               gpointer user_data) {
    (void)n_press;
    (void)y;
    (void)user_data;

    GtkWidget *widget = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
    gtk_widget_set_focusable(widget, TRUE);
    gtk_widget_grab_focus(widget);

    if (atomic_load_explicit(&get_transport_state()->is_recording, memory_order_acquire))
        return;

    size_t total_frames =
        atomic_load_explicit(&get_transport_state()->backing_track_frames, memory_order_acquire);
    if (total_frames == 0)
        return;

    GdkModifierType state =
        gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));

    if (state & GDK_CONTROL_MASK) {
        gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_DENIED);
        return;
    }

    int width = gtk_widget_get_width(waveform_area_bt);
    if (width <= 0)
        return;

    double virtual_width = width * zoom_multiplier;
    double scroll_x = gtk_adjustment_get_value(waveform_adj);
    double click_fraction = (x + scroll_x) / virtual_width;

    if (click_fraction < 0.0)
        click_fraction = 0.0;
    if (click_fraction > 1.0)
        click_fraction = 1.0;

    if ((state & GDK_SHIFT_MASK) == 0) {
        clear_loop_points();
        seek_backing_track(click_fraction);
    } else {
        size_t current_pos =
            atomic_load_explicit(&get_transport_state()->playback_pos, memory_order_acquire);
        double playhead_fraction = (double)current_pos / (double)total_frames;

        if (playhead_fraction == click_fraction)
            click_fraction += 0.000001;

        set_loop_points(playhead_fraction, click_fraction);
        seek_backing_track(fmin(playhead_fraction, click_fraction));
    }

    update_multitrack_status_ui();

    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    gtk_widget_queue_draw(waveform_area_bt);
    gtk_widget_queue_draw(waveform_area_input);
}

/**
 * @brief Gesture callback triggered when a multitrack drag starts.
 * @param gesture The drag gesture object.
 * @param start_x The starting X coordinate.
 * @param start_y The starting Y coordinate.
 * @param user_data Optional user data.
 * @return void
 */
void on_drag_begin(GtkGestureDrag *gesture, double start_x, double start_y, gpointer user_data) {
    (void)start_y;
    (void)user_data;
    if (atomic_load_explicit(&get_transport_state()->is_recording, memory_order_acquire))
        return;

    size_t total_frames =
        atomic_load_explicit(&get_transport_state()->backing_track_frames, memory_order_acquire);
    if (total_frames == 0)
        return;

    int width = gtk_widget_get_width(waveform_area_bt);
    if (width <= 0)
        return;

    GdkModifierType state =
        gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));

    if (state & GDK_SHIFT_MASK) {
        gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_DENIED);
        return;
    }

    if (state & GDK_CONTROL_MASK) {
        is_panning = true;
        drag_start_scroll_x = gtk_adjustment_get_value(waveform_adj);
        return;
    }

    double virtual_width = width * zoom_multiplier;
    double scroll_x = gtk_adjustment_get_value(waveform_adj);
    drag_start_fraction = (start_x + scroll_x) / virtual_width;

    if (drag_start_fraction < 0.0)
        drag_start_fraction = 0.0;
    if (drag_start_fraction > 1.0)
        drag_start_fraction = 1.0;

    if (total_frames > 0) {
        size_t frame = (size_t)(drag_start_fraction * total_frames);
        drag_start_fraction = (double)frame / (double)total_frames;
    }

    is_dragging = true;
    current_drag_fraction = drag_start_fraction;

    clear_loop_points();
}

/**
 * @brief Gesture callback triggered during a multitrack drag operation.
 * @param gesture The drag gesture object.
 * @param offset_x The current X offset from start.
 * @param offset_y The current Y offset from start.
 * @param user_data Optional user data.
 * @return void
 */
void on_drag_update(GtkGestureDrag *gesture, double offset_x, double offset_y, gpointer user_data) {
    (void)offset_y;
    (void)user_data;

    size_t total_frames =
        atomic_load_explicit(&get_transport_state()->backing_track_frames, memory_order_acquire);
    if (total_frames == 0)
        return;

    if (is_panning) {
        double new_scroll = drag_start_scroll_x - (offset_x * 1.5);
        double upper = gtk_adjustment_get_upper(waveform_adj);
        double page = gtk_adjustment_get_page_size(waveform_adj);
        if (new_scroll < 0)
            new_scroll = 0;
        if (new_scroll > upper - page)
            new_scroll = upper - page;
        gtk_adjustment_set_value(waveform_adj, new_scroll);
        return;
    }

    if (!is_dragging)
        return;

    int width = gtk_widget_get_width(waveform_area_bt);
    if (width <= 0)
        return;

    double start_x, start_y;
    gtk_gesture_drag_get_start_point(gesture, &start_x, &start_y);

    double virtual_width = width * zoom_multiplier;
    double scroll_x = gtk_adjustment_get_value(waveform_adj);
    current_drag_fraction = (start_x + offset_x + scroll_x) / virtual_width;

    if (current_drag_fraction < 0.0)
        current_drag_fraction = 0.0;
    if (current_drag_fraction > 1.0)
        current_drag_fraction = 1.0;

    if (total_frames > 0) {
        size_t frame = (size_t)(current_drag_fraction * total_frames);
        current_drag_fraction = (double)frame / (double)total_frames;
    }

    gtk_widget_queue_draw(waveform_area_bt);
    gtk_widget_queue_draw(waveform_area_input);
}

/**
 * @brief Gesture callback triggered when a multitrack drag operation completes, applying loop
 * points.
 * @param gesture The drag gesture object.
 * @param offset_x The total X offset from start.
 * @param offset_y The total Y offset from start.
 * @param user_data Optional user data.
 * @return void
 */
void on_drag_end(GtkGestureDrag *gesture, double offset_x, double offset_y, gpointer user_data) {
    (void)offset_y;
    (void)user_data;

    if (is_panning) {
        is_panning = false;
        return;
    }

    if (atomic_load_explicit(&get_transport_state()->is_recording, memory_order_acquire))
        return;

    size_t total_frames =
        atomic_load_explicit(&get_transport_state()->backing_track_frames, memory_order_acquire);
    if (total_frames == 0 || drag_start_fraction < 0.0)
        return;

    int width = gtk_widget_get_width(waveform_area_bt);
    if (width <= 0)
        return;

    double start_x, start_y;
    gtk_gesture_drag_get_start_point(gesture, &start_x, &start_y);

    double virtual_width = width * zoom_multiplier;
    double scroll_x = gtk_adjustment_get_value(waveform_adj);
    double end_fraction = (start_x + offset_x + scroll_x) / virtual_width;

    if (end_fraction < 0.0)
        end_fraction = 0.0;
    if (end_fraction > 1.0)
        end_fraction = 1.0;

    is_dragging = false;

    if (fabs(offset_x) >= 10.0) {
        set_loop_points(drag_start_fraction, end_fraction);
        seek_backing_track(fmin(drag_start_fraction, end_fraction));
    }

    update_multitrack_status_ui();

    drag_start_fraction = -1.0;
    gtk_widget_queue_draw(waveform_area_bt);
    gtk_widget_queue_draw(waveform_area_input);
}

static bool get_video_dot_positions(GtkWidget *widget, double *out_start_x, double *out_end_x,
                                    double *out_y) {

    if (video_end_frame <= video_start_frame)
        return false;

    size_t t_frames =
        atomic_load_explicit(&get_transport_state()->backing_track_frames, memory_order_acquire);
    if (t_frames == 0)
        return false;

    int width = gtk_widget_get_width(widget);
    double virtual_width = width * zoom_multiplier;
    double scroll_x = gtk_adjustment_get_value(waveform_adj);

    *out_y = gtk_widget_get_height(widget) / 2.0;
    *out_start_x = (((double)video_start_frame / (double)t_frames) * virtual_width) - scroll_x;
    *out_end_x = (((double)video_end_frame / (double)t_frames) * virtual_width) - scroll_x;
    return true;
}

/**
 * @brief Determines if the active video span belongs to a deletable live/session video take
 *        rather than an imported external video file.
 * @return true if the video take can be removed via the timeline dots, false if external.
 */
static bool is_deletable_video_take(void) {
    if (has_active_live_video() || strlen(stashed_raw_path) > 0)
        return true;
    if (strlen(ui_state.selected_track_path) > 0 && is_video_file(ui_state.selected_track_path)) {
        size_t t_frames =
        atomic_load_explicit(&get_transport_state()->backing_track_frames, memory_order_acquire);
        if (video_start_frame == 0 && video_end_frame >= t_frames)
            return false;
    }
    return true;
}

gboolean on_waveform_query_tooltip(GtkWidget *widget, gint x, gint y, gboolean keyboard_mode,
                                   GtkTooltip *tooltip, gpointer user_data) {
    (void)user_data;
    if (keyboard_mode)
        return FALSE;

    double dot_start_x, dot_end_x, dot_y;
    if (get_video_dot_positions(widget, &dot_start_x, &dot_end_x, &dot_y)) {
        if ((fabs((double)x - dot_start_x) <= 15.0 && fabs((double)y - dot_y) <= 15.0) ||
            (fabs((double)x - dot_end_x) <= 15.0 && fabs((double)y - dot_y) <= 15.0)) {
            if (!is_deletable_video_take()) {
                gtk_tooltip_set_text(tooltip, "External video track");
            } else {
                gtk_tooltip_set_text(tooltip, "Right-click dot to remove video");
            }
            return TRUE;
            }
    }
    return FALSE;
                                   }

                                   static void on_video_delete_confirm(GObject *source_object, GAsyncResult *res, gpointer user_data) {
                                       (void)user_data;
                                       GtkAlertDialog *alert = GTK_ALERT_DIALOG(source_object);
                                       if (gtk_alert_dialog_choose_finish(alert, res, NULL) == 0) {
                                           on_stop_clicked(NULL, NULL);

                                           if (!await_rt_thread_detach())
                                               return;

                                           int rec_track = atomic_load_explicit(&current_recording_track, memory_order_acquire);

                                           // Safe 0-RAM math state backup specifically to preserve video bounds metadata
                                           current_transaction_id++;
                                           push_math_undo_state(rec_track, current_transaction_id, 1.0f, 0, 0);

                                           mark_current_transaction_as_video_deletion();

                                           abort_active_video_preview(true);
                                           force_clear_mkv_video_preview();

                                           video_start_frame = 0;
                                           video_end_frame = 0;
                                           video_track_index = -1;

                                           mark_session_dirty();
                                           resume_rt_thread();

                                           invalidate_waveform_caches();
                                           if (waveform_area_bt)
                                               gtk_widget_queue_draw(waveform_area_bt);
                                           if (waveform_area_input)
                                               gtk_widget_queue_draw(waveform_area_input);

                                           log_and_notify(1, "Status: Video removed from session");
                                       }
                                   }

                                   void on_waveform_right_click(GtkGestureClick *gesture, int n_press, double x, double y,
                                                                gpointer user_data) {
                                       (void)n_press;
                                       (void)user_data;
                                       GtkWidget *widget = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
                                       double dot_start_x, dot_end_x, dot_y;

                                       if (get_video_dot_positions(widget, &dot_start_x, &dot_end_x, &dot_y)) {
                                           if ((fabs(x - dot_start_x) <= 15.0 && fabs(y - dot_y) <= 15.0) ||
                                               (fabs(x - dot_end_x) <= 15.0 && fabs(y - dot_y) <= 15.0)) {

                                               gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);

                                           if (!is_deletable_video_take()) {
                                               GtkAlertDialog *alert = gtk_alert_dialog_new("Unsupported Action");
                                               gtk_alert_dialog_set_detail(
                                                   alert, "Deleting the video track from external files is not supported.");
                                               GtkWindow *parent = GTK_WINDOW(gtk_widget_get_root(widget));
                                               gtk_alert_dialog_show(alert, parent);
                                               g_object_unref(alert);
                                           } else {
                                               GtkAlertDialog *alert = gtk_alert_dialog_new("Remove Video Take?");
                                               gtk_alert_dialog_set_detail(
                                                   alert,
                                                   "You are about to delete this video take from the session. Are you sure?");
                                               const char *buttons[] = {"Delete Video", "Cancel", NULL};
                                               gtk_alert_dialog_set_buttons(alert, buttons);
                                               gtk_alert_dialog_set_cancel_button(alert, 1);
                                               gtk_alert_dialog_set_default_button(alert, 0);

                                               GtkWindow *parent = GTK_WINDOW(gtk_widget_get_root(widget));
                                               gtk_alert_dialog_choose(alert, parent, NULL, on_video_delete_confirm, NULL);
                                               g_object_unref(alert);
                                           }
                                           return;
                                               }
                                       }
                                       gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_DENIED);
                                                                }

static float pending_gain_multiplier = 1.0f;
static guint gain_debounce_id = 0;

/**
 * @brief Deferred callback to apply accumulated gain multipliers directly on the main thread.
 * Threading overhead has been removed as the zero-RAM inverse NLE math executes instantly.
 * @param user_data Optional user data.
 * @return G_SOURCE_REMOVE to destroy the one-shot timer.
 */
static gboolean apply_gain_deferred(gpointer user_data) {
    (void)user_data;
    float mult = pending_gain_multiplier;

    pending_gain_multiplier = 1.0f;
    gain_debounce_id = 0;

    apply_gain_selection(mult);

    mark_session_dirty();

    update_multitrack_status_ui();

    invalidate_waveform_caches();

    if (waveform_area_bt)
        gtk_widget_queue_draw(waveform_area_bt);
    if (waveform_area_input)
        gtk_widget_queue_draw(waveform_area_input);

    return G_SOURCE_REMOVE;
}

/**
 * @brief Scroll event handler for waveform areas.
 * Shift+Scroll adjusts destructive track/selection gain; unmodified Scroll or Ctrl+Scroll zooms the
 * timeline.
 * @param controller The scroll event controller.
 * @param dx X-axis delta.
 * @param dy Y-axis delta.
 * @param user_data Optional user data.
 * @return TRUE if the event was consumed, FALSE otherwise.
 */
gboolean on_waveform_scroll(GtkEventControllerScroll *controller, double dx, double dy,
                            gpointer user_data) {
    (void)user_data;
    GdkModifierType state =
        gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(controller));
    double delta = (dy != 0.0) ? dy : dx;
    if (delta == 0.0)
        return FALSE;

    if (state & GDK_SHIFT_MASK) {
        float multiplier = (delta < 0) ? 1.122018f : 0.891251f;

        pending_gain_multiplier *= multiplier;

        if (gain_debounce_id != 0) {
            g_source_remove(gain_debounce_id);
        }
        gain_debounce_id = g_timeout_add(100, apply_gain_deferred, NULL);

        return TRUE;
    }

    if (delta < 0) {
        zoom_multiplier *= 1.05;
    } else {
        zoom_multiplier /= 1.05;
    }

    if (zoom_multiplier < 1.0)
        zoom_multiplier = 1.0;
    if (zoom_multiplier > 5000000.0)
        zoom_multiplier = 5000000.0;

    size_t current_pos =
        atomic_load_explicit(&get_transport_state()->playback_pos, memory_order_acquire);
    size_t total_frames =
        atomic_load_explicit(&get_transport_state()->backing_track_frames, memory_order_acquire);

    if (total_frames > 0 && waveform_area_bt) {
        int width = gtk_widget_get_width(waveform_area_bt);
        double virtual_width = width * zoom_multiplier;
        double playhead_virtual_x = ((double)current_pos / (double)total_frames) * virtual_width;

        gtk_adjustment_set_upper(waveform_adj, virtual_width);
        gtk_adjustment_set_page_size(waveform_adj, width);

        double new_scroll = playhead_virtual_x - (width / 2.0);
        if (new_scroll < 0)
            new_scroll = 0;
        if (new_scroll > virtual_width - width)
            new_scroll = virtual_width - width;

        gtk_adjustment_set_value(waveform_adj, new_scroll);
    }

    invalidate_waveform_caches();
    if (waveform_area_bt)
        gtk_widget_queue_draw(waveform_area_bt);
    if (waveform_area_input)
        gtk_widget_queue_draw(waveform_area_input);

    return TRUE;
}
