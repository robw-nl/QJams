#include "media_io.h"
#include "audio_core.h"
#include "audio_internal.h"
#include "audio_memory.h"
#include "config.h"
#include "ringbuffer.h"
#include "ui_globals.h"
#include "ui_multitrack.h"
#include <archive.h>
#include <archive_entry.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <math.h>
#include <rubberband/rubberband-c.h>
#include <sndfile.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define QJAMS_MAGIC "QJMS"
#define QJAMS_VERSION 6

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
    uint8_t track_is_muted[QJ_MAX_TRACKS];
    uint8_t track_is_soloed[QJ_MAX_TRACKS];
    char track_names[QJ_MAX_TRACKS][64];
    uint8_t track_has_audio[QJ_MAX_TRACKS];
    float track_gains[QJ_MAX_TRACKS];
    uint64_t video_start_frame;
    uint64_t video_end_frame;
    int32_t video_track_index;    // Appended for V5 backwards compatibility
    int32_t timebase_track_index; // Appended for V6 backwards compatibility
} SessionHeader;

typedef struct __attribute__((packed)) {
    uint8_t val;
    uint64_t count;
} QJParityRLEChunk;

typedef struct {
    unsigned char *data;
    sf_count_t offset;
    sf_count_t length;
    sf_count_t capacity;
} VirtMemIO;

static sf_count_t vio_get_filelen(void *user_data) {
    return ((VirtMemIO *)user_data)->length;
}

static sf_count_t vio_seek(sf_count_t offset, int whence, void *user_data) {
    VirtMemIO *vio = (VirtMemIO *)user_data;
    sf_count_t new_offset = 0;

    switch (whence) {
        case SEEK_SET:
            new_offset = offset;
            break;
        case SEEK_CUR:
            new_offset = vio->offset + offset;
            break;
        case SEEK_END:
            new_offset = vio->length + offset;
            break;
    }
    if (new_offset < 0)
        new_offset = 0;

    if (new_offset > vio->length) {
        if (new_offset > vio->capacity) {
            sf_count_t new_cap = new_offset + (1024 * 1024);
            unsigned char *new_data = realloc(vio->data, new_cap);
            if (!new_data)
                return vio->offset;
            vio->data = new_data;
            vio->capacity = new_cap;
        }
        memset(vio->data + vio->length, 0, new_offset - vio->length);
        vio->length = new_offset;
    }

    vio->offset = new_offset;
    return vio->offset;
}

static sf_count_t vio_read(void *ptr, sf_count_t count, void *user_data) {
    VirtMemIO *vio = (VirtMemIO *)user_data;
    if (vio->offset + count > vio->length)
        count = vio->length - vio->offset;
    memcpy(ptr, vio->data + vio->offset, count);
    vio->offset += count;
    return count;
}

static sf_count_t vio_write(const void *ptr, sf_count_t count, void *user_data) {
    VirtMemIO *vio = (VirtMemIO *)user_data;
    if (vio->offset + count > vio->capacity) {
        vio->capacity = (vio->capacity + count) * 2;
        unsigned char *new_data = realloc(vio->data, vio->capacity);
        if (!new_data)
            return 0;
        vio->data = new_data;
    }
    memcpy(vio->data + vio->offset, ptr, count);
    vio->offset += count;
    if (vio->offset > vio->length)
        vio->length = vio->offset;
    return count;
}

static sf_count_t vio_tell(void *user_data) {
    return ((VirtMemIO *)user_data)->offset;
}

/**
 * @brief Saves the active multi-track session to a .qjams USTAR archive.
 *        Pins track buffers via try_acquire_track_read() to prevent UAF during background autosave.
 * @param filepath Destination file path.
 * @return 0 on success, -1 on error or concurrent teardown.
 */
