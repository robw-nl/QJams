#ifndef UI_GLOBALS_H
#define UI_GLOBALS_H

#include "config.h"
#include "video_ringbuffer.h"
#include <gtk/gtk.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    QJamsConfig config;
    char config_path[1024];
    char playlist_path[1024];
    char selected_track_path[512];
    char workspace_status[512];
    bool is_existing_session;
} QJamsUIState;

extern QJamsUIState ui_state;

// Shared UI Widgets
extern GtkWidget *lbl_input_device;
extern GtkWidget *input_gain_spinner;
extern GtkWidget *lbl_track;
extern GtkWidget *lbl_status;
extern GtkWidget *lbl_hw_toast;
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

// Multitrack Controls
extern GtkWidget *lbl_multitrack_status;

// Dashboard & Command Post Widgets
extern GtkWidget *btn_load;
extern GtkWidget *btn_load_session;
extern GtkWidget *btn_load_playlist;
extern GtkWidget *btn_save_session;
extern GtkWidget *btn_save_playlist;
extern GtkWidget *btn_save_mux;
extern GtkWidget *btn_settings;
extern GtkWidget *btn_prev;
extern GtkWidget *btn_stop;
extern GtkWidget *btn_next;
extern GtkWidget *btn_speed;
extern GtkWidget *btn_loop_countdown;
extern GtkWidget *btn_overlay_clip;
extern GtkWidget *btn_reset;
extern GtkWidget *main_spinner;

void update_global_ui_locks(bool is_playing, bool is_recording);

// Muxing Paths & State
extern char current_raw_path[1024];
extern char stashed_raw_path[1024];

/**
 * @brief Checks whether a live temporary MKV video recording currently exists on disk.
 * @return true if current_raw_path is non-empty and accessible, false otherwise.
 */
static inline bool has_active_live_video(void) {
    return (strlen(current_raw_path) > 0 && access(current_raw_path, F_OK) == 0);
}
extern GPid active_muxer_pid;

// Unified Video Arm State
extern GtkWidget *btn_record_video;
extern GtkWidget *video_preview_frame;
extern _Atomic bool video_is_armed;
extern SPSC_Preview_Queue preview_queue;

extern _Atomic bool is_video_expanded;
extern GtkWidget *btn_video_size;

extern atomic_size_t video_start_frame;
extern atomic_size_t live_video_start_frame;
extern atomic_size_t video_end_frame;
extern int video_track_index;

void abort_active_video_preview(bool stash);
void restore_stashed_video_preview(void);
void clear_video_stash_async(void);

/**
 * @brief Toggles the main window's cursor between the busy wait spinner and default pointer.
 * @param waiting true to display the wait cursor, false to restore the default cursor.
 */
void set_app_waiting_state(bool waiting);

/**
 * @brief Stops active playback or recording and commits any live recording take.
 * @param button The stop button widget (or NULL when invoked programmatically).
 * @param user_data Optional callback data.
 */
void on_stop_clicked(GtkButton *button, gpointer user_data);

/**
 * @brief Refreshes multitrack status labels, zoom length label, and waveform caches.
 */
void trigger_global_ui_refresh(void);

/**
 * @brief Removes the background crash-recovery autosave archive from disk.
 */
void purge_autosave_file(void);

#endif
