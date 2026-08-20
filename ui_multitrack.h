#ifndef UI_LOOPER_H
#define UI_LOOPER_H

#include <gtk/gtk.h>

GtkWidget* create_multitrack_tracks_widget(void);
void refresh_multitrack_tracks_ui(void);
void multitrack_start_rename(void);
void get_multitrack_track_name(int idx, char *out_name);
void set_multitrack_track_name(int idx, const char *name);
void update_multitrack_status_ui(void);
void reset_multitrack_ui_states(void);
void on_blank_canvas_toggled(GObject *gobject, GParamSpec *pspec, gpointer user_data);
void ui_multitrack_init(GtkBuilder *b_multi, GtkBuilder *b_stack);

void multitrack_import_file_async(const char *filepath, int track_idx); // ADD THIS

#endif // UI_LOOPER_H
