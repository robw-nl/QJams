#ifndef UI_TRANSPORT_H
#define UI_TRANSPORT_H

#include <gtk/gtk.h>
#include <stdbool.h>

void update_zoom_button_label_to_length(void);
// void prepare_engine_for_new_track(void);
void adjust_blank_canvas(void);
void execute_global_canvas_reset(const char *next_track_path);
bool is_countdown_active(void);

void on_play_clicked(GtkButton *button, gpointer user_data);
void on_speed_clicked(GtkButton *button, gpointer user_data);
void on_loop_countdown_toggled(GtkToggleButton *btn, gpointer user_data);
void on_overlay_clip_toggled(GtkToggleButton *btn, gpointer user_data);
void step_playback_speed(float delta);
void sync_speed_ui_label(float speed);
void on_start_clicked(GtkButton *button, gpointer user_data);
void on_global_reset_clicked(GtkButton *button, gpointer user_data);
void on_crop_clicked(GtkButton *button, gpointer user_data);
void on_prev_track_clicked(GtkButton *button, gpointer user_data);
void on_next_track_clicked(GtkButton *button, gpointer user_data);
void on_record_audio_clicked(GtkButton *button, gpointer user_data);
void on_record_video_clicked(GtkButton *button, gpointer user_data);

gboolean transport_monitor_tick(gpointer user_data);

#endif // UI_TRANSPORT_H
