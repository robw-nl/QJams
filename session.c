#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <sndfile.h>
#include <archive.h>
#include <archive_entry.h>

#include "session.h"
#include "audio_engine.h"
#include "ui_multitrack.h"

#define QJAMS_MAGIC "QJMS"
#define QJAMS_VERSION 3

// Enforce strict byte-packing and fixed-width types to guarantee cross-architecture portability
typedef struct __attribute__((packed)) {
    char magic[4];
    uint32_t version;
    uint64_t pristine_frames;
    uint64_t backing_track_frames;
    float playback_speed;
    int32_t active_sample_rate;
    uint8_t loop_active;
    uint64_t loop_start_frame;
    uint64_t loop_end_frame;
    int32_t active_track_count;
    int32_t current_recording_track;
    uint8_t track_is_muted[MAX_TRACKS];
    uint8_t track_is_soloed[MAX_TRACKS];
    char track_names[MAX_TRACKS][64];
    uint8_t track_has_audio[MAX_TRACKS];
    float track_gains[MAX_TRACKS];
} SessionHeader;

// ==============================================================================
// VIRTUAL I/O RAM BUFFER (For In-Memory FLAC Encoding/Decoding)
// ==============================================================================

typedef struct {
    unsigned char *data;
    sf_count_t offset;
    sf_count_t length;
    sf_count_t capacity;
} VirtMemIO;

static sf_count_t vio_get_filelen(void *user_data) {
    return ((VirtMemIO*)user_data)->length;
}

static sf_count_t vio_seek(sf_count_t offset, int whence, void *user_data) {
    VirtMemIO *vio = (VirtMemIO*)user_data;
    switch (whence) {
        case SEEK_SET: vio->offset = offset; break;
        case SEEK_CUR: vio->offset += offset; break;
        case SEEK_END: vio->offset = vio->length + offset; break;
    }
    if (vio->offset < 0) vio->offset = 0;
    if (vio->offset > vio->length) vio->offset = vio->length;
    return vio->offset;
}

static sf_count_t vio_read(void *ptr, sf_count_t count, void *user_data) {
    VirtMemIO *vio = (VirtMemIO*)user_data;
    if (vio->offset + count > vio->length) count = vio->length - vio->offset;
    memcpy(ptr, vio->data + vio->offset, count);
    vio->offset += count;
    return count;
}

static sf_count_t vio_write(const void *ptr, sf_count_t count, void *user_data) {
    VirtMemIO *vio = (VirtMemIO*)user_data;
    if (vio->offset + count > vio->capacity) {
        vio->capacity = (vio->capacity + count) * 2;
        unsigned char *new_data = realloc(vio->data, vio->capacity);
        if (!new_data) return 0;
        vio->data = new_data;
    }
    memcpy(vio->data + vio->offset, ptr, count);
    vio->offset += count;
    if (vio->offset > vio->length) vio->length = vio->offset;
    return count;
}

static sf_count_t vio_tell(void *user_data) {
    return ((VirtMemIO*)user_data)->offset;
}

/**
 * @brief Saves the current multitrack session as a .tar archive containing a metadata header and compressed FLAC stems.
 * Runs purely in RAM using Virtual I/O buffers for maximum performance.
 * @param filepath Absolute path to the destination file.
 * @return 0 on success, or a negative error code.
 */
