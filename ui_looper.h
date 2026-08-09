#ifndef UI_LOOPER_H
#define UI_LOOPER_H

#include <gtk/gtk.h>

GtkWidget* create_looper_layers_widget(void);
void refresh_looper_layers_ui(void);
void looper_start_rename(void);
void get_looper_layer_name(int idx, char *out_name);
void set_looper_layer_name(int idx, const char *name);
void update_looper_status_ui(void);
void reset_looper_ui_states(void);
void on_blank_canvas_toggled(GObject *gobject, GParamSpec *pspec, gpointer user_data);
void ui_looper_init(GtkBuilder *builder);

#endif // UI_LOOPER_H
