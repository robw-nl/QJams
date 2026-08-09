#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"

/**
 * @brief Loads the QJams configuration from the specified file, returning defaults if not found.
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
        .window_width = 1150,
        .window_height = 750,
        .video_preview_width = 854,
        .video_preview_height = 480,
        .video_device = "/dev/video0",
        .audio_device = "", // NEW: Blank fallback allows scanner to auto-select first available hardware
        .looper_blank_canvas = 0,
        .looper_mode_active = 0,
        .freestyle_duration_min = 15,
        .looper_duration_min = 5,
        .num_saved_input_gains = 0
    };

    FILE *file = fopen(filepath, "r");
    if (!file) return config;

    char line[512];
    while (fgets(line, sizeof(line), file)) {
        line[strcspn(line, "\n")] = 0; // Strip newline

        char *key = strtok(line, "=");
        char *value = strtok(NULL, "");
        if (!key || !value) continue;

        if (strcmp(key, "BT_GAIN_MULTIPLIER") == 0) {
            config.bt_gain_multiplier = strtof(value, NULL);
        } else if (strcmp(key, "PLAYBACK_TARGET") == 0) {
            strncpy(config.playback_target, value, sizeof(config.playback_target) - 1);
        } else if (strcmp(key, "LAST_TRACK_DIR") == 0) {
            strncpy(config.last_track_dir, value, sizeof(config.last_track_dir) - 1);
        } else if (strcmp(key, "RECORDINGS_DIR") == 0) {
            strncpy(config.recordings_dir, value, sizeof(config.recordings_dir) - 1);
        } else if (strcmp(key, "WINDOW_WIDTH") == 0) {
            config.window_width = atoi(value);
        } else if (strcmp(key, "WINDOW_HEIGHT") == 0) {
            config.window_height = atoi(value);
        } else if (strcmp(key, "VIDEO_PREVIEW_WIDTH") == 0) {
            config.video_preview_width = atoi(value);
        } else if (strcmp(key, "VIDEO_PREVIEW_HEIGHT") == 0) {
            config.video_preview_height = atoi(value);
        } else if (strcmp(key, "VIDEO_DEVICE") == 0) {
            strncpy(config.video_device, value, sizeof(config.video_device) - 1);
        } else if (strcmp(key, "AUDIO_DEVICE") == 0) {
            strncpy(config.audio_device, value, sizeof(config.audio_device) - 1);
        } else if (strcmp(key, "LOOPER_BLANK_CANVAS") == 0) {
            config.looper_blank_canvas = atoi(value);
        } else if (strcmp(key, "LOOPER_MODE_ACTIVE") == 0) {
            config.looper_mode_active = atoi(value);
        } else if (strcmp(key, "FREESTYLE_DURATION_MIN") == 0) {
            config.freestyle_duration_min = atoi(value);
        } else if (strcmp(key, "LOOPER_DURATION_MIN") == 0) {
            config.looper_duration_min = atoi(value);
        } else if (strncmp(key, "INPUT_GAIN_", strlen("INPUT_GAIN_")) == 0) {
            // Ignore legacy standalone multiplier key to prevent phantom device creation
            if (strcmp(key, "INPUT_GAIN_MULTIPLIER") == 0) continue;

            if (config.num_saved_input_gains < MAX_SAVED_DEVICES) {
                strncpy(config.input_gains[config.num_saved_input_gains].device_name, key + strlen("INPUT_GAIN_"), sizeof(config.input_gains[0].device_name) - 1);
                config.input_gains[config.num_saved_input_gains].gain_multiplier = strtof(value, NULL);
                config.num_saved_input_gains++;
            }
        }
    }
    fclose(file);

    // Set the active multiplier based on the discovered device profile
    config.input_gain_multiplier = 1.0f;
    for (int i = 0; i < config.num_saved_input_gains; i++) {
        if (strcmp(config.input_gains[i].device_name, config.audio_device) == 0) {
            config.input_gain_multiplier = config.input_gains[i].gain_multiplier;
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
    fprintf(file, "WINDOW_WIDTH=%d\n", config->window_width);
    fprintf(file, "WINDOW_HEIGHT=%d\n", config->window_height);
    fprintf(file, "VIDEO_PREVIEW_WIDTH=%d\n", config->video_preview_width);
    fprintf(file, "VIDEO_PREVIEW_HEIGHT=%d\n", config->video_preview_height);
    fprintf(file, "VIDEO_DEVICE=%s\n", config->video_device);
    fprintf(file, "AUDIO_DEVICE=%s\n", config->audio_device);
    fprintf(file, "LOOPER_BLANK_CANVAS=%d\n", config->looper_blank_canvas);
    fprintf(file, "LOOPER_MODE_ACTIVE=%d\n", config->looper_mode_active);
    fprintf(file, "FREESTYLE_DURATION_MIN=%d\n", config->freestyle_duration_min);
    fprintf(file, "LOOPER_DURATION_MIN=%d\n", config->looper_duration_min);

    for (int i = 0; i < config->num_saved_input_gains; i++) {
        fprintf(file, "INPUT_GAIN_%s=%f\n", config->input_gains[i].device_name, config->input_gains[i].gain_multiplier);
    }

    fclose(file);
}