int save_qjams_session(const char* filepath) {
    if (!await_rt_thread_detach()) return -5;

    struct archive *a = archive_write_new();
    archive_write_add_filter_none(a);
    archive_write_set_format_ustar(a);
    if (archive_write_open_filename(a, filepath) != ARCHIVE_OK) {
        archive_write_free(a);
        resume_rt_thread();
        return -1;
    }

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
    header.active_track_count = atomic_load_explicit(&active_track_count, memory_order_acquire);
    header.current_recording_track = atomic_load_explicit(&current_recording_track, memory_order_acquire);

    for (int i = 0; i < MAX_TRACKS; i++) {
        header.track_is_muted[i] = false;
        header.track_is_soloed[i] = atomic_load_explicit(&master_tracks[i].is_soloed, memory_order_acquire);
        get_multitrack_track_name(i, header.track_names[i]);
        header.track_names[i][sizeof(header.track_names[i]) - 1] = '\0';
        header.track_has_audio[i] = atomic_load_explicit(&master_tracks[i].has_audio, memory_order_acquire);
        header.track_gains[i] = atomic_load_explicit(&master_tracks[i].gain, memory_order_acquire);
    }

    struct archive_entry *entry = archive_entry_new();
    archive_entry_set_pathname(entry, "header.bin");
    archive_entry_set_size(entry, sizeof(SessionHeader));
    archive_entry_set_filetype(entry, AE_IFREG);
    archive_entry_set_perm(entry, 0644);
    archive_write_header(a, entry);
    archive_write_data(a, &header, sizeof(SessionHeader));
    archive_entry_free(entry);

    SF_VIRTUAL_IO sf_vio = {
        .get_filelen = vio_get_filelen,
        .seek = vio_seek,
        .read = vio_read,
        .write = vio_write,
        .tell = vio_tell
    };

    for (int i = 0; i < MAX_TRACKS; i++) {
        if (header.track_has_audio[i] && master_tracks[i].active_buffer) {
            VirtMemIO vio = { .data = malloc(1024 * 1024), .capacity = 1024 * 1024, .offset = 0, .length = 0 };

            SF_INFO sfinfo = {0};
            sfinfo.channels = 2;
            sfinfo.samplerate = header.active_sample_rate;
            sfinfo.format = SF_FORMAT_FLAC | SF_FORMAT_PCM_24;

            SNDFILE *sf = sf_open_virtual(&sf_vio, SFM_WRITE, &sfinfo, &vio);
            if (sf) {
                sf_writef_float(sf, master_tracks[i].active_buffer, header.pristine_frames);
                sf_close(sf);

                char flac_name[32];
                snprintf(flac_name, sizeof(flac_name), "track_%d.flac", i);

                entry = archive_entry_new();
                archive_entry_set_pathname(entry, flac_name);
                archive_entry_set_size(entry, vio.length);
                archive_entry_set_filetype(entry, AE_IFREG);
                archive_entry_set_perm(entry, 0644);
                archive_write_header(a, entry);
                archive_write_data(a, vio.data, vio.length);
                archive_entry_free(entry);
            }
            free(vio.data);
        }
    }

    archive_write_close(a);
    archive_write_free(a);

    resume_rt_thread();
    return 0;
}

/**
 * @brief Loads a packaged DAW session from a .qjams archive, streaming FLAC tracks directly to RAM.
 * @param filepath Absolute path to the source file.
 * @return 0 on success, or a negative error code.
 */
