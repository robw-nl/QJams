#ifndef CONFIG_H
#define CONFIG_H

#define MAX_SAVED_DEVICES 32

/**
 * @brief Unified profile for an audio capture device, storing gain memory and routing roles.
 */
typedef struct {
    char device_name[128];
    float gain_multiplier;
    int is_primary;
    int is_fallback;
    int in_cycler;
} AudioDeviceProfile;

typedef struct {
    float input_gain_multiplier;
    float bt_gain_multiplier;
    char playback_target[128];
    char last_track_dir[512];
    char recordings_dir[512];
    char last_playlist_path[1024];
    int window_width;
    int window_height;
    char video_device[128];
    char audio_device[128];
    int multitrack_blank_canvas;
    int multitrack_mode_active; // Persist looper mode state
    int freestyle_duration_min;
    int multitrack_duration_min;
    int export_format; // 0 = FLAC, 1 = WAV, 2 = BOTH

    char playlist_slots[10][1024]; // Expanded to 10 Car Radio Presets

    // Dynamic Hardware Memory
    AudioDeviceProfile audio_profiles[MAX_SAVED_DEVICES];
    int num_audio_profiles;
} QJamsConfig;

/**
 * @brief Loads the QJams configuration from the specified file.
 * @param filepath The path to the configuration file.
 * @return A populated QJamsConfig structure.
 */
QJamsConfig load_qjams_config(const char* filepath);
/**
 * @brief Saves the current QJams configuration to the specified file.
 * @param filepath The path to the configuration file.
 * @param config Pointer to the QJamsConfig structure to save.
 * @return void
 */
void save_qjams_config(const char* filepath, const QJamsConfig* config);

#endif