int save_qjams_session(const char *filepath) {
    if (!try_acquire_track_read())
        return -1;
    char tmp_filepath[1024];
    snprintf(tmp_filepath, sizeof(tmp_filepath), "%s.tmp", filepath);
    struct archive *a = archive_write_new();
    archive_write_add_filter_none(a);
    archive_write_set_format_ustar(a);
    if (archive_write_open_filename(a, tmp_filepath) != ARCHIVE_OK) {
        archive_write_free(a);
        release_track_read();
        return -1;
    }
    SessionHeader header = {0};
    memcpy(header.magic, QJAMS_MAGIC, 4);
    header.version = QJAMS_VERSION;
    header.pristine_frames = pristine_frames;
    header.backing_track_frames =
        atomic_load_explicit(&get_transport_state()->backing_track_frames, memory_order_acquire);
    header.playback_speed =
        atomic_load_explicit(&get_transport_state()->playback_speed, memory_order_acquire);
    header.active_sample_rate =
        atomic_load_explicit(&get_transport_state()->active_sample_rate, memory_order_acquire);
    header.loop_active = atomic_load_explicit(&loop_active, memory_order_acquire);
    header.loop_start_frame = atomic_load_explicit(&loop_start_frame, memory_order_acquire);
    header.loop_end_frame = atomic_load_explicit(&loop_end_frame, memory_order_acquire);
    header.active_track_count = atomic_load_explicit(&active_track_count, memory_order_acquire);
    header.current_recording_track =
        atomic_load_explicit(&current_recording_track, memory_order_acquire);
    header.video_start_frame = video_start_frame;
    header.video_end_frame = video_end_frame;
    header.video_track_index = video_track_index; // Appended for V5 backwards compatibility
    header.timebase_track_index = atomic_load_explicit(&timebase_track, memory_order_acquire);

    for (int i = 0; i < QJ_MAX_TRACKS; i++) {
        header.track_is_muted[i] = false;
        header.track_is_soloed[i] =
            atomic_load_explicit(&master_tracks[i].is_soloed, memory_order_acquire);
        get_multitrack_track_name(i, header.track_names[i]);
        header.track_names[i][sizeof(header.track_names[i]) - 1] = '\0';
        header.track_has_audio[i] =
            atomic_load_explicit(&master_tracks[i].has_audio, memory_order_acquire);
        header.track_gains[i] = atomic_load_explicit(&master_tracks[i].gain, memory_order_acquire);
    }

    struct archive_entry *entry = archive_entry_new();
    archive_entry_set_pathname(entry, "header.bin");
    archive_entry_set_size(entry, sizeof(SessionHeader));
    archive_entry_set_filetype(entry, AE_IFREG);
    archive_entry_set_perm(entry, 0644);
    archive_write_header(a, entry);

    const char *h_ptr = (const char *)&header;
    size_t h_left = sizeof(SessionHeader);
    while (h_left > 0) {
        ssize_t w = archive_write_data(a, h_ptr, h_left);
        if (w <= 0)
            break;
        h_ptr += w;
        h_left -= w;
    }
    archive_entry_free(entry);

    SF_VIRTUAL_IO sf_vio = {.get_filelen = vio_get_filelen,
                            .seek = vio_seek,
                            .read = vio_read,
                            .write = vio_write,
                            .tell = vio_tell};

    for (int i = 0; i < QJ_MAX_TRACKS; i++) {
        if (header.track_has_audio[i] && master_tracks[i].active_buffer) {
            VirtMemIO vio = {
                .data = malloc(1024 * 1024), .capacity = 1024 * 1024, .offset = 0, .length = 0};

            /**
             * @brief Encode session tracks as genuine 24-bit lossless FLAC with hardware clipping
             * protection to minimize archive size and background autosave I/O latency.
             */
            SF_INFO sfinfo = {0};
            sfinfo.channels = 2;
            sfinfo.samplerate = header.active_sample_rate;
            sfinfo.format = SF_FORMAT_FLAC | SF_FORMAT_PCM_24;

            SNDFILE *sf = sf_open_virtual(&sf_vio, SFM_WRITE, &sfinfo, &vio);
            if (sf) {
                sf_command(sf, SFC_SET_CLIPPING, NULL, SF_TRUE);
                // AUTOSAVE: Clamp the read size to the physical allocation
                size_t safe_frames = header.pristine_frames;
                if (safe_frames > master_tracks[i].allocated_frames) {
                    safe_frames = master_tracks[i].allocated_frames;
                }

                (void)sf_writef_float(sf, master_tracks[i].active_buffer, safe_frames);
                sf_close(sf);

                char flac_name[32];
                snprintf(flac_name, sizeof(flac_name), "track_%d.flac", i);

                entry = archive_entry_new();
                archive_entry_set_pathname(entry, flac_name);
                archive_entry_set_size(entry, vio.length);
                archive_entry_set_filetype(entry, AE_IFREG);
                archive_entry_set_perm(entry, 0644);
                archive_write_header(a, entry);

                const char *v_ptr = (const char *)vio.data;
                size_t v_left = vio.length;
                while (v_left > 0) {
                    ssize_t w = archive_write_data(a, v_ptr, v_left);
                    if (w <= 0)
                        break;
                    v_ptr += w;
                    v_left -= w;
                }
                archive_entry_free(entry);
            }
            free(vio.data);

            // PARITY MAP RLE COMPRESSION & ARCHIVING
            if (master_tracks[i].parity_map && header.pristine_frames > 0) {
                size_t safe_p_frames = header.pristine_frames;
                if (safe_p_frames > master_tracks[i].allocated_frames) {
                    safe_p_frames = master_tracks[i].allocated_frames;
                }
                size_t rle_capacity = 256;
                size_t rle_count = 0;
                QJParityRLEChunk *rle_buf = malloc(rle_capacity * sizeof(QJParityRLEChunk));
                if (rle_buf && safe_p_frames > 0) {
                    uint8_t current_val = master_tracks[i].parity_map[0];
                    uint64_t current_len = 1;
                    for (size_t f = 1; f < safe_p_frames; f++) {
                        if (master_tracks[i].parity_map[f] == current_val) {
                            current_len++;
                        } else {
                            if (rle_count >= rle_capacity) {
                                rle_capacity *= 2;
                                QJParityRLEChunk *new_buf =
                                    realloc(rle_buf, rle_capacity * sizeof(QJParityRLEChunk));
                                if (!new_buf)
                                    break;
                                rle_buf = new_buf;
                            }
                            rle_buf[rle_count].val = current_val;
                            rle_buf[rle_count].count = current_len;
                            rle_count++;
                            current_val = master_tracks[i].parity_map[f];
                            current_len = 1;
                        }
                    }
                    if (rle_count >= rle_capacity) {
                        QJParityRLEChunk *new_buf =
                            realloc(rle_buf, (rle_count + 1) * sizeof(QJParityRLEChunk));
                        if (!new_buf) {
                            free(rle_buf);
                            rle_buf = NULL;
                        } else {
                            rle_buf = new_buf;
                        }
                    }
                    if (rle_buf) {
                        rle_buf[rle_count].val = current_val;
                        rle_buf[rle_count].count = current_len;
                        rle_count++;

                        char parity_name[64];
                        snprintf(parity_name, sizeof(parity_name), "track_%d_parity.bin", i);

                        struct archive_entry *p_entry = archive_entry_new();
                        archive_entry_set_pathname(p_entry, parity_name);
                        archive_entry_set_size(p_entry, rle_count * sizeof(QJParityRLEChunk));
                        archive_entry_set_filetype(p_entry, AE_IFREG);
                        archive_entry_set_perm(p_entry, 0644);
                        archive_write_header(a, p_entry);

                        const char *p_ptr = (const char *)rle_buf;
                        size_t p_left = rle_count * sizeof(QJParityRLEChunk);
                        while (p_left > 0) {
                            ssize_t w = archive_write_data(a, p_ptr, p_left);
                            if (w <= 0)
                                break;
                            p_ptr += w;
                            p_left -= w;
                        }
                        archive_entry_free(p_entry);
                        free(rle_buf);
                    }
                }
            }
        }
    }
    const char *active_video_file = NULL;
    if (has_active_live_video()) {
        active_video_file = current_raw_path;
    } else if (video_end_frame > video_start_frame && strlen(ui_state.selected_track_path) > 0 &&
               media_has_video(ui_state.selected_track_path) &&
               access(ui_state.selected_track_path, F_OK) == 0) {
        active_video_file = ui_state.selected_track_path;
    }
    if (active_video_file) {
        FILE *vf = fopen(active_video_file, "rb");
        if (vf) {
            fseek(vf, 0, SEEK_END);
            long v_size = ftell(vf);
            fseek(vf, 0, SEEK_SET);

            struct archive_entry *v_entry = archive_entry_new();
            archive_entry_set_pathname(v_entry, "video_take.mkv");
            archive_entry_set_size(v_entry, v_size);
            archive_entry_set_filetype(v_entry, AE_IFREG);
            archive_entry_set_perm(v_entry, 0644);
            archive_write_header(a, v_entry);

            char v_buf[8192];
            size_t bytes_read;
            while ((bytes_read = fread(v_buf, 1, sizeof(v_buf), vf)) > 0) {
                archive_write_data(a, v_buf, bytes_read);
            }
            archive_entry_free(v_entry);
            fclose(vf);
        }
    }

    int close_status = archive_write_close(a);
    archive_write_free(a);
    release_track_read();
    if (close_status != ARCHIVE_OK) {
        remove(tmp_filepath);
        return -1;
    }

    if (rename(tmp_filepath, filepath) != 0) {
        remove(tmp_filepath);
        return -1;
    }

    return 0;
}

