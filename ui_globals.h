#ifndef UI_GLOBALS_H
#define UI_GLOBALS_H

#include <gtk/gtk.h>
#include <stdbool.h>
#include "config.h"

typedef struct {
    QJamsConfig config;
    char config_path[1024];
    char playlist_path[1024];
    char selected_track_path[512];
    bool is_loading_track;
    int freestyle_duration_min; // Default should be 15
    int multitrack_duration_min;    // Default should be 5
    bool session_is_dirty;      // Tracks if canvas has been recorded to
} QJamsUIState;

extern QJamsUIState ui_state;

// Shared UI Widgets
extern GtkWidget *lbl_input_device;
extern GtkWidget *input_gain_spinner;
extern GtkWidget *lbl_track;
extern GtkWidget *lbl_status;
extern GtkWidget *btn_play;
extern GtkWidget *btn_record;
extern GtkWidget *btn_playlist_toggle;
extern GtkWidget *waveform_area_bt;
extern GtkWidget *waveform_area_input;
extern GtkWidget *btn_zoom;
extern GtkWidget *waveform_scrollbar;
extern GtkAdjustment *waveform_adj;
extern double zoom_multiplier;

// Layout Containers
extern GtkWidget *left_column_box;
extern GtkWidget *right_vbox;
extern GtkWidget *command_post_box;
extern GtkWidget *settings_dialog;

// Looper Controls
extern GtkWidget *mode_stack;
extern GtkWidget *btn_multitrack_mode;
extern GtkWidget *lbl_multitrack_status;
extern GtkWidget *btn_multitrack_undo;
extern GtkWidget *btn_multitrack_next;

// Dashboard & Command Post Widgets
extern GtkWidget *btn_load;
extern GtkWidget *btn_save_mux;
extern GtkWidget *btn_save_session;
extern GtkWidget *btn_settings;
extern GtkWidget *btn_prev;
extern GtkWidget *btn_stop;
extern GtkWidget *btn_next;
extern GtkWidget *btn_speed;
extern GtkWidget *btn_reset;
extern GtkWidget *lbl_roadmap;
extern GtkWidget *main_spinner; // The background task indicator

// Muxing Paths & State
extern char final_save_path[1024];
extern char current_raw_path[1024];
extern GPid active_muxer_pid;
extern _Atomic bool is_mkv_mode;

void trigger_track_load(void);
void on_stop_clicked(GtkButton *button, gpointer user_data);

#endif // UI_GLOBALS_H
