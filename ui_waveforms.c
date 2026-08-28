#include "ui_waveforms.h"
#include "ui_globals.h"
#include "audio_engine.h"
#include <math.h>

static double drag_start_fraction = -1.0;
static double current_drag_fraction = -1.0;
static bool is_dragging = false;
static bool is_panning = false;
static double drag_start_scroll_x = 0.0;
static gint64 last_seek_time = 0; // Timestamp cache to debounce rapid seek commands

#define MAX_UI_WIDTH 8192

static int cached_bt_width = 0;
static int cached_bt_height = 0;
static int cached_active_tracks = 1;
float *cached_bt_ptr = NULL;
static float bt_min_l[MAX_UI_WIDTH], bt_max_l[MAX_UI_WIDTH];
static float bt_min_r[MAX_UI_WIDTH], bt_max_r[MAX_UI_WIDTH];

// Dynamic BT Sweep caches
static float sweep_min_l[MAX_UI_WIDTH], sweep_max_l[MAX_UI_WIDTH];
static float sweep_min_r[MAX_UI_WIDTH], sweep_max_r[MAX_UI_WIDTH];

static int cached_input_width = 0;
static int cached_input_height = 0;
static float input_min_l[MAX_UI_WIDTH], input_max_l[MAX_UI_WIDTH];
static float input_min_r[MAX_UI_WIDTH], input_max_r[MAX_UI_WIDTH];

// Off-screen Cairo cache surfaces
static cairo_surface_t *surface_bt = NULL;
static cairo_surface_t *surface_input_grid = NULL;

extern _Atomic bool is_mkv_mode;

// Add forward declarations so the compiler knows these functions exist regardless of placement order
static void draw_envelope_waveform(cairo_t *cr, int width, int height, const float *min_buf, const float *max_buf, float gain, double y_center);
static void draw_sample_waveform(cairo_t *cr, int width, int height, const float *audio_buf, size_t total_frames, double scroll_x, double frames_per_pixel, float gain, double y_center, int channel_offset);
static void draw_playhead(cairo_t *cr, int width, int height, size_t current_pos, size_t total_frames);
void draw_waveform_grid(cairo_t *cr, int width, int height, size_t total_frames);

/**
 * @brief Assigns a highly visible neon color for each multitrack layer.
 */
