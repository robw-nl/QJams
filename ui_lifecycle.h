#ifndef UI_LIFECYCLE_H
#define UI_LIFECYCLE_H

#include <gtk/gtk.h>
#include <stdbool.h>

extern bool pending_quit_after_save;
gboolean on_window_close(GtkWindow *window, gpointer user_data);

#endif // UI_LIFECYCLE_H
