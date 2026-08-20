#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"

/**
 * @brief Loads the QJams configuration from the specified file, returning defaults if not found.
 * Utilizes thread-safe string tokenization and strictly bounded string copies to prevent memory bleeds.
 * @param filepath The path to the configuration file.
 * @return A populated QJamsConfig structure.
 */
QJamsConfig load_qjams_config(const char* filepath) {
    QJamsConfig config = {
        .input_gain_multiplier = 1.0f,
        .bt_gain_multiplier = 1.0f,
        .playback_target = "system:playback",
        .last_track_dir = "/home",
        .recordings_dir = "/home",
        .last_playlist_path = "",
        .window_width = 1400,
        .window_height = 1024,
        .video_device = "/dev/video0",
        .audio_device = "", // Blank fallback allows scanner to auto-select first available hardware
        .multitrack_blank_canvas = 0,
        .multitrack_mode_active = 0,
        .freestyle_duration_min = 15,
        .multitrack_duration_min = 5,
        .export_format = 0,
        .playlist_slots = {"", "", "", "", "", "", "", "", "", ""}, // 10 Slots
        .num_audio_profiles = 0
    };

    FILE *file = fopen(filepath, "r");
    if (!file) return config;

    char line[512];
    while (fgets(line, sizeof(line), file)) {
        line[strcspn(line, "\n")] = 0; // Strip newline

        // Thread-safe state tracker for re-entrant tokenization
        char *saveptr = NULL;
        char *key = strtok_r(line, "=", &saveptr);
        char *value = strtok_r(NULL, "", &saveptr);
        if (!key || !value) continue;

        if (strcmp(key, "BT_GAIN_MULTIPLIER") == 0) {
            config.bt_gain_multiplier = strtof(value, NULL);
        } else if (strcmp(key, "PLAYBACK_TARGET") == 0) {
            strncpy(config.playback_target, value, sizeof(config.playback_target) - 1);
            config.playback_target[sizeof(config.playback_target) - 1] = '\0';
        } else if (strcmp(key, "LAST_TRACK_DIR") == 0) {
            strncpy(config.last_track_dir, value, sizeof(config.last_track_dir) - 1);
            config.last_track_dir[sizeof(config.last_track_dir) - 1] = '\0';
        } else if (strcmp(key, "RECORDINGS_DIR") == 0) {
            strncpy(config.recordings_dir, value, sizeof(config.recordings_dir) - 1);
            config.recordings_dir[sizeof(config.recordings_dir) - 1] = '\0';
        } else if (strcmp(key, "LAST_PLAYLIST_PATH") == 0) {
            strncpy(config.last_playlist_path, value, sizeof(config.last_playlist_path) - 1);
            config.last_playlist_path[sizeof(config.last_playlist_path) - 1] = '\0';
        } else if (strcmp(key, "WINDOW_WIDTH") == 0) {
            config.window_width = atoi(value);
        } else if (strcmp(key, "WINDOW_HEIGHT") == 0) {
            config.window_height = atoi(value);
        } else if (strcmp(key, "VIDEO_DEVICE") == 0) {
            strncpy(config.video_device, value, sizeof(config.video_device) - 1);
            config.video_device[sizeof(config.video_device) - 1] = '\0';
        } else if (strcmp(key, "AUDIO_DEVICE") == 0) {
            strncpy(config.audio_device, value, sizeof(config.audio_device) - 1);
            config.audio_device[sizeof(config.audio_device) - 1] = '\0';
        } else if (strcmp(key, "MULTITRACK_BLANK_CANVAS") == 0) {
            config.multitrack_blank_canvas = atoi(value);
        } else if (strcmp(key, "MULTITRACK_MODE_ACTIVE") == 0) {
            config.multitrack_mode_active = atoi(value);
        } else if (strcmp(key, "FREESTYLE_DURATION_MIN") == 0) {
            config.freestyle_duration_min = atoi(value);
        } else if (strcmp(key, "MULTITRACK_DURATION_MIN") == 0) {
            config.multitrack_duration_min = atoi(value);
        } else if (strcmp(key, "EXPORT_FORMAT") == 0) {
            config.export_format = atoi(value);
        } else if (strncmp(key, "PLAYLIST_SLOT_", 14) == 0) {
            int slot = atoi(key + 14) - 1;
            if (slot >= 0 && slot < 10) {
                strncpy(config.playlist_slots[slot], value, sizeof(config.playlist_slots[0]) - 1);
                config.playlist_slots[slot][sizeof(config.playlist_slots[0]) - 1] = '\0';
            }
        } else if (strncmp(key, "AUDIO_PROFILE_", 14) == 0) {
            if (config.num_audio_profiles < MAX_SAVED_DEVICES) {
                strncpy(config.audio_profiles[config.num_audio_profiles].device_name, key + 14, 127);
                config.audio_profiles[config.num_audio_profiles].device_name[127] = '\0';

                float gain = 1.0f;
                int is_pri = 0, is_fall = 0, in_cyc = 0;
                sscanf(value, "%f,%d,%d,%d", &gain, &is_pri, &is_fall, &in_cyc);

                config.audio_profiles[config.num_audio_profiles].gain_multiplier = gain;
                config.audio_profiles[config.num_audio_profiles].is_primary = is_pri;
                config.audio_profiles[config.num_audio_profiles].is_fallback = is_fall;
                config.audio_profiles[config.num_audio_profiles].in_cycler = in_cyc;

                config.num_audio_profiles++;
            }
        } else if (strncmp(key, "INPUT_GAIN_", 11) == 0) {
            // Legacy configuration migration
            if (strcmp(key, "INPUT_GAIN_MULTIPLIER") == 0) continue;

            if (config.num_audio_profiles < MAX_SAVED_DEVICES) {
                strncpy(config.audio_profiles[config.num_audio_profiles].device_name, key + 11, 127);
                config.audio_profiles[config.num_audio_profiles].device_name[127] = '\0';
                config.audio_profiles[config.num_audio_profiles].gain_multiplier = strtof(value, NULL);
                config.audio_profiles[config.num_audio_profiles].is_primary = 0;
                config.audio_profiles[config.num_audio_profiles].is_fallback = 0;
                config.audio_profiles[config.num_audio_profiles].in_cycler = 0;
                config.num_audio_profiles++;
            }
        }
    }
    fclose(file);

    // Set the active multiplier based on the discovered device profile
    config.input_gain_multiplier = 1.0f;
    for (int i = 0; i < config.num_audio_profiles; i++) {
        if (strcmp(config.audio_profiles[i].device_name, config.audio_device) == 0) {
            config.input_gain_multiplier = config.audio_profiles[i].gain_multiplier;
            break;
        }
    }

    return config;
}

