#ifndef UI_COMMAND_POST_H
#define UI_COMMAND_POST_H

#include <gtk/gtk.h>

void ui_command_post_init(GtkBuilder *b_cmd, GtkWindow *window);
void on_select_track_clicked(GtkButton *button, gpointer window);
void on_save_mux_clicked(GtkButton *button, gpointer user_data);

gboolean idle_autosave_tick(gpointer user_data);
gboolean check_for_crash_recovery(gpointer user_data);
void command_post_load_session(const char *path);

#endif // UI_COMMAND_POST_H
