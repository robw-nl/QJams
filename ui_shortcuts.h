#ifndef UI_SHORTCUTS_H
#define UI_SHORTCUTS_H

#include <gtk/gtk.h>
#include <stdbool.h>

gboolean on_window_key_pressed(GtkEventControllerKey *controller, guint keyval, guint keycode,
                               GdkModifierType state, gpointer user_data);

#endif // UI_SHORTCUTS_H