static void set_track_color(cairo_t *cr, int track_idx, double alpha) {
    switch (track_idx) {
        case 0: cairo_set_source_rgba(cr, 0.9, 0.9, 0.9, alpha); break; // Base: Brighter Silver/Gray
        case 1: cairo_set_source_rgba(cr, 0.0, 0.8, 1.0, alpha); break; // Overdub 1: Cyan/Electric Blue
        case 2: cairo_set_source_rgba(cr, 1.0, 0.2, 0.8, alpha); break; // Overdub 2: Neon Magenta
        case 3: cairo_set_source_rgba(cr, 0.4, 1.0, 0.2, alpha); break; // Overdub 3: Lime Green
        case 4: cairo_set_source_rgba(cr, 1.0, 0.8, 0.0, alpha); break; // Overdub 4: Bright Yellow
        case 5: cairo_set_source_rgba(cr, 0.7, 0.3, 1.0, alpha); break; // Overdub 5: Electric Purple
        case 6: cairo_set_source_rgba(cr, 1.0, 0.5, 0.0, alpha); break; // Overdub 6: Neon Orange
        case 7: cairo_set_source_rgba(cr, 0.0, 1.0, 0.5, alpha); break; // Overdub 7: Spring Green
        case 8: cairo_set_source_rgba(cr, 0.8, 0.6, 1.0, alpha); break; // Overdub 8: Lavender
        case 9: cairo_set_source_rgba(cr, 1.0, 0.3, 0.3, alpha); break; // Overdub 9: Coral Red
        case 10: cairo_set_source_rgba(cr, 0.2, 0.6, 1.0, alpha); break; // Overdub 10: Sky Blue
        case 11: cairo_set_source_rgba(cr, 0.9, 0.9, 0.2, alpha); break; // Overdub 11: Lemon
        default: cairo_set_source_rgba(cr, 0.8, 0.8, 0.8, alpha); break;
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
void on_draw_track_color_dot(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data) {
    (void)area;
    int track_idx = GPOINTER_TO_INT(user_data);
    set_track_color(cr, track_idx, 1.0);

    bool has_audio = atomic_load_explicit(&master_tracks[track_idx].has_audio, memory_order_acquire);

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
    if (amp <= 0.001f) return 0.0f;
    float db = 20.0f * log10f(amp);
    if (db < -60.0f) return 0.0f;
    float vu = (db + 60.0f) / 60.0f;
    return (vu > 1.0f) ? 1.0f : vu;
}

/**
 * @brief Helper to scan a block of frames and update the min/max amplitude for left and right channels.
 * @param src_buf Source interleaved float buffer.
 * @param start_frame Starting frame index.
 * @param end_frame Ending frame index.
 * @param min_l Pointer to tracking variable for left min.
 * @param max_l Pointer to tracking variable for left max.
 * @param min_r Pointer to tracking variable for right min.
 * @param max_r Pointer to tracking variable for right max.
 */
static inline void scan_waveform_extents(const float *src_buf, size_t start_frame, size_t end_frame, float *min_l, float *max_l, float *min_r, float *max_r) {
    float lmin = *min_l, lmax = *max_l, rmin = *min_r, rmax = *max_r;
    for (size_t f = start_frame; f < end_frame; f++) {
        float vl = src_buf[f * 2];
        float vr = src_buf[f * 2 + 1];
        if (vl < lmin) lmin = vl;
        if (vl > lmax) lmax = vl;
        if (vr < rmin) rmin = vr;
        if (vr > rmax) rmax = vr;
    }
    *min_l = lmin; *max_l = lmax;
    *min_r = rmin; *max_r = rmax;
}

/**
 * @brief Draws the background timeline grid and timestamp labels for the waveform area.
 * Dynamically scales the tick intervals based on the visible viewport duration rather than the total track length,
 * ensuring continuous granularity at deep zoom levels.
 * @param cr The Cairo rendering context.
 * @param width The width of the drawing area.
 * @param height The height of the drawing area.
 * @param total_frames The total length of the loaded track for calculating bounds.
 */
void draw_waveform_grid(cairo_t *cr, int width, int height, size_t total_frames) {
    cairo_set_source_rgb(cr, 0.15, 0.15, 0.18);
    cairo_paint(cr);

    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.15);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, 0, height / 2.0);
    cairo_line_to(cr, width, height / 2.0);
    cairo_stroke(cr);

    if (total_frames == 0 || active_sample_rate <= 0) return;

    double total_seconds = (double)total_frames / active_sample_rate;
    int bt_width = gtk_widget_get_width(waveform_area_bt);
    double virtual_width = bt_width * zoom_multiplier;
    double pixels_per_sec = virtual_width / total_seconds;
    double scroll_x = gtk_adjustment_get_value(waveform_adj);

    double visible_seconds = width / pixels_per_sec;

    // --- Algorithmic Grid Scaling ---
    // Mathematically target ~8 text labels across whatever the current visible screen width is
    double target_text_step = visible_seconds / 8.0;

    // Calculate dynamic base-10 magnitude to snap to clean numbers (1, 2, or 5)
    double mag = pow(10.0, floor(log10(target_text_step)));
    double rel = target_text_step / mag;

    double text_step, tick_step;
    if (rel < 2.0) text_step = 1.0 * mag;
    else if (rel < 5.0) text_step = 2.0 * mag;
    else text_step = 5.0 * mag;

    // Safety bounds for extreme atomic-level zooming
    if (text_step < 0.00001) text_step = 0.00001;
    tick_step = text_step / 10.0;

    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 10.0);
    cairo_set_line_width(cr, 1.0);

    double start_sec = scroll_x / pixels_per_sec;
    double end_sec = (scroll_x + width) / pixels_per_sec;

    // Align starting tick safely to the grid
    double current_sec = floor(start_sec / tick_step) * tick_step;

    // High multiplier for safe float modulo math (handles nanosecond bounds safely)
    long long mult = 100000000LL;
    long long txt_val = llround(text_step * mult);
    long long mid_val = txt_val / 2;

    while (current_sec <= end_sec && current_sec <= total_seconds) {
        if (current_sec < 0.0) {
            current_sec += tick_step;
            continue;
        }

        double x = (current_sec * pixels_per_sec) - scroll_x;
        if (x < -50 || x > width + 50) {
            current_sec += tick_step;
            continue;
        }

        long long cur_val = llround(current_sec * mult);
        bool is_text = (txt_val > 0) && (cur_val % txt_val == 0);
        bool is_mid = (mid_val > 0) && (cur_val % mid_val == 0);

        if (is_text) {
            cairo_set_source_rgba(cr, 0.8, 0.8, 0.8, 1.0); // Major Tick
            cairo_move_to(cr, x, 0);
            cairo_line_to(cr, x, height);
            cairo_stroke(cr);

            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.8);
            cairo_move_to(cr, x + 3, 12);

            char time_str[32];
            if (text_step >= 1.0) {
                int mins = (int)current_sec / 60;
                int secs = (int)current_sec % 60;
                if (secs == 0) snprintf(time_str, sizeof(time_str), "%d:00", mins);
                else snprintf(time_str, sizeof(time_str), "%d:%02d", mins, secs);
            } else if (text_step >= 0.1) {
                snprintf(time_str, sizeof(time_str), "%.1fs", current_sec);
            } else if (text_step >= 0.01) {
                snprintf(time_str, sizeof(time_str), "%.2fs", current_sec);
            } else if (text_step >= 0.001) {
                snprintf(time_str, sizeof(time_str), "%.3fs", current_sec);
            } else if (text_step >= 0.0001) {
                snprintf(time_str, sizeof(time_str), "%.4fs", current_sec);
            } else {
                snprintf(time_str, sizeof(time_str), "%.5fs", current_sec);
            }
            cairo_show_text(cr, time_str);
        } else if (is_mid) {
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.20); // Medium Tick
            cairo_move_to(cr, x, 0);
            cairo_line_to(cr, x, height);
            cairo_stroke(cr);
        } else {
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.08); // Minor Tick
            cairo_move_to(cr, x, 0);
            cairo_line_to(cr, x, height);
            cairo_stroke(cr);
        }
        current_sec += tick_step;
    }
}

/**
 * @brief Cairo draw callback for the backing track waveform area.
 * @param area The drawing area widget.
 * @param cr The Cairo rendering context.
 * @param width The width of the drawing area.
 * @param height The height of the drawing area.
 * @param user_data Optional user data.
 * @return void
 */