/**
 * @brief Saves the current QJams configuration to the specified file.
 * @param filepath The path to the configuration file.
 * @param config Pointer to the QJamsConfig structure to save.
 * @return void
 */
void save_qjams_config(const char* filepath, const QJamsConfig* config) {
    FILE *file = fopen(filepath, "w");
    if (!file) {
        printf("Error: Could not open config file %s for writing.\n", filepath);
        return;
    }

    fprintf(file, "BT_GAIN_MULTIPLIER=%f\n", config->bt_gain_multiplier);
    fprintf(file, "PLAYBACK_TARGET=%s\n", config->playback_target);
    fprintf(file, "LAST_TRACK_DIR=%s\n", config->last_track_dir);
    fprintf(file, "RECORDINGS_DIR=%s\n", config->recordings_dir);
    fprintf(file, "LAST_PLAYLIST_PATH=%s\n", config->last_playlist_path);
    fprintf(file, "WINDOW_WIDTH=%d\n", config->window_width);
    fprintf(file, "WINDOW_HEIGHT=%d\n", config->window_height);
    fprintf(file, "VIDEO_DEVICE=%s\n", config->video_device);
    fprintf(file, "AUDIO_DEVICE=%s\n", config->audio_device);
    fprintf(file, "MULTITRACK_BLANK_CANVAS=%d\n", config->multitrack_blank_canvas);
    fprintf(file, "MULTITRACK_MODE_ACTIVE=%d\n", config->multitrack_mode_active);
    fprintf(file, "FREESTYLE_DURATION_MIN=%d\n", config->freestyle_duration_min);
    fprintf(file, "MULTITRACK_DURATION_MIN=%d\n", config->multitrack_duration_min);
    fprintf(file, "EXPORT_FORMAT=%d\n", config->export_format);

    for (int i = 0; i < 10; i++) {
        if (strlen(config->playlist_slots[i]) > 0) {
            fprintf(file, "PLAYLIST_SLOT_%d=%s\n", i + 1, config->playlist_slots[i]);
        }
    }

    for (int i = 0; i < config->num_audio_profiles; i++) {
        fprintf(file, "AUDIO_PROFILE_%s=%f,%d,%d,%d\n",
                config->audio_profiles[i].device_name,
                config->audio_profiles[i].gain_multiplier,
                config->audio_profiles[i].is_primary,
                config->audio_profiles[i].is_fallback,
                config->audio_profiles[i].in_cycler);
    }

    fclose(file);
}