/**
 * @brief Loads a multi-track QJams session (.qjams USTAR archive) into memory.
 *        Validates entry sizes, prevents duplicate entry leaks, and guards against
 *        64-bit integer overflow in RLE parity map decompression.
 * @param filepath Path to the .qjams archive on disk.
 * @return 0 on success, negative error code on failure or corrupt archive.
 */
int load_qjams_session(const char *filepath) {
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
    VirtMemIO track_vios[QJ_MAX_TRACKS] = {0};
    VirtMemIO parity_vios[QJ_MAX_TRACKS] = {0};
    bool video_restored = false;
    const la_int64_t max_entry_bytes =
        (la_int64_t)(120ULL * 60ULL * 192000ULL * 2ULL * sizeof(float) + 65536ULL);
    do {
        const char *pathname = archive_entry_pathname(entry);
        la_int64_t raw_size = archive_entry_size(entry);
        if (!pathname || raw_size < 0)
            continue;
        size_t size = (size_t)raw_size;
        if (strcmp(pathname, "header.bin") == 0) {
            size_t read_limit = (size < sizeof(SessionHeader)) ? size : sizeof(SessionHeader);
            char *r_ptr = (char *)&header;
            size_t r_left = read_limit;
            memset(&header, 0, sizeof(SessionHeader));
            while (r_left > 0) {
                ssize_t r_bytes = archive_read_data(a, r_ptr, r_left);
                if (r_bytes <= 0)
                    break;
                r_ptr += r_bytes;
                r_left -= r_bytes;
            }
            size_t excess = size > read_limit ? size - read_limit : 0;
            char drain_buf[1024];
            while (excess > 0) {
                ssize_t r_bytes = archive_read_data(
                    a, drain_buf, excess > sizeof(drain_buf) ? sizeof(drain_buf) : excess);
                if (r_bytes <= 0)
                    break;
                excess -= r_bytes;
            }
            header_loaded = true;
        } else if (strncmp(pathname, "track_", 6) == 0) {
            if (raw_size == 0 || raw_size > max_entry_bytes)
                continue;
            int track_idx;
            if (strstr(pathname, "_parity.bin") &&
                sscanf(pathname, "track_%d_parity.bin", &track_idx) == 1 && track_idx >= 0 &&
                track_idx < QJ_MAX_TRACKS) {
                if (parity_vios[track_idx].data) {
                    free(parity_vios[track_idx].data);
                    parity_vios[track_idx].data = NULL;
                }
                parity_vios[track_idx].data = malloc(size);
                if (!parity_vios[track_idx].data)
                    continue;
                parity_vios[track_idx].capacity = size;
                parity_vios[track_idx].length = size;
                parity_vios[track_idx].offset = 0;
                char *v_ptr = (char *)parity_vios[track_idx].data;
                size_t v_left = size;
                while (v_left > 0) {
                    ssize_t r_bytes = archive_read_data(a, v_ptr, v_left);
                    if (r_bytes <= 0)
                        break;
                    v_ptr += r_bytes;
                    v_left -= r_bytes;
                }
            } else if (sscanf(pathname, "track_%d.flac", &track_idx) == 1 && track_idx >= 0 &&
                       track_idx < QJ_MAX_TRACKS) {
                if (track_vios[track_idx].data) {
                    free(track_vios[track_idx].data);
                    track_vios[track_idx].data = NULL;
                }
                track_vios[track_idx].data = malloc(size);
                if (!track_vios[track_idx].data)
                    continue;
                track_vios[track_idx].capacity = size;
                track_vios[track_idx].length = size;
                track_vios[track_idx].offset = 0;
                char *v_ptr = (char *)track_vios[track_idx].data;
                size_t v_left = size;
                while (v_left > 0) {
                    ssize_t r_bytes = archive_read_data(a, v_ptr, v_left);
                    if (r_bytes <= 0)
                        break;
                    v_ptr += r_bytes;
                    v_left -= r_bytes;
                }
            }
        } else if (strcmp(pathname, "video_take.mkv") == 0) {
            FILE *vf = fopen("/tmp/qjams_session_video.mkv", "wb");
            if (vf) {
                char v_buf[8192];
                size_t v_left = size;
                while (v_left > 0) {
                    size_t read_chunk = (v_left > sizeof(v_buf)) ? sizeof(v_buf) : v_left;
                    ssize_t r_bytes = archive_read_data(a, v_buf, read_chunk);
                    if (r_bytes <= 0)
                        break;
                    fwrite(v_buf, 1, r_bytes, vf);
                    v_left -= r_bytes;
                }
                fclose(vf);
                video_restored = true;
            }
        }
    } while (archive_read_next_header(a, &entry) == ARCHIVE_OK);
    archive_read_close(a);
    archive_read_free(a);
    if (!header_loaded || strncmp(header.magic, QJAMS_MAGIC, 4) != 0 ||
        (header.version < 3 || header.version > QJAMS_VERSION)) {
        for (int i = 0; i < QJ_MAX_TRACKS; i++) {
            if (track_vios[i].data)
                free(track_vios[i].data);
            if (parity_vios[i].data)
                free(parity_vios[i].data);
        }
        if (video_restored)
            remove("/tmp/qjams_session_video.mkv");
        return -3;
    }
    if (header.active_track_count < 1 || header.active_track_count > QJ_MAX_TRACKS)
        header.active_track_count = 1;
    if (header.current_recording_track < 0 || header.current_recording_track >= QJ_MAX_TRACKS)
        header.current_recording_track = 0;
    if (header.playback_speed <= 0.05f || header.playback_speed > 4.0f)
        header.playback_speed = 1.0f;
    if (header.loop_start_frame > header.backing_track_frames)
        header.loop_start_frame = 0;
    if (header.loop_end_frame > header.backing_track_frames)
        header.loop_end_frame = header.backing_track_frames;
    if (header.loop_start_frame >= header.loop_end_frame)
        header.loop_active = 0;
    size_t max_allowed_frames = 120ULL * 60ULL * 192000ULL;
    if (header.pristine_frames == 0 || header.pristine_frames > max_allowed_frames) {
        for (int i = 0; i < QJ_MAX_TRACKS; i++) {
            if (track_vios[i].data)
                free(track_vios[i].data);
            if (parity_vios[i].data)
                free(parity_vios[i].data);
        }
        if (video_restored)
            remove("/tmp/qjams_session_video.mkv");
        return -6;
    }
    if (init_exact_loop_canvas(header.pristine_frames) != 0) {
        for (int i = 0; i < QJ_MAX_TRACKS; i++) {
            if (track_vios[i].data)
                free(track_vios[i].data);
            if (parity_vios[i].data)
                free(parity_vios[i].data);
        }
        if (video_restored)
            remove("/tmp/qjams_session_video.mkv");
        return -5;
    }
    atomic_store_explicit(&get_transport_state()->backing_track_frames, header.backing_track_frames,
                          memory_order_release);
    atomic_store_explicit(&get_transport_state()->playback_speed, header.playback_speed,
                          memory_order_release);
    extern _Atomic float speed_target;
    atomic_store_explicit(&speed_target, header.playback_speed, memory_order_relaxed);
    extern _Atomic bool stretcher_ratio_update;
    atomic_store_explicit(&stretcher_ratio_update, true, memory_order_release);

    atomic_store_explicit(&get_transport_state()->active_sample_rate, header.active_sample_rate,
                          memory_order_release);
    atomic_store_explicit(&loop_start_frame, header.loop_start_frame, memory_order_release);
    atomic_store_explicit(&loop_end_frame, header.loop_end_frame, memory_order_release);
    atomic_store_explicit(&loop_active, header.loop_active != 0, memory_order_release);
    atomic_store_explicit(&active_track_count, header.active_track_count, memory_order_release);
    atomic_store_explicit(&current_recording_track, header.current_recording_track,
                          memory_order_release);
    atomic_store_explicit(&selected_tracks_mask, (1U << header.current_recording_track),
                          memory_order_release);
    if (header.version >= 4) {
        video_start_frame = header.video_start_frame;
        video_end_frame = header.video_end_frame;
    } else {
        video_start_frame = 0;
        video_end_frame = 0;
    }
    if (header.version >= 5) {
        video_track_index = header.video_track_index;
    } else if (header.version == 4 && video_end_frame > video_start_frame) {
        video_track_index = 0;
    } else {
        video_track_index = -1;
    }
    if (video_restored) {
        strncpy(current_raw_path, "/tmp/qjams_session_video.mkv", 1023);
        current_raw_path[1023] = '\0';
    } else {
        current_raw_path[0] = '\0';
    }
    SF_VIRTUAL_IO sf_vio = {.get_filelen = vio_get_filelen,
                            .seek = vio_seek,
                            .read = vio_read,
                            .write = vio_write,
                            .tell = vio_tell};

    for (int i = 0; i < QJ_MAX_TRACKS; i++) {
        atomic_store_explicit(&master_tracks[i].is_soloed, header.track_is_soloed[i] != 0,
                              memory_order_release);
        atomic_store_explicit(&master_tracks[i].has_audio, false, memory_order_release);
        float gain_val = (header.track_gains[i] >= 0.0f && header.track_gains[i] <= 10.0f)
                             ? header.track_gains[i]
                             : 1.0f;
        set_multitrack_layer_gain(i, gain_val);
        header.track_names[i][sizeof(header.track_names[i]) - 1] = '\0';
        set_multitrack_track_name(i, header.track_names[i]);
        bool decoded_ok = false;
        if (header.track_has_audio[i] && master_tracks[i].active_buffer && track_vios[i].data) {
            SF_INFO sfinfo = {0};
            SNDFILE *sf = sf_open_virtual(&sf_vio, SFM_READ, &sfinfo, &track_vios[i]);
            if (sf) {
                if (sfinfo.channels == 2) {
                    sf_readf_float(sf, master_tracks[i].active_buffer, header.pristine_frames);
                    decoded_ok = true;
                } else {
                    log_and_notify(1,
                                   "<span foreground='#ff4444'><b>Critical: Malformed session FLAC "
                                   "(channels != 2). Track %d aborted.</b></span>",
                                   i);
                }
                sf_close(sf);
            }
        }
        if (decoded_ok && master_tracks[i].parity_map) {
            if (parity_vios[i].data) {
                QJParityRLEChunk *rle_buf = (QJParityRLEChunk *)parity_vios[i].data;
                size_t num_chunks = (size_t)parity_vios[i].length / sizeof(QJParityRLEChunk);
                size_t write_idx = 0;
                size_t max_p_frames = (header.pristine_frames <= master_tracks[i].allocated_frames)
                                          ? (size_t)header.pristine_frames
                                          : master_tracks[i].allocated_frames;
                for (size_t c = 0; c < num_chunks && write_idx < max_p_frames; c++) {
                    uint8_t val = rle_buf[c].val;
                    uint64_t count = rle_buf[c].count;
                    size_t remaining = max_p_frames - write_idx;
                    size_t to_write = (count <= (uint64_t)remaining) ? (size_t)count : remaining;
                    if (to_write > 0) {
                        memset(&master_tracks[i].parity_map[write_idx], val, to_write);
                        write_idx += to_write;
                    }
                }
            } else {
                size_t last_f = 0;
                for (size_t f = header.pristine_frames; f > 0; f--) {
                    if (fabsf(master_tracks[i].active_buffer[(f - 1) * 2]) > 0.00015f ||
                        fabsf(master_tracks[i].active_buffer[(f - 1) * 2 + 1]) > 0.00015f) {
                        last_f = f;
                        break;
                    }
                }
                if (last_f > 0) {
                    memset(master_tracks[i].parity_map, 1, last_f);
                }
            }
        }
        atomic_store_explicit(&master_tracks[i].has_audio, decoded_ok, memory_order_release);
        if (track_vios[i].data)
            free(track_vios[i].data);
        if (parity_vios[i].data)
            free(parity_vios[i].data);
    }

    int resolved_timebase = 0;
    if (header.version >= 6) {
        resolved_timebase = header.timebase_track_index;
    } else {
        for (int i = 0; i < QJ_MAX_TRACKS; i++) {
            if (atomic_load_explicit(&master_tracks[i].has_audio, memory_order_acquire)) {
                resolved_timebase = i;
                break;
            }
        }
    }

    atomic_store_explicit(&timebase_track, resolved_timebase, memory_order_release);
    atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);
    atomic_store_explicit(&cached_audible_frames, get_last_audible_frame(), memory_order_release);

    mark_session_saved();
    return 0;
}

