#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdatomic.h>

#include "session.h"
#include "audio_engine.h"
#include "ui_looper.h"

#define QJAMS_MAGIC "QJMS"
#define QJAMS_VERSION 1

typedef struct {
    char magic[4];
    uint32_t version;
    size_t pristine_frames;
    size_t backing_track_frames;
    float playback_speed;
    int active_sample_rate;
    bool loop_active;
    size_t loop_start_frame;
    size_t loop_end_frame;
    int active_layer_count;
    int current_recording_layer;
    bool layer_is_muted[MAX_LOOPS];
    bool layer_is_soloed[MAX_LOOPS];
    char layer_names[MAX_LOOPS][64];
    bool layer_has_audio[MAX_LOOPS];
} SessionHeader;

int save_qjams_session(const char* filepath) {
    FILE *f = fopen(filepath, "wb");
    if (!f) return -1;

    SessionHeader header = {0};
    memcpy(header.magic, QJAMS_MAGIC, 4);
    header.version = QJAMS_VERSION;
    header.pristine_frames = pristine_frames;
    header.backing_track_frames = atomic_load_explicit(&backing_track_frames, memory_order_acquire);
    header.playback_speed = atomic_load_explicit(&playback_speed, memory_order_acquire);
    header.active_sample_rate = atomic_load_explicit(&active_sample_rate, memory_order_acquire);
    header.loop_active = atomic_load_explicit(&loop_active, memory_order_acquire);
    header.loop_start_frame = atomic_load_explicit(&loop_start_frame, memory_order_acquire);
    header.loop_end_frame = atomic_load_explicit(&loop_end_frame, memory_order_acquire);
    header.active_layer_count = atomic_load_explicit(&active_layer_count, memory_order_acquire);
    header.current_recording_layer = atomic_load_explicit(&current_recording_layer, memory_order_acquire);

    for (int i = 0; i < MAX_LOOPS; i++) {
        header.layer_is_muted[i] = atomic_load_explicit(&layer_is_muted[i], memory_order_acquire);
        header.layer_is_soloed[i] = atomic_load_explicit(&layer_is_soloed[i], memory_order_acquire);
        get_looper_layer_name(i, header.layer_names[i]);
        header.layer_has_audio[i] = (loop_layers[i] != NULL);
    }

    fwrite(&header, sizeof(SessionHeader), 1, f);

    for (int i = 0; i < MAX_LOOPS; i++) {
        if (header.layer_has_audio[i] && pristine_frames > 0) {
            fwrite(loop_layers[i], sizeof(float), pristine_frames * 2, f);
        }
    }

    fclose(f);
    return 0;
}

int load_qjams_session(const char* filepath) {
    FILE *f = fopen(filepath, "rb");
    if (!f) return -1;

    SessionHeader header;
    if (fread(&header, sizeof(SessionHeader), 1, f) != 1) { fclose(f); return -2; }
    if (strncmp(header.magic, QJAMS_MAGIC, 4) != 0 || header.version != QJAMS_VERSION) { fclose(f); return -3; }

    // Use existing engine logic to validate RAM, clear queues, and swap pointers safely
    if (init_exact_loop_canvas(header.pristine_frames) != 0) { fclose(f); return -4; }

    atomic_store_explicit(&backing_track_frames, header.backing_track_frames, memory_order_release);
    set_playback_speed(header.playback_speed);
    atomic_store_explicit(&active_sample_rate, header.active_sample_rate, memory_order_release);
    atomic_store_explicit(&loop_active, header.loop_active, memory_order_release);
    atomic_store_explicit(&loop_start_frame, header.loop_start_frame, memory_order_release);
    atomic_store_explicit(&loop_end_frame, header.loop_end_frame, memory_order_release);
    atomic_store_explicit(&active_layer_count, header.active_layer_count, memory_order_release);
    atomic_store_explicit(&current_recording_layer, header.current_recording_layer, memory_order_release);

    for (int i = 0; i < MAX_LOOPS; i++) {
        atomic_store_explicit(&layer_is_muted[i], header.layer_is_muted[i], memory_order_release);
        atomic_store_explicit(&layer_is_soloed[i], header.layer_is_soloed[i], memory_order_release);
        set_looper_layer_name(i, header.layer_names[i]);

        if (header.layer_has_audio[i] && loop_layers[i]) {
            fread(loop_layers[i], sizeof(float), header.pristine_frames * 2, f);
        }
    }

    // Sync base track to pristine cache so grid rendering works natively
    if (pristine_bt_buf && loop_layers[0]) {
        memcpy(pristine_bt_buf, loop_layers[0], header.pristine_frames * 2 * sizeof(float));
    }

    fclose(f);
    return 0;
}
