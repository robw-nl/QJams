#ifndef UI_MULTITRACK_H
#define UI_MULTITRACK_H

#include <gtk/gtk.h>

GtkWidget *create_multitrack_tracks_widget(void);
void refresh_multitrack_tracks_ui(void);
void multitrack_start_rename(void);
void get_multitrack_track_name(int idx, char *out_name);
void set_multitrack_track_name(int idx, const char *name);
void update_multitrack_status_ui(void);
void reset_multitrack_ui_states(void);
void multitrack_select_all_active(void);
void ui_multitrack_init(GtkBuilder *b_multi);

void multitrack_import_file_async(const char *filepath, int track_idx);
void multitrack_import_file_at_frame_async(const char *filepath, int track_idx, size_t start_frame);
void multitrack_import_jukebox_async(const char *filepath, int track_idx);
gboolean handle_track_drop_payload(GtkDropTarget *target, const GValue *value, int start_track_idx,
                                   double drop_x);

#endif // UI_MULTITRACK_H
