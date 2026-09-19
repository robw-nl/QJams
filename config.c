#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <pthread.h>
#include <gtk/gtk.h>
#include <glib.h>
#include "config.h"
#include "ui_globals.h"

static FILE *qj_log_file = NULL;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

void init_qjams_logger(const char *qjams_dir, int enable_logging) {
    if (!enable_logging) return;

    char old_log[1024], new_log[1024];
    for (int i = 4; i >= 1; i--) {
        snprintf(old_log, sizeof(old_log), "%s/QJams.%d.log", qjams_dir, i);
        snprintf(new_log, sizeof(new_log), "%s/QJams.%d.log", qjams_dir, i + 1);
        rename(old_log, new_log);
    }
    snprintf(old_log, sizeof(old_log), "%s/QJams.log", qjams_dir);
    snprintf(new_log, sizeof(new_log), "%s/QJams.1.log", qjams_dir);
    rename(old_log, new_log);

    snprintf(new_log, sizeof(new_log), "%s/QJams.log", qjams_dir);
    qj_log_file = fopen(new_log, "w");
    if (qj_log_file) {
        setvbuf(qj_log_file, NULL, _IONBF, 0); // Strict unbuffered I/O
        time_t t = time(NULL);
        struct tm tm = *localtime(&t);
        fprintf(qj_log_file, "=========================================================\n");
        fprintf(qj_log_file, "QJams Session Log - Boot: %04d-%02d-%02d %02d:%02d:%02d\n",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
        fprintf(qj_log_file, "=========================================================\n\n");
    }
}

void close_qjams_logger(void) {
    if (qj_log_file) {
        fclose(qj_log_file);
        qj_log_file = NULL;
    }
}

static gboolean deferred_notify_status(gpointer data) {
    char *msg = (char *)data;
    if (lbl_status) gtk_label_set_markup(GTK_LABEL(lbl_status), msg);
    g_free(msg);
    return G_SOURCE_REMOVE;
}

static gboolean deferred_notify_toast(gpointer data) {
    char *msg = (char *)data;
    if (lbl_hw_toast) {
        gtk_label_set_markup(GTK_LABEL(lbl_hw_toast), msg);
        gtk_widget_set_visible(lbl_hw_toast, TRUE);
    }
    g_free(msg);
    return G_SOURCE_REMOVE;
}

/**
 * @brief Thread-safe logging and UI notification router.
 * @param notify_target 0 = Console/Log only, 1 = Console/Log + Main Status, 2 = Console/Log + Hardware Toast
 * @param fmt Standard printf formatting string
 */
void log_and_notify(int notify_target, const char *fmt, ...) {
    char buffer[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    char clean_buffer[2048];
    size_t j = 0;
    int in_tag = 0;
    for (int i = 0; buffer[i] != '\0' && j < sizeof(clean_buffer) - 1; i++) {
        if (buffer[i] == '<') {
            in_tag = 1;
            continue;
        }
        if (buffer[i] == '>') {
            in_tag = 0;
            continue;
        }
        if (!in_tag) {
            clean_buffer[j++] = buffer[i];
        }
    }
    clean_buffer[j] = '\0';

    size_t len = strlen(clean_buffer);

    pthread_mutex_lock(&log_mutex);
    printf("%s", clean_buffer);
    if (len > 0 && clean_buffer[len-1] != '\n') printf("\n");

    if (qj_log_file) {
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        fprintf(qj_log_file, "[%02d:%02d:%02d] %s", tm.tm_hour, tm.tm_min, tm.tm_sec, clean_buffer);
        if (len > 0 && clean_buffer[len-1] != '\n') fprintf(qj_log_file, "\n");
    }
    pthread_mutex_unlock(&log_mutex);

    if (notify_target == 1) {
        g_idle_add(deferred_notify_status, g_strdup(buffer));
    } else if (notify_target == 2) {
        g_idle_add(deferred_notify_toast, g_strdup(buffer));
    }
}

/**
 * @brief Loads the QJams configuration from the specified file, returning defaults if not found.
 * Utilizes thread-safe string tokenization and strictly bounded string copies to prevent memory bleeds.
 * @param filepath The path to the configuration file.
 * @return A populated QJamsConfig structure.
 */
QJamsConfig load_qjams_config(const char* filepath) {
    // Dynamically fetch OS-standard Music directory, fallback to Home
    const char *target_dir = g_get_user_special_dir(G_USER_DIRECTORY_MUSIC);
    if (!target_dir) target_dir = g_get_home_dir();

    char default_dir[512];
    snprintf(default_dir, sizeof(default_dir), "%s/QJams", target_dir);

    QJamsConfig config = {
        .input_gain_multiplier = 1.0f,
        .bt_gain_multiplier = 1.0f,
        .playback_target = "system:playback",
        .last_track_dir = "", // Initialized dynamically below
        .recordings_dir = "", // Initialized dynamically below
        .last_playlist_path = "",
        .window_width = 1400,
        .window_height = 1024,
        .video_device = "/dev/video0",
        .audio_device = "",
        .multitrack_blank_canvas = 0,
        .multitrack_mode_active = 0,
        .default_canvas_limit_min = 5,
        .export_format = 0,
        .enable_logging = 0,
        .playlist_slots = {"", "", "", "", "", "", "", "", "", ""},
        .num_audio_profiles = 0
    };

    strncpy(config.last_track_dir, default_dir, sizeof(config.last_track_dir) - 1);
    strncpy(config.recordings_dir, default_dir, sizeof(config.recordings_dir) - 1);

    FILE *file = fopen(filepath, "r");
    if (!file) return config;

    char line[1024]; // Increased buffer to absorb long path names
    while (fgets(line, sizeof(line), file)) {
        // Strip both Windows (\r) and Linux (\n) line endings entirely
        line[strcspn(line, "\r\n")] = 0;

        // Split safely at the first '=' to prevent swallowing empty variables
        char *eq = strchr(line, '=');
        if (!eq) continue;

        *eq = '\0';
        char *key = line;
        char *value = eq + 1;

        if (strcmp(key, "BT_GAIN_MULTIPLIER") == 0) {
            char *comma = strchr(value, ',');
            if (comma) *comma = '.';
            config.bt_gain_multiplier = (float)g_ascii_strtod(value, NULL);
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
        } else if (strcmp(key, "DEFAULT_CANVAS_LIMIT_MIN") == 0) {
            config.default_canvas_limit_min = atoi(value);
        } else if (strcmp(key, "EXPORT_FORMAT") == 0) {
            config.export_format = atoi(value);
        } else if (strcmp(key, "ENABLE_LOGGING") == 0) {
            config.enable_logging = atoi(value);
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

                char **parts = g_strsplit(value, ",", -1);
                int num_parts = g_strv_length(parts);

                if (num_parts >= 4) {
                    config.audio_profiles[config.num_audio_profiles].in_pref = atoi(parts[num_parts - 1]);
                    config.audio_profiles[config.num_audio_profiles].is_fallback = atoi(parts[num_parts - 2]);
                    config.audio_profiles[config.num_audio_profiles].is_primary = atoi(parts[num_parts - 3]);

                    if (num_parts == 4) {
                        config.audio_profiles[config.num_audio_profiles].gain_multiplier = (float)g_ascii_strtod(parts[0], NULL);
                    } else {
                        char float_str[32];
                        snprintf(float_str, sizeof(float_str), "%s.%s", parts[0], parts[1]);
                        config.audio_profiles[config.num_audio_profiles].gain_multiplier = (float)g_ascii_strtod(float_str, NULL);
                    }
                } else if (num_parts > 0) {
                    // Legacy migration: If only 2 parts exist, it's a comma-separated float with missing flags
                    if (num_parts == 2 && strlen(parts[1]) > 0 && g_ascii_isdigit(parts[1][0])) {
                        char float_str[32];
                        snprintf(float_str, sizeof(float_str), "%s.%s", parts[0], parts[1]);
                        config.audio_profiles[config.num_audio_profiles].gain_multiplier = (float)g_ascii_strtod(float_str, NULL);
                    } else {
                        config.audio_profiles[config.num_audio_profiles].gain_multiplier = (float)g_ascii_strtod(parts[0], NULL);
                    }
                    config.audio_profiles[config.num_audio_profiles].is_primary = 0;
                    config.audio_profiles[config.num_audio_profiles].is_fallback = 0;
                    config.audio_profiles[config.num_audio_profiles].in_pref = 0;
                }

                g_strfreev(parts);
                config.num_audio_profiles++;
            }
        } else if (strncmp(key, "INPUT_GAIN_", 11) == 0) {
            // Legacy configuration migration
            if (strcmp(key, "INPUT_GAIN_MULTIPLIER") == 0) continue;

            if (config.num_audio_profiles < MAX_SAVED_DEVICES) {
                strncpy(config.audio_profiles[config.num_audio_profiles].device_name, key + 11, 127);
                config.audio_profiles[config.num_audio_profiles].device_name[127] = '\0';

                char *comma = strchr(value, ',');
                if (comma) *comma = '.';
                config.audio_profiles[config.num_audio_profiles].gain_multiplier = (float)g_ascii_strtod(value, NULL);
                config.audio_profiles[config.num_audio_profiles].is_primary = 0;
                config.audio_profiles[config.num_audio_profiles].is_fallback = 0;
                config.audio_profiles[config.num_audio_profiles].in_pref = 0;
                config.num_audio_profiles++;
            }
        }
    }
    fclose(file);

    // BOUNDARY FALLBACK: Sanitize limits to prevent OOM allocation failures on boot
    if (config.default_canvas_limit_min <= 0 || config.default_canvas_limit_min > 120) {
        config.default_canvas_limit_min = 5;
    }

    // FORMAT BOUNDARY: Prevents FFmpeg argv generation crashes
    if (config.export_format < 0 || config.export_format > 2) {
        config.export_format = 0;
    }

    // GTK WINDOW BOUNDARIES: Prevents WM crashes or invisible windows
    if (config.window_width < 800 || config.window_width > 8192) {
        config.window_width = 1400;
    }
    if (config.window_height < 600 || config.window_height > 8192) {
        config.window_height = 1024;
    }

    // DSP GAIN BOUNDARIES: Protects against massive floats (GTK limit is +24dB / ~15.8x)
    if (config.bt_gain_multiplier < 0.0f || config.bt_gain_multiplier > 16.0f) {
        config.bt_gain_multiplier = 1.0f;
    }
    for (int i = 0; i < config.num_audio_profiles; i++) {
        if (config.audio_profiles[i].gain_multiplier < 0.0f || config.audio_profiles[i].gain_multiplier > 16.0f) {
            config.audio_profiles[i].gain_multiplier = 1.0f;
        }
    }

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
    char tmp_filepath[1024];
    snprintf(tmp_filepath, sizeof(tmp_filepath), "%s.tmp", filepath);

    FILE *file = fopen(tmp_filepath, "w");
    if (!file) {
        log_and_notify(0, "Error: Could not open config file %s for writing.\n", tmp_filepath);
        return;
    }

    char bt_gain_buf[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_dtostr(bt_gain_buf, sizeof(bt_gain_buf), config->bt_gain_multiplier);

    fprintf(file, "BT_GAIN_MULTIPLIER=%s\n", bt_gain_buf);
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
    fprintf(file, "DEFAULT_CANVAS_LIMIT_MIN=%d\n", config->default_canvas_limit_min);
    fprintf(file, "EXPORT_FORMAT=%d\n", config->export_format);
    fprintf(file, "ENABLE_LOGGING=%d\n", config->enable_logging);

    for (int i = 0; i < 10; i++) {
        if (strlen(config->playlist_slots[i]) > 0) {
            fprintf(file, "PLAYLIST_SLOT_%d=%s\n", i + 1, config->playlist_slots[i]);
        }
    }

    for (int i = 0; i < config->num_audio_profiles; i++) {
        char gain_buf[G_ASCII_DTOSTR_BUF_SIZE];
        g_ascii_dtostr(gain_buf, sizeof(gain_buf), config->audio_profiles[i].gain_multiplier);

        fprintf(file, "AUDIO_PROFILE_%s=%s,%d,%d,%d\n",
                config->audio_profiles[i].device_name,
                gain_buf,
                config->audio_profiles[i].is_primary,
                config->audio_profiles[i].is_fallback,
                config->audio_profiles[i].in_pref);
    }

    // Force data to disk before closing
    fflush(file);
    fclose(file);

    // Atomic swap guarantees the real file is never zero-bytes
    rename(tmp_filepath, filepath);
}