static int drain_and_extract_frames(AVCodecContext *codec_ctx, AVFrame *frame, float **raw_buf,
                                    size_t *capacity, size_t *total_samples, int channels,
                                    int *true_rate) {
    while (avcodec_receive_frame(codec_ctx, frame) == 0) {
        if (true_rate && frame->sample_rate > 0)
            *true_rate = frame->sample_rate;
        int nb_samples = frame->nb_samples;

        // Safety [Critical]: Detect true decoded layout to prevent heap over-reads on Mono files
        int frame_channels =
            frame->ch_layout.nb_channels > 0 ? frame->ch_layout.nb_channels : channels;

        if ((size_t)nb_samples > SIZE_MAX / channels)
            return -1;
        size_t new_samples = (size_t)nb_samples * channels;

        if (SIZE_MAX - *total_samples < new_samples)
            return -1;

        if (*total_samples + new_samples > *capacity * channels) {
            if (*capacity > SIZE_MAX / 2)
                return -1;
            size_t new_cap = *capacity * 2;

            if (new_cap > SIZE_MAX / (channels * sizeof(float)))
                return -1;

            float *tmp = realloc(*raw_buf, new_cap * channels * sizeof(float));
            if (!tmp)
                return -1;
            *capacity = new_cap;
            *raw_buf = tmp;
        }

        for (int s = 0; s < nb_samples; s++) {
            for (int c = 0; c < channels; c++) {
                // Safely duplicate Mono (0) to Right channel (1) if missing
                int src_c = (c < frame_channels) ? c : 0;
                float sample_val = 0.0f;

                if (frame->format == AV_SAMPLE_FMT_FLTP)
                    sample_val = ((float *)frame->data[src_c])[s];
                else if (frame->format == AV_SAMPLE_FMT_FLT)
                    sample_val = ((float *)frame->data[0])[s * frame_channels + src_c];
                else if (frame->format == AV_SAMPLE_FMT_S16P)
                    sample_val = ((int16_t *)frame->data[src_c])[s] / 32768.0f;
                else if (frame->format == AV_SAMPLE_FMT_S16)
                    sample_val = ((int16_t *)frame->data[0])[s * frame_channels + src_c] / 32768.0f;
                else if (frame->format == AV_SAMPLE_FMT_S32P)
                    sample_val = ((int32_t *)frame->data[src_c])[s] / 2147483648.0f;
                else if (frame->format == AV_SAMPLE_FMT_S32)
                    sample_val =
                        ((int32_t *)frame->data[0])[s * frame_channels + src_c] / 2147483648.0f;

                (*raw_buf)[(*total_samples)++] = sample_val;
            }
        }
    }
    return 0;
}