void on_draw_bt_waveform(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data) {
    (void)area; (void)user_data;

    if (ui_state.is_loading_track || !pristine_bt_buf || pristine_frames == 0) {
        draw_waveform_grid(cr, width, height, 0);
        return;
    }

    int bt_width = gtk_widget_get_width(waveform_area_bt);
    double virtual_width = bt_width * zoom_multiplier;
    double scroll_x = gtk_adjustment_get_value(waveform_adj);

    double frames_per_pixel = (double)pristine_frames / virtual_width;
    if (frames_per_pixel == 0) frames_per_pixel = 1.0;

    size_t current_pos = atomic_load_explicit(&playback_pos, memory_order_acquire);
    int active_tracks = atomic_load_explicit(&active_track_count, memory_order_relaxed);
    int rec_track = atomic_load_explicit(&current_recording_track, memory_order_relaxed);

    static double cached_bt_scroll_x = -1.0;
    static double cached_bt_zoom = -1.0;
    static float *cached_track_ptrs[MAX_TRACKS] = {NULL};
    static bool cached_audible[MAX_TRACKS] = {false};

    bool any_solo_active = false;
    bool track_audible[MAX_TRACKS] = {false};

    // Pass 1: Determine if any solo is engaged
    for (int l = 0; l < MAX_TRACKS; l++) {
        if (atomic_load_explicit(&master_tracks[l].is_soloed, memory_order_relaxed)) {
            any_solo_active = true;
            break;
        }
    }

    bool pointers_changed = false;
    // Pass 2: Map audibility and check for state invalidation
    for (int i = 0; i < MAX_TRACKS; i++) {
        // STRICT: A layer is only audible if it is unmuted/soloed AND explicitly contains recorded audio
        track_audible[i] = (!any_solo_active || atomic_load_explicit(&master_tracks[i].is_soloed, memory_order_relaxed)) &&
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

    if (width != cached_bt_width || height != cached_bt_height || pristine_bt_buf != cached_bt_ptr ||
        active_tracks != cached_active_tracks || scroll_x != cached_bt_scroll_x || zoom_multiplier != cached_bt_zoom || pointers_changed) {

        if (surface_bt) {
            cairo_surface_destroy(surface_bt);
            surface_bt = NULL;
        }

        surface_bt = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    cairo_t *surface_cr = cairo_create(surface_bt);

    draw_waveform_grid(surface_cr, width, height, backing_track_frames);

    // Clamp the render bounds to strictly prevent out-of-bounds pointer sweeps
    int max_render_track = active_tracks;
    if (max_render_track > MAX_TRACKS) max_render_track = MAX_TRACKS;

    for (int l = 0; l < max_render_track; l++) {
        float *active_buf = master_tracks[l].active_buffer;
        if (!active_buf || !track_audible[l]) continue;

        if (frames_per_pixel >= 1.0) {
            memset(bt_min_l, 0, sizeof(bt_min_l));
            memset(bt_max_l, 0, sizeof(bt_max_l));
            memset(bt_min_r, 0, sizeof(bt_min_r));
            memset(bt_max_r, 0, sizeof(bt_max_r));

            for (int x = 0; x < width && x < MAX_UI_WIDTH; x++) {
                size_t start_frame = (size_t)((scroll_x + x) * frames_per_pixel);
                size_t end_frame = (size_t)((scroll_x + x + 1) * frames_per_pixel);

                if (end_frame <= start_frame) end_frame = start_frame + 1;
                if (start_frame >= pristine_frames) continue;
                if (end_frame > pristine_frames) end_frame = pristine_frames;

                scan_waveform_extents(active_buf, start_frame, end_frame, &bt_min_l[x], &bt_max_l[x], &bt_min_r[x], &bt_max_r[x]);
            }

            float layer_gain = atomic_load_explicit(&master_tracks[l].gain, memory_order_relaxed);

            // Restore OVER operator to prevent additive color clipping when tracks stack
            cairo_set_operator(surface_cr, CAIRO_OPERATOR_OVER);
            double alpha = 0.65; // Boost alpha slightly to retain vibrancy under standard overlap
            set_track_color(surface_cr, l, alpha);

            cairo_set_line_width(surface_cr, 1.0);
            draw_envelope_waveform(surface_cr, width, height, bt_min_l, bt_max_l, layer_gain, height / 4.0);
            draw_envelope_waveform(surface_cr, width, height, bt_min_r, bt_max_r, layer_gain, 3.0 * height / 4.0);
        } else {
            float layer_gain = atomic_load_explicit(&master_tracks[l].gain, memory_order_relaxed);
            double alpha = 0.65;

            // Restore OVER operator here as well for deep zoom views
            cairo_set_operator(surface_cr, CAIRO_OPERATOR_OVER);
            set_track_color(surface_cr, l, alpha);

            cairo_set_line_width(surface_cr, 1.5);
            draw_sample_waveform(surface_cr, width, height, active_buf, pristine_frames, scroll_x, frames_per_pixel, layer_gain, height / 4.0, 0);
            draw_sample_waveform(surface_cr, width, height, active_buf, pristine_frames, scroll_x, frames_per_pixel, layer_gain, 3.0 * height / 4.0, 1);
        }
    }

    cairo_destroy(surface_cr);
    cached_bt_width = width;
    cached_bt_height = height;
    cached_bt_ptr = pristine_bt_buf;
    cached_active_tracks = active_tracks;
    cached_bt_scroll_x = scroll_x;
    cached_bt_zoom = zoom_multiplier;
        }

        if (surface_bt) {
            cairo_set_source_surface(cr, surface_bt, 0, 0);
            cairo_paint(cr);
        }

        if (active_tracks > 1 && atomic_load_explicit(&engine_is_recording, memory_order_relaxed)) {
            float *rec_buf = master_tracks[rec_track].active_buffer;

            if (frames_per_pixel >= 1.0 && rec_buf) {
                memset(sweep_min_l, 0, sizeof(sweep_min_l));
                memset(sweep_max_l, 0, sizeof(sweep_max_l));
                memset(sweep_min_r, 0, sizeof(sweep_min_r));
                memset(sweep_max_r, 0, sizeof(sweep_max_r));

                for (int x = 0; x < width && x < MAX_UI_WIDTH; x++) {
                    size_t start_frame = (size_t)((scroll_x + x) * frames_per_pixel);
                    size_t end_frame = (size_t)((scroll_x + x + 1) * frames_per_pixel);

                    if (start_frame >= current_pos) break;
                    if (end_frame > current_pos) end_frame = current_pos;

                    scan_waveform_extents(rec_buf, start_frame, end_frame,
                                          &sweep_min_l[x], &sweep_max_l[x], &sweep_min_r[x], &sweep_max_r[x]);
                }

                cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
                set_track_color(cr, rec_track, 0.5);
                cairo_set_line_width(cr, 1.0);
                draw_envelope_waveform(cr, width, height, sweep_min_l, sweep_max_l, 1.0f, height / 4.0);
                draw_envelope_waveform(cr, width, height, sweep_min_r, sweep_max_r, 1.0f, 3.0 * height / 4.0);
            } else if (rec_buf) {
                cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
                set_track_color(cr, rec_track, 0.5);
                cairo_set_line_width(cr, 1.5);
                draw_sample_waveform(cr, width, height, rec_buf, current_pos, scroll_x, frames_per_pixel, 1.0f, height / 4.0, 0);
                draw_sample_waveform(cr, width, height, rec_buf, current_pos, scroll_x, frames_per_pixel, 1.0f, 3.0 * height / 4.0, 1);
            }
        }

        if (atomic_load_explicit(&loop_active, memory_order_acquire)) {
            size_t l_start = atomic_load_explicit(&loop_start_frame, memory_order_relaxed);
            size_t l_end = atomic_load_explicit(&loop_end_frame, memory_order_relaxed);

            size_t t_frames = atomic_load_explicit(&backing_track_frames, memory_order_acquire);
            double start_px = (((double)l_start / (double)t_frames) * virtual_width) - scroll_x;
            double end_px = (((double)l_end / (double)t_frames) * virtual_width) - scroll_x;

            cairo_set_source_rgba(cr, 0.3, 0.7, 1.0, 0.25);
            cairo_rectangle(cr, round(start_px), 0, round(end_px) - round(start_px), height);
            cairo_fill(cr);
        } else if (is_dragging && drag_start_fraction >= 0.0) {
            double start_px = (drag_start_fraction * virtual_width) - scroll_x;
            double end_px = (current_drag_fraction * virtual_width) - scroll_x;
            if (start_px > end_px) { double tmp = start_px; start_px = end_px; end_px = tmp; }
            cairo_set_source_rgba(cr, 0.3, 0.7, 1.0, 0.25);
            cairo_rectangle(cr, round(start_px), 0, round(end_px) - round(start_px), height);
            cairo_fill(cr);
        }

        draw_playhead(cr, width, height, current_pos, backing_track_frames);
}

/**
 * @brief Applies the standard multi-color stop linear gradient to a VU meter cairo context.
 * @param cr The Cairo rendering context.
 * @param width The total width of the widget.
 */
static void apply_vu_gradient(cairo_t *cr, int width) {
    cairo_pattern_t *pat = cairo_pattern_create_linear(0.0, 0.0, width, 0.0);
    cairo_pattern_add_color_stop_rgb(pat, 0.0,   0.18, 0.80, 0.44);
    cairo_pattern_add_color_stop_rgb(pat, 0.899, 0.18, 0.80, 0.44);
    cairo_pattern_add_color_stop_rgb(pat, 0.90,  0.94, 0.76, 0.05);
    cairo_pattern_add_color_stop_rgb(pat, 0.979, 0.94, 0.76, 0.05);
    cairo_pattern_add_color_stop_rgb(pat, 0.98,  0.90, 0.29, 0.23);
    cairo_pattern_add_color_stop_rgb(pat, 1.0,   0.90, 0.29, 0.23);
    cairo_set_source(cr, pat);
    cairo_pattern_destroy(pat);
}

/**
 * @brief ENVELOPE MODE: Draws min/max audio blocks for zoomed-out views.
 * Skips silent columns and bypasses anti-aliasing to minimize Cairo path construction overhead.
 * @param cr The Cairo rendering context.
 * @param width The pixel width of the drawing area.
 * @param height The pixel height of the drawing area.
 * @param min_buf Array of minimum amplitude values per pixel column.
 * @param max_buf Array of maximum amplitude values per pixel column.
 * @param gain The linear gain multiplier applied to the rendered waveform.
 * @param y_center The vertical baseline offset for this channel.
 * @return void
 */
static void draw_envelope_waveform(cairo_t *cr, int width, int height, const float *min_buf, const float *max_buf, float gain, double y_center) {
    int max_x = (width < MAX_UI_WIDTH) ? width : MAX_UI_WIDTH;
    double scale_h = (height / 4.0) * 0.9;

    cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);

    // Anchor the starting point
    cairo_move_to(cr, 0, y_center - (max_buf[0] * gain * scale_h));

    for (int x = 0; x < max_x; x++) {
        float min_val = min_buf[x] * gain;
        float max_val = max_buf[x] * gain;

        double y_top = y_center - (max_val * scale_h);
        double y_bot = y_center - (min_val * scale_h);

        // Connect horizontally to the next sample point
        cairo_line_to(cr, x, y_top);

        // If zoomed out, draw the vertical min/max envelope bar
        if (y_top != y_bot) {
            cairo_line_to(cr, x, y_bot);
        }
    }
    cairo_stroke(cr);
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_DEFAULT);
}

