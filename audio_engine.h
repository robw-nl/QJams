#ifndef AUDIO_ENGINE_H
#define AUDIO_ENGINE_H

#include <jack/jack.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdalign.h>

extern _Atomic bool engine_is_recording;
extern _Atomic bool engine_is_armed;
extern _Atomic bool engine_is_paused;
extern _Atomic bool engine_is_playing;

extern _Atomic float vu_peak_input_l;
extern _Atomic float vu_peak_input_r;
extern _Atomic float vu_peak_bt_l;
extern _Atomic float vu_peak_bt_r;

// --- Waveform Data Exports ---
extern float *pristine_bt_buf;
extern size_t pristine_frames;
extern float *backing_track_buf;
extern atomic_size_t backing_track_frames;
extern atomic_size_t playback_pos;

// Multi-Track Multitrack Exports
#define MAX_TRACKS 12

extern float *clipboard_buffers[MAX_TRACKS];
extern size_t clipboard_frames;
extern _Atomic uint32_t selected_tracks_mask;

typedef struct {
    alignas(64) float *active_buffer;
    float *undo_buffer;
    _Atomic float gain;
    _Atomic bool is_soloed;
    _Atomic bool has_audio;
    _Atomic bool has_undo;
    _Atomic bool undo_has_audio;
    char name[64];
} QJTrackBuffer;

extern QJTrackBuffer master_tracks[MAX_TRACKS];

extern _Atomic int active_track_count;
extern _Atomic int current_recording_track;

extern atomic_int active_sample_rate;
extern _Atomic float playback_speed;

extern _Atomic uint32_t rt_dropped_audio_frames;
extern _Atomic uint32_t rt_cmd_queue_full;

extern _Atomic bool request_track_free;
extern _Atomic bool safe_to_free_track;

extern _Atomic bool seek_flag;
extern _Atomic double seek_target;

bool await_rt_thread_detach(void);
void resume_rt_thread(void);

int clear_or_restore_track(int track_idx);
void multitrack_next_track(void);
void multitrack_undo_track(void);
void reset_backing_track_position(void);
void seek_backing_track(double fraction);
void set_playback_speed(float speed);
void set_input_gain(float input_gain_multiplier);
void set_bt_gain(float multiplier);
void set_multitrack_layer_gain(int track_idx, float multiplier);
int load_backing_track(const char* filepath, jack_client_t* client_ptr);
int import_to_layer(const char* filepath, int track_idx);
int init_empty_loop_canvas(int duration_seconds);
int init_exact_loop_canvas(size_t exact_frames);
int resize_loop_canvas_seconds(int duration_seconds);
int init_audio_engine(size_t queue_capacity, float init_input_gain, jack_client_t* existing_client, const char* target_audio_device, const char* target_playback_device);
void shutdown_audio_engine(void);
int patch_audio_ports(const char* target_audio_device);
int patch_playback_ports(const char* target_playback_device);

extern _Atomic bool loop_active;
extern _Atomic size_t loop_start_frame;
extern _Atomic size_t loop_end_frame;

void set_loop_points(double start_frac, double end_frac);
void clear_loop_points(void);
void swap_multitrack_tracks(int idx_a, int idx_b);
void clear_multitrack_track(int track_idx);

// True Multi-Select DAW Operations
void copy_selection(void);
void cut_selection(void);
void paste_selection(void);
void global_paste_selection(void);
void delete_selection(void);
void global_ripple_delete_selection(void);
void apply_smart_fade_selection(void);
void apply_blend_fade_selection(void);
void undo_selection(void);

float* render_mixdown_region(size_t start, size_t end);

#endif // AUDIO_ENGINE_H
