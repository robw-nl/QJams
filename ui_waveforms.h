#ifndef UI_WAVEFORMS_H
#define UI_WAVEFORMS_H

#include <gtk/gtk.h>

/**
 * @brief Cairo draw callback for the backing track waveform area.
 * @param area The drawing area widget.
 * @param cr The Cairo rendering context.
 * @param width The width of the drawing area.
 * @param height The height of the drawing area.
 * @param user_data Optional user data.
 * @return void
 */
void on_draw_bt_waveform(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data);
/**
 * @brief Cairo draw callback for the Quad Cortex input waveform area.
 * @param area The drawing area widget.
 * @param cr The Cairo rendering context.
 * @param width The width of the drawing area.
 * @param height The height of the drawing area.
 * @param user_data Optional user data.
 * @return void
 */

void on_draw_input_waveform(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data);

gboolean on_waveform_scroll(GtkEventControllerScroll *controller, double dx, double dy, gpointer user_data);

void on_draw_input_waveform(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data);
/**
 * @brief Cairo draw callback for a generic horizontal VU meter.
 * @param area The drawing area widget.
 * @param cr The Cairo rendering context.
 * @param width The width of the meter.
 * @param height The height of the meter.
 * @param user_data Pointer to the atomic float providing peak amplitude data.
 * @return void
 */
void on_draw_vu_meter(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data);
/**
 * @brief Cairo draw callback to render the layer identification color dot.
 * @param area The drawing area widget.
 * @param cr The Cairo rendering context.
 * @param width The width of the dot area.
 * @param height The height of the dot area.
 * @param user_data The layer index cast to a gpointer.
 * @return void
 */
void on_draw_track_color_dot(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data);
/**
 * @brief UI tick callback to schedule redraws for dynamic waveform and meter widgets.
 * @param widget The widget to queue for redraw.
 * @param frame_clock The GDK frame clock.
 * @param user_data Optional user data.
 * @return gboolean G_SOURCE_CONTINUE
 */
gboolean on_waveform_tick(GtkWidget *widget, GdkFrameClock *frame_clock, gpointer user_data);
/**
 * @brief Factory function to create and bind a new VU meter drawing area.
 * @param peak_var_ptr Pointer to the atomic float supplying peak data.
 * @return Pointer to the newly created GtkWidget drawing area.
 */
GtkWidget* make_meter(gpointer peak_var_ptr);

/**
 * @brief Forces the UI to discard its cached surfaces and float arrays, prompting a full rescan of the audio memory on the next tick.
 * @return void
 */
void invalidate_waveform_caches(void);

/**
 * @brief Clears the active selection anchor if one is pending.
 */
void clear_waveform_anchor(void);

/**
 * @brief Click gesture callback for the waveform area to handle anchor-and-extend selections.
 */
void on_waveform_click_pressed(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data);

/**
 * @brief Gesture callback triggered when a looper drag starts.
 * @param gesture The drag gesture object.
 * @param start_x The starting X coordinate.
 * @param start_y The starting Y coordinate.
 * @param user_data Optional user data.
 * @return void
 */
void on_drag_begin(GtkGestureDrag *gesture, double start_x, double start_y, gpointer user_data);
/**
 * @brief Gesture callback triggered during a looper drag operation.
 * @param gesture The drag gesture object.
 * @param offset_x The current X offset from start.
 * @param offset_y The current Y offset from start.
 * @param user_data Optional user data.
 * @return void
 */
void on_drag_update(GtkGestureDrag *gesture, double offset_x, double offset_y, gpointer user_data);
/**
 * @brief Gesture callback triggered when a looper drag operation completes, applying loop points.
 * @param gesture The drag gesture object.
 * @param offset_x The total X offset from start.
 * @param offset_y The total Y offset from start.
 * @param user_data Optional user data.
 * @return void
 */
void on_drag_end(GtkGestureDrag *gesture, double offset_x, double offset_y, gpointer user_data);

#endif // UI_WAVEFORMS_H