/**
 * @brief OSCILLOSCOPE MODE: Draws precise sample interpolation and lollipop markers for deep zooms.
 * @param cr The Cairo rendering context.
 * @param width The pixel width of the drawing area.
 * @param height The pixel height of the drawing area.
 * @param audio_buf The raw interleaved audio buffer.
 * @param total_frames Total frames available in the buffer.
 * @param scroll_x The current virtual scroll offset.
 * @param frames_per_pixel The sub-pixel resolution ratio.
 * @param gain The linear gain multiplier.
 * @param y_center The vertical baseline offset.
 * @param channel_offset 0 for Left, 1 for Right.
 * @return void
 */
static void draw_sample_waveform(cairo_t *cr, int width, int height, const float *audio_buf, size_t total_frames, double scroll_x, double frames_per_pixel, float gain, double y_center, int channel_offset) {
    double scale_h = (height / 4.0) * 0.9;
    double pixels_per_frame = 1.0 / frames_per_pixel;

    size_t start_frame = (size_t)(scroll_x * frames_per_pixel);
    size_t end_frame = (size_t)((scroll_x + width) * frames_per_pixel) + 2;

    if (start_frame >= total_frames) return;
    if (end_frame > total_frames) end_frame = total_frames;

    cairo_set_antialias(cr, CAIRO_ANTIALIAS_DEFAULT);

    // 1. Draw the continuous interpolated line
    cairo_move_to(cr, (start_frame / frames_per_pixel) - scroll_x, y_center - (audio_buf[start_frame * 2 + channel_offset] * gain * scale_h));
    for (size_t i = start_frame + 1; i < end_frame; i++) {
        cairo_line_to(cr, (i / frames_per_pixel) - scroll_x, y_center - (audio_buf[i * 2 + channel_offset] * gain * scale_h));
    }
    cairo_stroke(cr);

    // 2. Draw "Lollipops" (stems and dots) if zoomed in deeply (> 8 pixels per sample)
    if (pixels_per_frame > 8.0) {
        for (size_t i = start_frame; i < end_frame; i++) {
            double x = (i / frames_per_pixel) - scroll_x;
            double y = y_center - (audio_buf[i * 2 + channel_offset] * gain * scale_h);

            cairo_set_line_width(cr, 0.5);
            cairo_move_to(cr, x, y_center);
            cairo_line_to(cr, x, y);
            cairo_stroke(cr);

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
static void draw_playhead(cairo_t *cr, int width, int height, size_t current_pos, size_t total_frames) {
    if (current_pos > 0 && total_frames > 0) {
        int bt_width = gtk_widget_get_width(waveform_area_bt);
        double virtual_width = bt_width * zoom_multiplier;
        double scroll_x = gtk_adjustment_get_value(waveform_adj);
        double playhead_x = (((double)current_pos / (double)total_frames) * virtual_width) - scroll_x;

        if (playhead_x >= 0 && playhead_x <= width) {
            cairo_set_source_rgb(cr, 1.0, 0.8, 0.0);
            cairo_set_line_width(cr, 2.0);
            cairo_move_to(cr, playhead_x, 0);
            cairo_line_to(cr, playhead_x, height);
            cairo_stroke(cr);
        }
    }
}

// --- PUBLIC CALLBACKS ---
/**
 * @brief Cairo draw callback for a generic horizontal VU meter.
 * @param area The drawing area widget.
 * @param cr The Cairo rendering context.
 * @param width The width of the meter.
 * @param height The height of the meter.
 * @param user_data Pointer to the atomic float providing peak amplitude data.
 * @return void
 */
void on_draw_vu_meter(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data) {
    (void)area;
    _Atomic float *peak_ptr = (_Atomic float *)user_data;
    float amp = atomic_load_explicit(peak_ptr, memory_order_relaxed);
    float vu = amp_to_vu(amp);

    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_fill(cr);

    if (vu > 0.0f) {
        apply_vu_gradient(cr, width);
        cairo_rectangle(cr, 0, 0, width * vu, height);
        cairo_fill(cr);
    }

    cairo_set_source_rgb(cr, 0.75, 0.75, 0.75);
    cairo_set_line_width(cr, 1.0);
    cairo_rectangle(cr, 0.5, 0.5, width - 1.0, height - 1.0);
    cairo_stroke(cr);
}

/**
 * @brief UI tick callback to schedule redraws for dynamic waveform and meter widgets.
 * Intelligently sleeps massive canvas repaints when the engine is idle.
 * Memory state variables are tracked independently per waveform to prevent race conditions
 * and missed redraws when multiple canvases share the same tick rate.
 * @param widget The widget to queue for redraw.
 * @param frame_clock The GDK frame clock.
 * @param user_data Optional user data.
 * @return gboolean G_SOURCE_CONTINUE
 */
gboolean on_waveform_tick(GtkWidget *widget, GdkFrameClock *frame_clock, gpointer user_data) {
    (void)frame_clock; (void)user_data;

    // 1. VU meters are cheap and must redraw unconditionally so they naturally decay to 0
    if (widget != waveform_area_bt && widget != waveform_area_input) {
        gtk_widget_queue_draw(widget);
        return G_SOURCE_CONTINUE;
    }

    // TELEMETRY: Extract RT Audio Dropouts safely on the UI thread
    extern _Atomic uint32_t rt_dropped_audio_frames;
    uint32_t dropped = atomic_exchange_explicit(&rt_dropped_audio_frames, 0, memory_order_relaxed);
    if (dropped > 0) {
        printf("[RT-EVENT] Warning: Dropped %u audio frames (Ringbuffer Overrun)\n", dropped);
    }

    // Check for silent background encoder failure
    extern _Atomic bool encoder_disk_error;
    if (atomic_exchange_explicit(&encoder_disk_error, false, memory_order_acquire)) {
        if (btn_stop && gtk_widget_is_sensitive(btn_stop)) {
            g_signal_emit_by_name(btn_stop, "clicked");
            gtk_label_set_markup(GTK_LABEL(lbl_status), "<span foreground='#ff4444'><b>Status: RECORDING ABORTED (Disk Full / I/O Error)</b></span>");
        }
    }

    // 2. Track state changes for the massive waveform canvases independently
    static size_t last_pos_bt = SIZE_MAX;
    static size_t last_pos_input = SIZE_MAX;
    static void* last_buf_bt = NULL;
    static void* last_buf_input = NULL;
    static void* last_tick_layers[MAX_TRACKS] = {NULL};

    size_t current_pos = atomic_load_explicit(&playback_pos, memory_order_acquire);
    bool is_active = atomic_load_explicit(&engine_is_playing, memory_order_acquire) ||
    atomic_load_explicit(&engine_is_recording, memory_order_acquire);

    bool layers_swapped = false;

    // Update dynamic track time label
    extern void refresh_track_time_display(void);
    refresh_track_time_display();

    for (int i = 0; i < MAX_TRACKS; i++) {
        if (master_tracks[i].active_buffer != last_tick_layers[i]) {
            layers_swapped = true;
            last_tick_layers[i] = master_tracks[i].active_buffer;
        }
    }

    // PAGE TURN LOGIC (Only execute once per tick on the top canvas)
    if (backing_track_frames > 0 && widget == waveform_area_bt) {
        int width = gtk_widget_get_width(waveform_area_bt);
        double virtual_width = width * zoom_multiplier;

        // Dynamically update bounds if the window resized
        if (gtk_adjustment_get_upper(waveform_adj) != virtual_width) {
            gtk_adjustment_set_upper(waveform_adj, virtual_width);
            gtk_adjustment_set_page_size(waveform_adj, width);
            // Lock scrolling to 50 physical pixels per wheel tick instead of 10% of the view
            gtk_adjustment_set_step_increment(waveform_adj, 50.0);
            gtk_adjustment_set_page_increment(waveform_adj, width * 0.9);
        }

        // Follow playhead if playing, recording, or if the user manually seeks while paused
        if (is_active || current_pos != last_pos_bt) {
            double playhead_virtual_x = ((double)current_pos / (double)backing_track_frames) * virtual_width;
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
        if (is_active || is_dragging || current_pos != last_pos_bt || pristine_bt_buf != last_buf_bt || layers_swapped) {
            gtk_widget_queue_draw(widget);
            last_pos_bt = current_pos;
            last_buf_bt = pristine_bt_buf;

            // Auto-stop logic
            if (is_active && backing_track_frames > 0 && current_pos >= backing_track_frames) {
                on_stop_clicked(NULL, NULL);
            }
        }
    } else if (widget == waveform_area_input) {
        if (is_active || is_dragging || current_pos != last_pos_input || pristine_bt_buf != last_buf_input || layers_swapped) {
            gtk_widget_queue_draw(widget);
            last_pos_input = current_pos;
            last_buf_input = pristine_bt_buf;
        }
    }

    return G_SOURCE_CONTINUE;
}

/**
 * @brief Factory function to create and bind a new VU meter drawing area.
 * @param peak_var_ptr Pointer to the atomic float supplying peak data.
 * @return Pointer to the newly created GtkWidget drawing area.
 */
GtkWidget* make_meter(gpointer peak_var_ptr) {
    GtkWidget *meter = gtk_drawing_area_new();
    gtk_widget_set_size_request(meter, -1, 12);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(meter), on_draw_vu_meter, peak_var_ptr, NULL);
    gtk_widget_add_tick_callback(meter, (GtkTickCallback)on_waveform_tick, NULL, NULL);
    return meter;
}

/**
 * @brief Forces the UI to discard its cached surfaces and float arrays, prompting a full rescan of the audio memory on the next tick.
 * @return void
 */
void invalidate_waveform_caches(void) {
    cached_input_width = 0;
    cached_bt_width = 0;
    cached_bt_ptr = NULL;    // Force surface rebuild even if memory address is reused
}

/**
* @brief Cairo draw callback for the Quad Cortex input waveform area.
* @param area The drawing area widget.
* @param cr The Cairo rendering context.
* @param width The width of the drawing area.
* @param height The height of the drawing area.
* @param user_data Optional user data.
* @return void
*/
void on_draw_input_waveform(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data) {
    (void)area; (void)user_data;

    bool is_recording = atomic_load_explicit(&engine_is_recording, memory_order_acquire);
    int rec_track = atomic_load_explicit(&current_recording_track, memory_order_relaxed);
    float *active_input_buf = master_tracks[rec_track].active_buffer;

    if (ui_state.is_loading_track || !active_input_buf || backing_track_frames == 0) {
        draw_waveform_grid(cr, width, height, 0);
        return;
    }

    int bt_width = gtk_widget_get_width(waveform_area_bt);
    double virtual_width = bt_width * zoom_multiplier;
    double scroll_x = gtk_adjustment_get_value(waveform_adj);
    double frames_per_pixel = (double)backing_track_frames / virtual_width;
    if (frames_per_pixel == 0) frames_per_pixel = 1.0;

    size_t current_pos = atomic_load_explicit(&playback_pos, memory_order_acquire);
    size_t target_scan = backing_track_frames;

    static double cached_input_scroll_x = -1.0;
    static double cached_input_zoom = -1.0;

    if (width != cached_input_width || height != cached_input_height ||
        scroll_x != cached_input_scroll_x || zoom_multiplier != cached_input_zoom) {

        if (surface_input_grid) {
            cairo_surface_destroy(surface_input_grid);
            surface_input_grid = NULL;
        }
        surface_input_grid = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    cairo_t *surface_cr = cairo_create(surface_input_grid);
    draw_waveform_grid(surface_cr, width, height, backing_track_frames);
    cairo_destroy(surface_cr);

    cached_input_width = width;
    cached_input_height = height;
    cached_input_scroll_x = scroll_x;
    cached_input_zoom = zoom_multiplier;
        }

        if (surface_input_grid) {
            cairo_set_source_surface(cr, surface_input_grid, 0, 0);
            cairo_paint(cr);
        }

        if (is_recording) {
            if (frames_per_pixel >= 1.0) {
                memset(input_min_l, 0, sizeof(input_min_l));
                memset(input_max_l, 0, sizeof(input_max_l));
                memset(input_min_r, 0, sizeof(input_min_r));
                memset(input_max_r, 0, sizeof(input_max_r));

                for (int x = 0; x < width && x < MAX_UI_WIDTH; x++) {
                    size_t start_frame = (size_t)((scroll_x + x) * frames_per_pixel);
                    size_t end_frame = (size_t)((scroll_x + x + 1) * frames_per_pixel);

                    if (end_frame <= start_frame) end_frame = start_frame + 1;
                    if (start_frame >= target_scan) break;
                    if (end_frame > target_scan) end_frame = target_scan;

                    scan_waveform_extents(active_input_buf, start_frame, end_frame,
                                          &input_min_l[x], &input_max_l[x], &input_min_r[x], &input_max_r[x]);
                }

                cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
                set_track_color(cr, rec_track, 0.5);
                cairo_set_line_width(cr, 1.0);
                draw_envelope_waveform(cr, width, height, input_min_l, input_max_l, 1.0f, height / 4.0);
                draw_envelope_waveform(cr, width, height, input_min_r, input_max_r, 1.0f, 3.0 * height / 4.0);
            } else {
                cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
                set_track_color(cr, rec_track, 0.5);
                cairo_set_line_width(cr, 1.5);
                draw_sample_waveform(cr, width, height, active_input_buf, target_scan, scroll_x, frames_per_pixel, 1.0f, height / 4.0, 0);
                draw_sample_waveform(cr, width, height, active_input_buf, target_scan, scroll_x, frames_per_pixel, 1.0f, 3.0 * height / 4.0, 1);
            }
        }

        if (atomic_load_explicit(&loop_active, memory_order_acquire)) {
            size_t l_start = atomic_load_explicit(&loop_start_frame, memory_order_relaxed);
            size_t l_end = atomic_load_explicit(&loop_end_frame, memory_order_relaxed);

            size_t t_frames = atomic_load_explicit(&backing_track_frames, memory_order_acquire);
            double start_px = (((double)l_start / (double)t_frames) * virtual_width) - scroll_x;
            double end_px = (((double)l_end / (double)t_frames) * virtual_width) - scroll_x;

            cairo_set_source_rgba(cr, 0.3, 0.7, 1.0, 0.25);
            cairo_rectangle(cr, round(start_px), 0, round(end_px) - round(start_px), height);
            cairo_fill(cr);
        } else if (is_dragging && drag_start_fraction >= 0.0) {
            double start_px = (drag_start_fraction * virtual_width) - scroll_x;
            double end_px = (current_drag_fraction * virtual_width) - scroll_x;
            if (start_px > end_px) { double tmp = start_px; start_px = end_px; end_px = tmp; }
            cairo_set_source_rgba(cr, 0.3, 0.7, 1.0, 0.25);
            cairo_rectangle(cr, round(start_px), 0, round(end_px) - round(start_px), height);
            cairo_fill(cr);
        }

        draw_playhead(cr, width, height, current_pos, backing_track_frames);
}

// --- LOOPER DRAG GESTURES --

void clear_waveform_anchor(void) {
    clear_loop_points();
}

void on_waveform_click_pressed(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data) {
    (void)n_press; (void)y; (void)user_data;
    if (atomic_load_explicit(&engine_is_recording, memory_order_acquire)) return;
    if (backing_track_frames == 0) return;

    GdkModifierType state = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));

    // GUARD: Abort click processing if Ctrl is held, reserving it exclusively for panning
    if (state & GDK_CONTROL_MASK) {
        gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_DENIED);
        return;
    }

    int width = gtk_widget_get_width(waveform_area_bt);
    if (width <= 0) return;

    double virtual_width = width * zoom_multiplier;
    double scroll_x = gtk_adjustment_get_value(waveform_adj);
    double click_fraction = (x + scroll_x) / virtual_width;

    if (click_fraction < 0.0) click_fraction = 0.0;
    if (click_fraction > 1.0) click_fraction = 1.0;

    size_t total_frames = atomic_load_explicit(&backing_track_frames, memory_order_acquire);

    if ((state & GDK_SHIFT_MASK) == 0) {
        // Normal Click: Clear loop and move playhead
        clear_loop_points();
        seek_backing_track(click_fraction);
    } else {
        // Shift + Click: Create selection between current playhead and click location
        size_t current_pos = atomic_load_explicit(&playback_pos, memory_order_acquire);
        double playhead_fraction = (double)current_pos / (double)total_frames;

        // Failsafe: Prevent mathematically identical start/end loops
        if (playhead_fraction == click_fraction) click_fraction += 0.000001;

        set_loop_points(playhead_fraction, click_fraction);
        seek_backing_track(fmin(playhead_fraction, click_fraction));
    }

    last_seek_time = g_get_monotonic_time();
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
    (void)start_y; (void)user_data;
    if (atomic_load_explicit(&engine_is_recording, memory_order_acquire)) return;
    if (backing_track_frames == 0) return;

    int width = gtk_widget_get_width(waveform_area_bt);
    if (width <= 0) return;

    GdkModifierType state = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));

    // Abort drag if Shift is held to allow click gesture to exclusively handle Anchor & Extend
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

    if (drag_start_fraction < 0.0) drag_start_fraction = 0.0;
    if (drag_start_fraction > 1.0) drag_start_fraction = 1.0;

    size_t total_frames = atomic_load_explicit(&backing_track_frames, memory_order_acquire);
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
    (void)offset_y; (void)user_data;
    if (backing_track_frames == 0) return;

    if (is_panning) {
        double new_scroll = drag_start_scroll_x - (offset_x * 1.5);
        double upper = gtk_adjustment_get_upper(waveform_adj);
        double page = gtk_adjustment_get_page_size(waveform_adj);
        if (new_scroll < 0) new_scroll = 0;
        if (new_scroll > upper - page) new_scroll = upper - page;
        gtk_adjustment_set_value(waveform_adj, new_scroll);
        return;
    }

    if (!is_dragging) return;

    int width = gtk_widget_get_width(waveform_area_bt);
    if (width <= 0) return;

    double start_x, start_y;
    gtk_gesture_drag_get_start_point(gesture, &start_x, &start_y);

    double virtual_width = width * zoom_multiplier;
    double scroll_x = gtk_adjustment_get_value(waveform_adj);
    current_drag_fraction = (start_x + offset_x + scroll_x) / virtual_width;

    if (current_drag_fraction < 0.0) current_drag_fraction = 0.0;
    if (current_drag_fraction > 1.0) current_drag_fraction = 1.0;

    size_t total_frames = atomic_load_explicit(&backing_track_frames, memory_order_acquire);
    if (total_frames > 0) {
        size_t frame = (size_t)(current_drag_fraction * total_frames);
        current_drag_fraction = (double)frame / (double)total_frames;
    }

    gtk_widget_queue_draw(waveform_area_bt);
    gtk_widget_queue_draw(waveform_area_input);
}

