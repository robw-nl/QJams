#ifndef AUDIO_ENGINE_H
#define AUDIO_ENGINE_H

#include <jack/jack.h>
#include <stdatomic.h>
#include <stdbool.h>

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
extern float *recorded_input_buf;
extern float *recorded_bt_buf; // Captures live fader sweeps
extern atomic_size_t backing_track_frames;
extern atomic_size_t playback_pos;

// Multi-Track Looper Exports
#define MAX_LOOPS 6
extern float *loop_layers[MAX_LOOPS];
extern _Atomic bool layer_is_muted[MAX_LOOPS];
extern _Atomic int active_layer_count;
extern _Atomic int current_recording_layer;
extern _Atomic bool layer_is_soloed[MAX_LOOPS];

extern atomic_int active_sample_rate;
extern _Atomic float playback_speed;
extern _Atomic bool is_looper_mode; // NEW: Controls video encoder bypass

extern _Atomic bool request_track_free;
extern _Atomic bool safe_to_free_track;

extern _Atomic bool seek_flag;
extern _Atomic double seek_target;

/**
 * @brief Advances the looper to the next overdub layer.
 * @return void
 */
void looper_next_layer(void);
/**
 * @brief Undoes the last recorded layer and decrements the active layer count.
 * @return void
 */
void looper_undo_layer(void);

/**
 * @brief Resets the backing track playhead to the beginning and clears the active recording buffer.
 * @return void
 */
void reset_backing_track_position(void);
/**
 * @brief Moves the playback position to a specific fraction of the track length.
 * @param fraction The target position as a decimal (0.0 to 1.0).
 * @return void
 */
void seek_backing_track(double fraction);
/**
 * @brief Adjusts the time-stretch ratio of the backing track.
 * @param speed The linear speed multiplier (e.g., 1.0 is normal, 0.5 is half speed).
 * @return void
 */
void set_playback_speed(float speed);
/**
 * @brief Updates the linear gain multiplier for the Quad Cortex input.
 * @param input_gain_multiplier The computed linear gain value.
 * @return void
 */
void set_input_gain(float input_gain_multiplier);
/**
 * @brief Updates the linear gain multiplier for the backing track.
 * @param multiplier The computed linear gain value.
 * @return void
 */
void set_bt_gain(float multiplier);

/**
 * @brief Loads an audio file, resamples it to match the JACK sample rate, and initializes the RubberBand stretcher.
 * @param filepath The absolute path to the audio file.
 * @param client_ptr Pointer to the active JACK client.
 * @return 0 on success, or -1 if the file could not be read or processed.
 */
int load_backing_track(const char* filepath, jack_client_t* client_ptr);
/**
 * @brief Initializes a blank multi-track loop canvas of a specified duration.
 * @param duration_seconds The length of the blank canvas in seconds.
 * @return 0 on success, -2 on memory failure.
 */
int init_empty_loop_canvas(int duration_seconds);
/**
 * @brief Initializes a multi-track loop canvas of an exact frame count.
 * @param exact_frames The exact length of the canvas in frames.
 * @return 0 on success, -2 on memory failure.
 */
int init_exact_loop_canvas(size_t exact_frames);
/**
 * @brief Registers the JACK client, sets callbacks, and initializes lock-free SPSC queues.
 * @param queue_capacity The power-of-two size limit for the audio ringbuffer.
 * @param init_input_gain The initial gain multiplier for the Quad Cortex.
 * @param existing_client Pointer to a pre-opened JACK client.
 * @param target_audio_device The name identifier for the physical capture hardware.
 * @param target_playback_device The name identifier for the physical playback hardware.
 * @return 0 on success, 1 on JACK failure, 2 on port patching failure.
 */
int init_audio_engine(size_t queue_capacity, float init_input_gain, jack_client_t* existing_client, const char* target_audio_device, const char* target_playback_device);
/**
 * @brief Closes the JACK client and frees all allocated DSP buffers, queues, and states.
 * @return void
 */
void shutdown_audio_engine(void);
/**
 * @brief Connects physical USB hardware inputs to the internal QJams capture ports.
 * @param target_audio_device The name identifier of the hardware to connect.
 * @return 0 on success, 2 if the ports could not be located or routed.
 */
int patch_audio_ports(const char* target_audio_device);
/**
 * @brief Connects QJams output ports to the physical hardware playback ports.
 * @param target_playback_device The name identifier of the hardware to connect.
 * @return 0 on success, 2 if the ports could not be located or routed.
 */
int patch_playback_ports(const char* target_playback_device);

// Looper section
extern _Atomic bool loop_active;
extern _Atomic size_t loop_start_frame;
extern _Atomic size_t loop_end_frame;

/**
 * @brief Sets the A/B frame boundaries for continuous looped playback.
 * @param start_frac The starting position as a decimal (0.0 to 1.0).
 * @param end_frac The ending position as a decimal (0.0 to 1.0).
 * @return void
 */
void set_loop_points(double start_frac, double end_frac);
/**
 * @brief Disables the active loop boundaries and restores linear playback.
 * @return void
 */
void clear_loop_points(void);;

/**
 * @brief Instantly zeroes out the memory for a specific looper overdub layer.
 * @param layer_idx The index of the layer to clear.
 * @return void
 */
void clear_looper_layer(int layer_idx);

/**
 * @brief Swaps the memory pointers of two looper layers, allowing for latency-free reordering.
 * @param idx_a The first layer index.
 * @param idx_b The second layer index.
 * @return void
 */
void swap_looper_layers(int idx_a, int idx_b);

#endif
