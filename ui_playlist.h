#ifndef UI_PLAYLIST_H
#define UI_PLAYLIST_H

#include <gtk/gtk.h>
#include <stdbool.h>

// --- EXPOSED TRACK LIST GOBJECT ---
#define QJ_TYPE_TRACK (qj_track_get_type())
G_DECLARE_FINAL_TYPE(QjTrack, qj_track, QJ, TRACK, GObject)

struct _QjTrack {
    GObject parent_instance;
    char *filepath;
    char *display_name;
    bool is_qjams_native;
};

/**
 * @brief Creates and initializes the main GTK playlist widget structure.
 * @return A pointer to the created GtkWidget (scrolled window containing the list).
 */
GtkWidget* create_playlist_widget(void);

/**
 * @brief Loads track entries into the playlist from a given .m3u file.
 * @param filepath The absolute path to the playlist file.
 * @return void
 */
void load_playlist_from_file(const char *filepath);

/**
 * @brief Saves the current playlist order and tracks to the default .m3u file.
 * @return void
 */
void save_current_playlist(void);

/**
 * @brief Updates the UI state of the playlist toggle button based on the currently selected track.
 * @return void
 */
void update_playlist_toggle_state(void);

/**
 * @brief Removes a track from the playlist and saves the updated list to disk.
 * @param filepath The absolute path to the track.
 * @return void
 */
void remove_track_from_playlist(const char* filepath);

/**
 * @brief Callback that adds or removes the currently selected track from the active playlist.
 * @param button The GTK button triggering the callback.
 * @param user_data Optional user data passed to the callback.
 * @return void
 */
void on_playlist_toggle_clicked(GtkButton *button, gpointer user_data);

/**
 * @brief Cycles to the next or previous track in the playlist.
 * @param direction Direction to cycle (-1 for previous, 1 for next).
 * @return true if a new track was loaded, false otherwise.
 */
bool cycle_playlist_track(int direction);

void remove_active_track_from_playlist(const char *path);

void activate_playlist_preset(int slot);

#endif // UI_PLAYLIST_H
