#ifndef MEDIA_IO_H
#define MEDIA_IO_H

#include <gtk/gtk.h>
#include <jack/jack.h>
#include <stdbool.h>

// --- EXPORT TASK PAYLOAD ---
typedef struct {
    char dir[1024];
    char base_no_ext[1024];
    char date_str[32];
    float *mix_buf_ptr;
    size_t start_f;
    size_t end_f;
    size_t video_end_f;
    int samplerate;
    bool has_video;
} ExportTaskData;

// --- EXPORT API ---
void export_mixdown_thread(GTask *task, gpointer source_object, gpointer task_data,
                           GCancellable *cancellable);

// --- SESSION I/O API ---
int save_qjams_session(const char *filepath);
int load_qjams_session(const char *filepath);

// --- MEDIA IMPORT API ---
int import_to_layer(const char *filepath, int track_idx, size_t start_frame, bool auto_advance);
bool extract_track_metadata(const char *filepath, char *output_buffer, size_t max_len);
bool media_has_video(const char *filepath);
bool is_video_file(const char *filepath);
bool is_audio_file(const char *filepath);

#endif