/**
 * @brief Decodes the primary audio stream of a media file into a flat float buffer using FFmpeg.
 * @param filepath Path to the input media file.
 * @param out_raw Output pointer receiving the allocated float sample buffer.
 * @param out_frames Output pointer receiving the total decoded frame count.
 * @param out_channels Output pointer receiving the channel count.
 * @param out_rate Output pointer receiving the decoded sample rate.
 * @param target_jack_rate Active JACK sample rate for RAM capacity estimation.
 * @return 0 on success, -1 on decode error, -2 on insufficient RAM.
 */
static int load_audio_ffmpeg(const char *filepath, float **out_raw, sf_count_t *out_frames,
                             int *out_channels, int *out_rate, int target_jack_rate) {
    AVFormatContext *fmt_ctx = NULL;
    if (avformat_open_input(&fmt_ctx, filepath, NULL, NULL) != 0)
        return -1;
    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    int stream_idx = -1;
    for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            stream_idx = (int)i;
            break;
        }
    }

    if (stream_idx < 0) {
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    AVStream *st = fmt_ctx->streams[stream_idx];
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    AVCodecContext *codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        avformat_close_input(&fmt_ctx);
        return -1;
    }
    avcodec_parameters_to_context(codec_ctx, st->codecpar);

    if (codec_ctx->ch_layout.nb_channels == 0 ||
        codec_ctx->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) {
        av_channel_layout_default(&codec_ctx->ch_layout, 2);
    }
    if (codec_ctx->sample_rate == 0) {
        codec_ctx->sample_rate = 48000;
    }

    codec_ctx->pkt_timebase = st->time_base;

    if (avcodec_open2(codec_ctx, codec, NULL) < 0) {
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    int channels = codec_ctx->ch_layout.nb_channels > 0 ? codec_ctx->ch_layout.nb_channels : 2;
    int sample_rate = codec_ctx->sample_rate;

    double duration_sec = 0.0;
    if (st->duration != AV_NOPTS_VALUE) {
        duration_sec = st->duration * av_q2d(st->time_base);
    } else if (fmt_ctx->duration != AV_NOPTS_VALUE) {
        duration_sec = (double)fmt_ctx->duration / AV_TIME_BASE;
    }

    if (duration_sec > 0.0) {
        sf_count_t estimated_frames = (sf_count_t)(duration_sec * sample_rate);
        if (calculate_safe_history_capacity((size_t)estimated_frames, sample_rate,
                                            target_jack_rate) == 0) {
            avcodec_free_context(&codec_ctx);
            avformat_close_input(&fmt_ctx);
            return -2;
        }
    }

    size_t capacity = 1024 * 1024;
    float *raw_buf = malloc(capacity * (size_t)channels * sizeof(float));
    if (!raw_buf) {
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    size_t total_samples = 0;
    int actual_rate = sample_rate;

    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();

    while (av_read_frame(fmt_ctx, pkt) == 0) {
        if (pkt->stream_index == stream_idx) {
            if (avcodec_send_packet(codec_ctx, pkt) == 0) {
                if (drain_and_extract_frames(codec_ctx, frame, &raw_buf, &capacity, &total_samples,
                                             channels, &actual_rate) < 0) {
                    av_packet_unref(pkt);
                    goto loop_cleanup;
                }
            }
        }
        av_packet_unref(pkt);
    }

    if (avcodec_send_packet(codec_ctx, NULL) == 0) {
        if (drain_and_extract_frames(codec_ctx, frame, &raw_buf, &capacity, &total_samples,
                                     channels, &actual_rate) < 0)
            goto loop_cleanup;
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);

    *out_raw = raw_buf;
    *out_frames = (sf_count_t)(total_samples / (size_t)channels);
    *out_channels = channels;
    *out_rate = actual_rate;
    return 0;

loop_cleanup:
    av_frame_free(&frame);
    av_packet_free(&pkt);
    free(raw_buf);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    return -1;
}

static void resample_stream(SwrContext *swr_ctx, const float *in_data, sf_count_t in_frames,
                            int in_channels, float *out_data, size_t target_out_frames,
                            double ratio) {
    if (!swr_ctx || !in_data || !out_data)
        return;

    int chunk_size = 1000000;
    int max_out_chunk = (int)((double)chunk_size * ratio) + 8192;

    sf_count_t in_pos = 0;
    size_t out_pos = 0;

    while (in_pos < in_frames) {
        int in_count = (int)((in_frames - in_pos > (sf_count_t)chunk_size) ? chunk_size
                                                                           : (in_frames - in_pos));

        size_t space_left = target_out_frames - out_pos;
        if (space_left == 0)
            break;

        int out_count = (space_left > (size_t)max_out_chunk) ? max_out_chunk : (int)space_left;

        const uint8_t *in_ptr[1] = {(const uint8_t *)(in_data + (in_pos * in_channels))};
        uint8_t *out_ptr[1] = {(uint8_t *)(out_data + (out_pos * 2))};

        int ret = swr_convert(swr_ctx, out_ptr, out_count, in_ptr, in_count);
        if (ret < 0)
            break;

        in_pos += in_count;
        out_pos += (size_t)ret;
    }

    while (out_pos < target_out_frames) {
        size_t space_left = target_out_frames - out_pos;
        int out_count = (space_left > (size_t)max_out_chunk) ? max_out_chunk : (int)space_left;
        uint8_t *out_ptr[1] = {(uint8_t *)(out_data + (out_pos * 2))};

        int ret = swr_convert(swr_ctx, out_ptr, out_count, NULL, 0);
        if (ret <= 0)
            break;
        out_pos += (size_t)ret;
    }
}

bool is_video_file(const char *filepath) {
    if (!filepath)
        return false;
    const char *ext = strrchr(filepath, '.');
    if (!ext)
        return false;
    return (g_ascii_strcasecmp(ext, ".mkv") == 0 || g_ascii_strcasecmp(ext, ".mp4") == 0 ||
            g_ascii_strcasecmp(ext, ".webm") == 0);
}

bool is_audio_file(const char *filepath) {
    if (!filepath)
        return false;
    const char *ext = strrchr(filepath, '.');
    if (!ext)
        return false;
    return (g_ascii_strcasecmp(ext, ".flac") == 0 || g_ascii_strcasecmp(ext, ".wav") == 0 ||
            g_ascii_strcasecmp(ext, ".ogg") == 0 || g_ascii_strcasecmp(ext, ".mp3") == 0 ||
            g_ascii_strcasecmp(ext, ".m4a") == 0 || g_ascii_strcasecmp(ext, ".aiff") == 0);
}

/**
 * @brief Probes the FFmpeg streams of a media file to determine if a video stream is present.
 * Bypasses string/extension matching to guarantee accuracy across MP4, WebM, and MKV containers.
 * @param filepath Absolute path to the media file.
 * @return true if an AVMEDIA_TYPE_VIDEO stream is found, false otherwise.
 */
bool media_has_video(const char *filepath) {
    AVFormatContext *fmt_ctx = NULL;
    if (avformat_open_input(&fmt_ctx, filepath, NULL, NULL) != 0)
        return false;
    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        avformat_close_input(&fmt_ctx);
        return false;
    }
    bool has_video = false;
    for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            // Ignore embedded album art / attached cover pictures
            if (!(fmt_ctx->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
                has_video = true;
                break;
            }
        }
    }
    avformat_close_input(&fmt_ctx);
    return has_video;
}

