#ifndef UI_WAVEFORMS_H
#define UI_WAVEFORMS_H

#include <gtk/gtk.h>

// Timeline hover state declarations
void on_canvas_hover_motion(gpointer controller, double x, double y, gpointer user_data);
void on_canvas_hover_enter(gpointer controller, double x, double y, gpointer user_data);
void on_canvas_hover_leave(gpointer controller, gpointer user_data);

/**
 * @brief Cairo draw callback for the backing track waveform area.
 * @param area The drawing area widget.
 * @param cr The Cairo rendering context.
 * @param width The width of the drawing area.
 * @param height The height of the drawing area.
 * @param user_data Optional user data.
 * @return void
 */
void on_draw_bt_waveform(GtkDrawingArea *area, cairo_t *cr, int width, int height,
                         gpointer user_data);

/**
 * @brief Scroll event handler for waveform areas. Handles zooming and destructive gain edits.
 * @param controller The scroll event controller.
 * @param dx X-axis delta.
 * @param dy Y-axis delta.
 * @param user_data Optional user data.
 * @return TRUE if the event was consumed, FALSE otherwise.
 */
gboolean on_waveform_scroll(GtkEventControllerScroll *controller, double dx, double dy,
                            gpointer user_data);

/**
 * @brief Cairo draw callback for the active capture device input waveform area.
 * @param area The drawing area widget.
 * @param cr The Cairo rendering context.
 * @param width The width of the drawing area.
 * @param height The height of the drawing area.
 * @param user_data Optional user data.
 * @return void
 */
void on_draw_input_waveform(GtkDrawingArea *area, cairo_t *cr, int width, int height,
                            gpointer user_data);

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
void on_draw_track_color_dot(GtkDrawingArea *area, cairo_t *cr, int width, int height,
                             gpointer user_data);

/**
 * @brief UI tick callback to schedule redraws for dynamic waveform and meter widgets.
 * @param widget The widget to queue for redraw.
 * @param frame_clock The GDK frame clock.
 * @param user_data Optional user data.
 * @return gboolean G_SOURCE_CONTINUE
 */
gboolean on_waveform_tick(GtkWidget *widget, GdkFrameClock *frame_clock, gpointer user_data);

/**
 * @brief Forces the UI to discard its cached surfaces and float arrays, prompting a full rescan of
 * the audio memory on the next tick.
 * @return void
 */
void invalidate_waveform_caches(void);

/**
 * @brief Click gesture callback for the waveform area to handle anchor-and-extend selections.
 * @param gesture The click gesture object.
 * @param n_press Number of presses.
 * @param x X coordinate.
 * @param y Y coordinate.
 * @param user_data Optional user data.
 * @return void
 */
void on_waveform_click_pressed(GtkGestureClick *gesture, int n_press, double x, double y,
                               gpointer user_data);

/**
 * @brief Right-click gesture callback for removing video takes via timeline red dot indicators.
 * @param gesture The click gesture object.
 * @param n_press Number of presses.
 * @param x X coordinate.
 * @param y Y coordinate.
 * @param user_data Optional user data.
 * @return void
 */
void on_waveform_right_click(GtkGestureClick *gesture, int n_press, double x, double y,
                             gpointer user_data);

/**
 * @brief Tooltip query callback for video take boundary dots on the waveform canvas.
 * @param widget The waveform widget.
 * @param x X coordinate.
 * @param y Y coordinate.
 * @param keyboard_mode Whether triggered by keyboard.
 * @param tooltip The GtkTooltip object.
 * @param user_data Optional user data.
 * @return TRUE if tooltip should be shown, FALSE otherwise.
 */
gboolean on_waveform_query_tooltip(GtkWidget *widget, gint x, gint y, gboolean keyboard_mode,
                                   GtkTooltip *tooltip, gpointer user_data);

/**
 * @brief Gesture callback triggered when a multitrack drag starts.
 * @param gesture The drag gesture object.
 * @param start_x The starting X coordinate.
 * @param start_y The starting Y coordinate.
 * @param user_data Optional user data.
 * @return void
 */
void on_drag_begin(GtkGestureDrag *gesture, double start_x, double start_y, gpointer user_data);

/**
 * @brief Gesture callback triggered during a multitrack drag operation.
 * @param gesture The drag gesture object.
 * @param offset_x The current X offset from start.
 * @param offset_y The current Y offset from start.
 * @param user_data Optional user data.
 * @return void
 */
void on_drag_update(GtkGestureDrag *gesture, double offset_x, double offset_y, gpointer user_data);

/**
 * @brief Gesture callback triggered when a multitrack drag operation completes, applying loop
 * points.
 * @param gesture The drag gesture object.
 * @param offset_x The total X offset from start.
 * @param offset_y The total Y offset from start.
 * @param user_data Optional user data.
 * @return void
 */
void on_drag_end(GtkGestureDrag *gesture, double offset_x, double offset_y, gpointer user_data);

#endif // UI_WAVEFORMS_H
