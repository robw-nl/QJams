#ifndef AUDIO_CORE_H
#define AUDIO_CORE_H

#include "ringbuffer.h"
#include "video_ringbuffer.h"
#include <jack/jack.h>
#include <rubberband/rubberband-c.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stdbool.h>

typedef struct {
    _Atomic bool is_recording;
    _Atomic bool is_armed;
    _Atomic bool is_paused;
    _Atomic bool is_playing;
    atomic_size_t backing_track_frames;
    alignas(64) atomic_size_t playback_pos;
    atomic_int active_sample_rate;
    _Atomic float playback_speed;
} QJTransportState;

QJTransportState *get_transport_state(void);

extern _Atomic bool audio_available;
extern jack_client_t *client;
extern jack_port_t *input_port_1;
extern jack_port_t *input_port_2;
extern jack_port_t *output_port_1;
extern jack_port_t *output_port_2;
extern SPSC_Audio_Queue audio_queue;
extern SPSC_Video_Queue video_queue;
extern SPSC_Stereo_Queue stretch_queue;
extern SPSC_Command_Queue cmd_queue;
extern int event_pipe[2];

extern _Atomic float vu_peak_input_l;
extern _Atomic float vu_peak_input_r;
extern _Atomic float vu_peak_bt_l;
extern _Atomic float vu_peak_bt_r;

// --- Waveform Data Exports ---
extern _Atomic int active_track_count;
extern _Atomic int current_recording_track;
extern _Atomic int timebase_track;

extern _Atomic uint32_t rt_dropped_audio_frames;


extern _Atomic bool seek_flag;
extern _Atomic double seek_target;
extern _Atomic bool loop_wrap_pending;
extern _Atomic bool engine_is_shutting_down;


void seek_backing_track(double fraction);
void set_playback_speed(float speed);
void set_input_gain(float input_gain_multiplier);
void set_bt_gain(float multiplier);
void set_multitrack_layer_gain(int track_idx, float multiplier);
int init_audio_engine(size_t queue_capacity, float init_input_gain, jack_client_t *existing_client,
                      const char *target_audio_device, const char *target_playback_device);
int reboot_audio_engine(const char *target_audio_device, const char *target_playback_device,
                        float input_gain_multiplier);
void shutdown_audio_engine(void);
int get_jack_event_fd(void);
float soft_clip(float x);

extern _Atomic bool stretcher_flush_request;
extern RubberBandState rt_rb_state;
extern atomic_size_t pristine_read_pos;

extern _Atomic bool loop_active;
extern _Atomic size_t loop_start_frame;
extern _Atomic size_t loop_end_frame;

void set_loop_points(double start_frac, double end_frac);
void clear_loop_points(void);


typedef struct {
    uint32_t dropped_frames;
    int sample_rate;
    int ledger_undos;
    int ledger_redos;
    int ledger_capacity;
    size_t qjams_rss_bytes;
    size_t available_ram_bytes;
    size_t total_ram_bytes;
} QJEngineTelemetry;

void get_engine_telemetry(QJEngineTelemetry *out_stats);

extern _Atomic size_t cached_audible_frames;
void trigger_audible_scan_async(void);

size_t get_last_audible_frame(void);
size_t get_last_recorded_frame(void);

#endif // AUDIO_CORE_H