int import_to_layer(const char *filepath, int track_idx, size_t start_frame, bool auto_advance) {
    if (!client || track_idx < 0 || track_idx >= QJ_MAX_TRACKS || pristine_frames == 0)
        return -1;

    // Check the undo ledger and session state. Only a brand-new blank canvas is untouched.
    bool canvas_was_untouched = (global_undo_ledger.count == 0 && !ui_state.is_existing_session);

    bool file_has_video = media_has_video(filepath);

    // MANDATORY TEARDOWN: Only tear down existing video if the incoming file replaces it with a new
    // video stream
    if (file_has_video) {
        abort_active_video_preview(false);
        video_start_frame = 0;
        video_end_frame = 0;
    }

    SF_INFO sfinfo = {0};
    float *raw_data = NULL;
    jack_nframes_t jack_rate = jack_get_sample_rate(client);

    SNDFILE *file = NULL;
    if (!g_str_has_suffix(filepath, ".mp3") && !g_str_has_suffix(filepath, ".MP3")) {
        file = sf_open(filepath, SFM_READ, &sfinfo);
    }

    if (file) {
        if (sfinfo.frames <= 0 || sfinfo.samplerate <= 0 || sfinfo.channels <= 0 ||
            sfinfo.channels > 64 ||
            (size_t)sfinfo.frames > SIZE_MAX / ((size_t)sfinfo.channels * sizeof(float))) {
            sf_close(file);
            return -3;
        }

        if (calculate_safe_history_capacity((size_t)sfinfo.frames, sfinfo.samplerate, jack_rate) ==
            0) {
            sf_close(file);
            return -2;
        }

        raw_data = calloc((size_t)sfinfo.frames * (size_t)sfinfo.channels, sizeof(float));
        if (!raw_data) {
            sf_close(file);
            return -2;
        }
        sf_readf_float(file, raw_data, sfinfo.frames);
        sf_close(file);
    } else {
        int rate = 0, channels = 0;
        sf_count_t frames = 0;
        int ret = load_audio_ffmpeg(filepath, &raw_data, &frames, &channels, &rate, jack_rate);
        if (ret != 0) {
            return ret;
        }
        sfinfo.frames = frames;
        sfinfo.channels = channels;
        sfinfo.samplerate = rate;
    }

    if (sfinfo.frames == 0 || sfinfo.samplerate == 0) {
        if (raw_data)
            free(raw_data);
        return -3;
    }

    double ratio = (double)jack_rate / (double)sfinfo.samplerate;
    size_t resampled_frames = (size_t)(sfinfo.frames * ratio);

    if (start_frame + resampled_frames > pristine_frames) {
        int required_seconds = (int)((start_frame + resampled_frames) / jack_rate) + 1;
        int resize_result = resize_loop_canvas_seconds(required_seconds);
        if (resize_result != 0) {
            if (raw_data)
                free(raw_data);
            return resize_result;
        }
    }

    float *src_out = calloc(resampled_frames * 2, sizeof(float));

    if (!src_out) {
        if (raw_data)
            free(raw_data);
        return -2;
    }

    if ((int)jack_rate == sfinfo.samplerate && (sfinfo.channels == 2 || sfinfo.channels == 1)) {
        if (sfinfo.channels == 2) {
            memcpy(src_out, raw_data, resampled_frames * 2 * sizeof(float));
        } else {
            for (size_t f = 0; f < resampled_frames; f++) {
                src_out[f * 2] = raw_data[f];
                src_out[f * 2 + 1] = raw_data[f];
            }
        }
    } else {
        SwrContext *swr_ctx = swr_alloc();
        AVChannelLayout in_ch_layout, out_ch_layout;
        av_channel_layout_default(&in_ch_layout, sfinfo.channels);
        av_channel_layout_default(&out_ch_layout, 2);

        av_opt_set_chlayout(swr_ctx, "in_chlayout", &in_ch_layout, 0);
        av_opt_set_int(swr_ctx, "in_sample_rate", sfinfo.samplerate, 0);
        av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt", AV_SAMPLE_FMT_FLT, 0);

        av_opt_set_chlayout(swr_ctx, "out_chlayout", &out_ch_layout, 0);
        av_opt_set_int(swr_ctx, "out_sample_rate", jack_rate, 0);
        av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt", AV_SAMPLE_FMT_FLT, 0);

        swr_init(swr_ctx);
        resample_stream(swr_ctx, raw_data, sfinfo.frames, sfinfo.channels, src_out,
                        resampled_frames, ratio);
        swr_free(&swr_ctx);
    }

    free(raw_data);

    int truncated = 0;
    size_t frames_to_copy = resampled_frames;

    if (start_frame >= pristine_frames) {
        free(src_out);
        return -2;
    }

    if (start_frame + resampled_frames > pristine_frames) {
        frames_to_copy = pristine_frames - start_frame;
        truncated = 1;
    }

    if (!await_rt_thread_detach()) {
        free(src_out);
        return -1;
    }

    if (start_frame >= pristine_frames) {
        resume_rt_thread();
        free(src_out);
        return -2;
    }

    current_transaction_id++;
    push_undo_state(track_idx, current_transaction_id);

    if (file_has_video) {
        mark_current_transaction_as_video();
        float current_speed =
            atomic_load_explicit(&get_transport_state()->playback_speed, memory_order_relaxed);
        if (current_speed <= 0.0f)
            current_speed = 1.0f;
        video_start_frame = (size_t)((double)start_frame / (double)current_speed);
        video_end_frame = (size_t)((double)(start_frame + frames_to_copy) / (double)current_speed);
        video_track_index = track_idx; // Dynamically link the video to this specific UI row
    }

    int current_timebase = atomic_load_explicit(&timebase_track, memory_order_acquire);
    if (!atomic_load_explicit(&master_tracks[current_timebase].has_audio, memory_order_acquire)) {
        atomic_store_explicit(&timebase_track, track_idx, memory_order_release);
        current_timebase = track_idx;
    }

    // Splicing in-place instead of replacing the layer

    // Paste the file audio with exact sample-accuracy (No more 48-sample gap!)
    size_t actual_frames_to_copy = frames_to_copy;
    if (start_frame + actual_frames_to_copy > pristine_frames) {
        actual_frames_to_copy = pristine_frames - start_frame;
    }

    bool use_soft_clip = (ui_state.config.overlay_soft_clip != 0);

    for (size_t i = 0; i < actual_frames_to_copy * 2; i++) {
        if (use_soft_clip) {
            master_tracks[track_idx].active_buffer[(start_frame * 2) + i] = soft_clip(
                master_tracks[track_idx].active_buffer[(start_frame * 2) + i] + src_out[i]);
        } else {
            master_tracks[track_idx].active_buffer[(start_frame * 2) + i] += src_out[i];
        }
    }
    free(src_out);

    // Burn the color parity marker directly adjacent to the previous clip
    if (master_tracks[track_idx].parity_map) {
        uint8_t last_parity = 0;
        if (start_frame > 0) {
            last_parity = master_tracks[track_idx].parity_map[start_frame - 1];
        }
        uint8_t next_parity = (last_parity == 0 || last_parity % 2 == 0) ? 1 : 2;
        memset(&master_tracks[track_idx].parity_map[start_frame], next_parity,
               actual_frames_to_copy * sizeof(uint8_t));
    }

    atomic_store_explicit(&stretcher_flush_request, true, memory_order_release);

    int active = atomic_load_explicit(&active_track_count, memory_order_acquire);
    if (track_idx >= active) {
        atomic_store_explicit(&active_track_count, track_idx + 1, memory_order_release);
    }

    atomic_store_explicit(&master_tracks[track_idx].has_audio, true, memory_order_release);

    if (auto_advance && track_idx < QJ_MAX_TRACKS - 1) {
        int next_track = track_idx + 1;
        atomic_store_explicit(&current_recording_track, next_track, memory_order_release);
        atomic_store_explicit(&selected_tracks_mask, (1 << next_track), memory_order_release);
        int current_active = atomic_load_explicit(&active_track_count, memory_order_acquire);
        if (current_active <= next_track) {
            atomic_store_explicit(&active_track_count, next_track + 1, memory_order_release);
        }
    } else {
        // Keep focus safely on the track the user just imported to
        atomic_store_explicit(&current_recording_track, track_idx, memory_order_release);
        atomic_store_explicit(&selected_tracks_mask, (1 << track_idx), memory_order_release);
    }

    resume_rt_thread();
    atomic_store_explicit(&cached_audible_frames, get_last_audible_frame(), memory_order_release);

    // SINGLE-FILE WORKFLOW: Clear the dirty flag if this was the first audio imported to the canvas
    if (canvas_was_untouched) {
        mark_session_saved();
    }

    char *base_name = g_path_get_basename(filepath);
    log_and_notify(0,
                   "[IO-DECODE] Loaded: \"%s\" | Track %d | %dHz -> %dHz (Ratio=%.3f) | Frames=%zu "
                   "| Status=%s\n",
                   base_name, track_idx, sfinfo.samplerate, jack_rate, ratio, resampled_frames,
                   truncated ? "TRUNCATED" : "OK");
    g_free(base_name);

    return truncated ? 1 : 0;
}