int load_qjams_session(const char* filepath) {
    struct archive *a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);

    if (archive_read_open_filename(a, filepath, 10240) != ARCHIVE_OK) {
        archive_read_free(a);
        return -1;
    }

    struct archive_entry *entry;
    if (archive_read_next_header(a, &entry) != ARCHIVE_OK) {
        archive_read_close(a);
        archive_read_free(a);
        return -2;
    }

    SessionHeader header;
    bool header_loaded = false;
    VirtMemIO track_vios[MAX_TRACKS] = {0};

    do {
        const char *pathname = archive_entry_pathname(entry);
        size_t size = archive_entry_size(entry);

        if (strcmp(pathname, "header.bin") == 0) {
            if (size == sizeof(SessionHeader)) {
                archive_read_data(a, &header, size);
                header_loaded = true;
            }
        } else if (strncmp(pathname, "track_", 6) == 0) {
            int track_idx;
            if (sscanf(pathname, "track_%d.flac", &track_idx) == 1 && track_idx >= 0 && track_idx < MAX_TRACKS) {
                track_vios[track_idx].data = malloc(size);
                track_vios[track_idx].capacity = size;
                track_vios[track_idx].length = size;
                track_vios[track_idx].offset = 0;
                archive_read_data(a, track_vios[track_idx].data, size);
            }
        }
    } while (archive_read_next_header(a, &entry) == ARCHIVE_OK);

    archive_read_close(a);
    archive_read_free(a);

    // Strict V3 Only Validation
    if (!header_loaded || strncmp(header.magic, QJAMS_MAGIC, 4) != 0 || header.version != QJAMS_VERSION) {
        for (int i=0; i<MAX_TRACKS; i++) if (track_vios[i].data) free(track_vios[i].data);
        return -3;
    }

    // Sanitation
    if (header.active_track_count < 1 || header.active_track_count > MAX_TRACKS) header.active_track_count = 1;
    if (header.current_recording_track < 0 || header.current_recording_track >= MAX_TRACKS) header.current_recording_track = 0;
    if (header.playback_speed <= 0.05f || header.playback_speed > 4.0f) header.playback_speed = 1.0f;
    if (header.loop_start_frame > header.backing_track_frames) header.loop_start_frame = 0;
    if (header.loop_end_frame > header.backing_track_frames) header.loop_end_frame = header.backing_track_frames;
    if (header.loop_start_frame >= header.loop_end_frame) header.loop_active = 0;

    if (init_exact_loop_canvas(header.pristine_frames) != 0) {
        for (int i=0; i<MAX_TRACKS; i++) if (track_vios[i].data) free(track_vios[i].data);
        return -4;
    }

    if (!await_rt_thread_detach()) {
        for (int i=0; i<MAX_TRACKS; i++) if (track_vios[i].data) free(track_vios[i].data);
        return -5;
    }

    atomic_store_explicit(&backing_track_frames, header.backing_track_frames, memory_order_release);
    set_playback_speed(header.playback_speed);
    atomic_store_explicit(&active_sample_rate, header.active_sample_rate, memory_order_release);
    atomic_store_explicit(&loop_active, header.loop_active != 0, memory_order_release);
    atomic_store_explicit(&loop_start_frame, header.loop_start_frame, memory_order_release);
    atomic_store_explicit(&loop_end_frame, header.loop_end_frame, memory_order_release);
    atomic_store_explicit(&active_track_count, header.active_track_count, memory_order_release);
    atomic_store_explicit(&current_recording_track, header.current_recording_track, memory_order_release);

    SF_VIRTUAL_IO sf_vio = {
        .get_filelen = vio_get_filelen,
        .seek = vio_seek,
        .read = vio_read,
        .write = vio_write,
        .tell = vio_tell
    };

    for (int i = 0; i < MAX_TRACKS; i++) {
        atomic_store_explicit(&master_tracks[i].is_soloed, header.track_is_soloed[i] != 0, memory_order_release);
        atomic_store_explicit(&master_tracks[i].has_audio, header.track_has_audio[i] != 0, memory_order_release);
        atomic_store_explicit(&master_tracks[i].has_undo, false, memory_order_release);

        float gain_val = (header.track_gains[i] >= 0.0f && header.track_gains[i] <= 10.0f) ? header.track_gains[i] : 1.0f;
        set_multitrack_layer_gain(i, gain_val);

        header.track_names[i][sizeof(header.track_names[i]) - 1] = '\0';
        set_multitrack_track_name(i, header.track_names[i]);

        if (header.track_has_audio[i] && master_tracks[i].active_buffer && track_vios[i].data) {
            SF_INFO sfinfo = {0};
            SNDFILE *sf = sf_open_virtual(&sf_vio, SFM_READ, &sfinfo, &track_vios[i]);
            if (sf) {
                sf_readf_float(sf, master_tracks[i].active_buffer, header.pristine_frames);
                sf_close(sf);
            }
        }
        if (track_vios[i].data) free(track_vios[i].data);
    }

    if (pristine_bt_buf && master_tracks[0].active_buffer) {
        memcpy(pristine_bt_buf, master_tracks[0].active_buffer, header.pristine_frames * 2 * sizeof(float));
    }

    resume_rt_thread();
    return 0;
}
