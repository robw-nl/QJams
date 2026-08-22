#ifndef UI_DASHBOARD_H
#define UI_DASHBOARD_H

#include <gtk/gtk.h>

void ui_dashboard_init(GtkBuilder *b_dash, GtkBuilder *b_wave);
void update_zoom_button_label_to_length(void);
void prepare_engine_for_new_track(void);
void adjust_blank_canvas_for_mode(bool is_looper);

// Exported for global keypress shortcuts in gui.c
void on_play_clicked(GtkButton *button, gpointer user_data);
void on_speed_clicked(GtkButton *button, gpointer user_data);
void on_start_clicked(GtkButton *button, gpointer user_data);
void on_global_reset_clicked(GtkButton *button, gpointer user_data);

void update_dashboard_cycler_ui(void);

#endif // UI_DASHBOARD_H