/**
 * @brief Gesture callback triggered when a multitrack drag operation completes, applying loop points.
 * @param gesture The drag gesture object.
 * @param offset_x The total X offset from start.
 * @param offset_y The total Y offset from start.
 * @param user_data Optional user data.
 * @return void
 */
void on_drag_end(GtkGestureDrag *gesture, double offset_x, double offset_y, gpointer user_data) {
    (void)offset_y; (void)user_data;

    if (is_panning) {
        is_panning = false;
        return;
    }

    if (atomic_load_explicit(&engine_is_recording, memory_order_acquire)) return;
    if (backing_track_frames == 0 || drag_start_fraction < 0.0) return;

    int width = gtk_widget_get_width(waveform_area_bt);
    if (width <= 0) return;

    double start_x, start_y;
    gtk_gesture_drag_get_start_point(gesture, &start_x, &start_y);

    double virtual_width = width * zoom_multiplier;
    double scroll_x = gtk_adjustment_get_value(waveform_adj);
    double end_fraction = (start_x + offset_x + scroll_x) / virtual_width;

    if (end_fraction < 0.0) end_fraction = 0.0;
    if (end_fraction > 1.0) end_fraction = 1.0;

    is_dragging = false;

    // Use physical screen pixels to differentiate a click from a drag.
    // Increasing the threshold to 10.0px prevents accidental micro-loops from sloppy clicks.
    // Pure clicks are naturally handled by on_waveform_click_pressed on mouse-down.
    if (fabs(offset_x) >= 10.0) {
        set_loop_points(drag_start_fraction, end_fraction);
        seek_backing_track(fmin(drag_start_fraction, end_fraction));
        last_seek_time = g_get_monotonic_time();
    }

    drag_start_fraction = -1.0;
    gtk_widget_queue_draw(waveform_area_input); // Ensure final boundary snaps to bottom canvas
}

