#ifndef UI_DASHBOARD_H
#define UI_DASHBOARD_H

#include <gtk/gtk.h>

void ui_dashboard_init(GtkBuilder *b_dash, GtkBuilder *b_wave);
void update_zoom_button_label_to_length(void);
void prepare_engine_for_new_track(void);
void adjust_blank_canvas(void);
void execute_global_canvas_reset(void);

void on_play_clicked(GtkButton *button, gpointer user_data);
void on_speed_clicked(GtkButton *button, gpointer user_data);
void on_start_clicked(GtkButton *button, gpointer user_data);
void on_global_reset_clicked(GtkButton *button, gpointer user_data);

void update_dashboard_pref_ui(void);
void refresh_track_time_display(void);
void ui_dashboard_shutdown(void);

// countdown timer for inside canvas loop
bool is_countdown_active(void);

#endif // UI_DASHBOARD_H
