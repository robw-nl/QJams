#ifndef UI_DASHBOARD_H
#define UI_DASHBOARD_H

#include <gtk/gtk.h>
#include <stdatomic.h>
#include <stdbool.h>

void ui_dashboard_init(GtkBuilder *b_dash, GtkBuilder *b_wave);
void update_dashboard_pref_ui(void);
void refresh_track_time_display(void);
extern _Atomic bool is_loop_wrapping;
extern _Atomic bool suppress_video_overwrite;

gboolean on_jack_pipe_event(gint fd, GIOCondition condition, gpointer user_data);

#endif // UI_DASHBOARD_H
