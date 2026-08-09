#ifndef UI_SETTINGS_H
#define UI_SETTINGS_H

#include <gtk/gtk.h>

/**
 * @brief Handles the settings button click, opening the hardware preferences dialog.
 * @param button The GTK button triggering the callback.
 * @param user_data Pointer to the main window to set as the transient parent.
 * @return void
 */
void on_settings_clicked(GtkButton *button, gpointer user_data);

#endif // UI_SETTINGS_H