void export_mixdown_thread(GTask *task, gpointer source_object, gpointer task_data,
                           GCancellable *cancellable) {
    (void)source_object;
    (void)cancellable;
    ExportTaskData *data = (ExportTaskData *)task_data;

    size_t frames_to_render = data->end_f - data->start_f;

    if (frames_to_render > 0) {
        data->mix_buf_ptr = render_mixdown_region(data->start_f, data->end_f);

        if (data->mix_buf_ptr) {
            g_task_return_boolean(task, TRUE);
            return;
        }
    }
    g_task_return_boolean(task, FALSE);
}

typedef struct {
    time_t start_time;
    int max_duration;
} FFmpegProbeTimeout;

static int probe_interrupt_cb(void *ctx) {
    FFmpegProbeTimeout *timeout = (FFmpegProbeTimeout *)ctx;
    if (time(NULL) - timeout->start_time > timeout->max_duration) {
        return 1;
    }
    return 0;
}

bool extract_track_metadata(const char *filepath, char *output_buffer, size_t max_len) {
    bool is_native = false;

    if (g_str_has_suffix(filepath, ".qjams")) {
        gchar *basename = g_path_get_basename(filepath);
        char *dot = strrchr(basename, '.');
        if (dot)
            *dot = '\0';
        snprintf(output_buffer, max_len, "%s", basename);
        g_free(basename);
        return true;
    }

    AVFormatContext *fmt_ctx = avformat_alloc_context();
    FFmpegProbeTimeout timeout = {.start_time = time(NULL), .max_duration = 3};
    fmt_ctx->interrupt_callback.callback = probe_interrupt_cb;
    fmt_ctx->interrupt_callback.opaque = &timeout;

    if (avformat_open_input(&fmt_ctx, filepath, NULL, NULL) == 0) {
        avformat_find_stream_info(fmt_ctx, NULL);

        AVDictionaryEntry *title = NULL;
        AVDictionaryEntry *artist = NULL;
        AVDictionaryEntry *tag = NULL;

        while ((tag = av_dict_get(fmt_ctx->metadata, "", tag, AV_DICT_IGNORE_SUFFIX))) {
            if (g_str_has_prefix(tag->value, "Recorded with QJams"))
                is_native = true;
            if (g_ascii_strcasecmp(tag->key, "title") == 0)
                title = tag;
            if (g_ascii_strcasecmp(tag->key, "artist") == 0)
                artist = tag;
        }

        if (!is_native) {
            for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
                tag = NULL;
                while ((tag = av_dict_get(fmt_ctx->streams[i]->metadata, "", tag,
                                          AV_DICT_IGNORE_SUFFIX))) {
                    if (g_str_has_prefix(tag->value, "Recorded with QJams"))
                        is_native = true;
                }
            }
        }

        int duration_sec = 0;
        if (fmt_ctx->duration != AV_NOPTS_VALUE) {
            duration_sec = fmt_ctx->duration / AV_TIME_BASE;
        }

        if (title && artist) {
            snprintf(output_buffer, max_len, "%s - %s [%02d:%02d]", artist->value, title->value,
                     duration_sec / 60, duration_sec % 60);
        } else if (title) {
            snprintf(output_buffer, max_len, "%s [%02d:%02d]", title->value, duration_sec / 60,
                     duration_sec % 60);
        } else {
            gchar *basename = g_path_get_basename(filepath);
            snprintf(output_buffer, max_len, "%s", basename);
            g_free(basename);
        }

        avformat_close_input(&fmt_ctx);
    } else {
        gchar *basename = g_path_get_basename(filepath);
        snprintf(output_buffer, max_len, "%s", basename);
        g_free(basename);
    }

    gchar *valid_utf8 = g_utf8_make_valid(output_buffer, -1);
    strncpy(output_buffer, valid_utf8, max_len - 1);
    output_buffer[max_len - 1] = '\0';
    g_free(valid_utf8);

    return is_native;
}
