#ifndef UI_COMMAND_POST_H
#define UI_COMMAND_POST_H

#include <gtk/gtk.h>

void ui_command_post_init(GtkBuilder *b_cmd, GtkBuilder *b_stack, GtkWindow *window);
void on_select_track_clicked(GtkButton *button, gpointer window);
void on_save_mux_clicked(GtkButton *button, gpointer user_data);

#endif // UI_COMMAND_POST_H