gboolean on_waveform_scroll(GtkEventControllerScroll *controller, double dx, double dy, gpointer user_data) {
    (void)user_data;
    GdkModifierType state = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(controller));

    if (state & GDK_SHIFT_MASK) {
        return TRUE;
    }

    if (state & GDK_CONTROL_MASK) {
        double delta = (dy != 0.0) ? dy : dx;

        if (delta < 0) {
            zoom_multiplier *= 1.10;
        } else {
            zoom_multiplier /= 1.10;
        }

        if (zoom_multiplier < 1.0) zoom_multiplier = 1.0;
        if (zoom_multiplier > 5000000.0) zoom_multiplier = 5000000.0;

        size_t current_pos = atomic_load_explicit(&playback_pos, memory_order_acquire);
        size_t total_frames = atomic_load_explicit(&backing_track_frames, memory_order_acquire);

        extern GtkWidget *waveform_area_bt;
        if (total_frames > 0 && waveform_area_bt) {
            int width = gtk_widget_get_width(waveform_area_bt);
            double virtual_width = width * zoom_multiplier;
            double playhead_virtual_x = ((double)current_pos / (double)total_frames) * virtual_width;

            gtk_adjustment_set_upper(waveform_adj, virtual_width);
            gtk_adjustment_set_page_size(waveform_adj, width);

            double new_scroll = playhead_virtual_x - (width / 2.0);
            if (new_scroll < 0) new_scroll = 0;
            if (new_scroll > virtual_width - width) new_scroll = virtual_width - width;

            gtk_adjustment_set_value(waveform_adj, new_scroll);
        }

        extern void invalidate_waveform_caches(void);
        extern GtkWidget *waveform_area_input;

        invalidate_waveform_caches();
        if (waveform_area_bt) gtk_widget_queue_draw(waveform_area_bt);
        if (waveform_area_input) gtk_widget_queue_draw(waveform_area_input);

        return TRUE;
    }

    return FALSE;
}
