#ifndef CONFIG_H
#define CONFIG_H

#define MAX_SAVED_DEVICES 32

typedef struct {
    char device_name[128];
    float gain_multiplier;
} DeviceGainProfile;

typedef struct {
    float input_gain_multiplier;
    float bt_gain_multiplier;
    char playback_target[128];
    char last_track_dir[512];
    char recordings_dir[512];
    int window_width;
    int window_height;
    int video_preview_width;
    int video_preview_height;
    char video_device[128];
    char audio_device[128];
    int looper_blank_canvas;
    int looper_mode_active; // Persist looper mode state
    int freestyle_duration_min;
    int looper_duration_min;

    // Dynamic Hardware Memory
    DeviceGainProfile input_gains[MAX_SAVED_DEVICES];
    int num_saved_input_gains;
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
