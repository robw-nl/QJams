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
extern atomic_size_t backing_track_frames;
extern atomic_size_t playback_pos;

// Multi-Track Looper Exports
#define MAX_TRACKS 12
extern float *multitrack_tracks[MAX_TRACKS];
extern float *undo_tracks[MAX_TRACKS];
extern _Atomic int active_track_count;
extern _Atomic int current_recording_track;
extern _Atomic bool track_is_soloed[MAX_TRACKS];
extern _Atomic float multitrack_track_gains[MAX_TRACKS];

extern atomic_int active_sample_rate;
extern _Atomic float playback_speed;
extern _Atomic bool is_multitrack_mode; // NEW: Controls video encoder bypass

extern _Atomic bool request_track_free;
extern _Atomic bool safe_to_free_track;

extern _Atomic bool seek_flag;
extern _Atomic double seek_target;

bool await_rt_thread_detach(void);
void resume_rt_thread(void);

extern _Atomic bool track_has_audio[MAX_TRACKS];

/**
 * @brief Master functions applying synchronized cuts and fades across all active layers.
 */
void master_cut_selection(void);
void master_smart_fade(void);
void master_blend_fade(void);
void master_undo_edits(void);

/**
 * @brief Clears a layer to absolute silence (backing up to undo) or restores it if already empty.
 * @param track_idx The target layer index.
 * @return 0 if cleared, 1 if restored, -1 on error.
 */
int clear_or_restore_track(int track_idx);
/**
 * @brief Advances the looper to the next overdub layer.
 * @return void
 */
void multitrack_next_track(void);
/**
 * @brief Undoes the last recorded layer and decrements the active layer count.
 * @return void
 */
void multitrack_undo_track(void);

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
void set_multitrack_layer_gain(int track_idx, float multiplier);
/**
 * @brief Loads an audio file, resamples it to match the JACK sample rate, and initializes the RubberBand stretcher.
 * @param filepath The absolute path to the audio file.
 * @param client_ptr Pointer to the active JACK client.
 * @return 0 on success, or -1 if the file could not be read or processed.
 */
int load_backing_track(const char* filepath, jack_client_t* client_ptr);
/**
 * @brief Decodes an audio file and safely imports it into a specific multitrack layer.
 * Enforces Strict Bounding: truncates long files and zero-pads short files to match pristine_frames.
 * @param filepath The absolute path to the audio file.
 * @param track_idx The target layer index (0 to MAX_TRACKS - 1).
 * @return 0 on success, -1 on format error, -2 on memory failure.
 */
int import_to_layer(const char* filepath, int track_idx);
/**
 * @brief Initializes a blank multi-track loop canvas of a specified duration.
 * @param duration_seconds The length of the blank canvas in seconds.
 * @return 0 on success, -2 on memory failure.
 */
int init_empty_loop_canvas(int duration_seconds);
// FILE: audio_engine.h
/**
 * @brief Initializes a multi-track loop canvas of an exact frame count.
 * @param exact_frames The exact length of the canvas in frames.
 * @return 0 on success, -2 on memory failure.
 */
int init_exact_loop_canvas(size_t exact_frames);
/**
 * @brief Non-destructively resizes the active recording canvas.
 * @param duration_seconds The new length of the canvas in seconds.
 * @return 0 on success, -1 on timeout, -2 on memory failure.
 */
int resize_loop_canvas_seconds(int duration_seconds);
/**
 * @brief Registers the JACK client, sets callbacks, and initializes lock-free SPSC queues.* @brief Registers the JACK client, sets callbacks, and initializes lock-free SPSC queues.
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
void clear_loop_points(void);

/**
 * @brief Instantly zeroes out the memory for a specific looper overdub layer.
 * @param track_idx The index of the layer to clear.
 * @return void
 */
void clear_multitrack_track(int track_idx);

/**
 * @brief Swaps the memory pointers of two looper layers, allowing for latency-free reordering.
 * @param idx_a The first layer index.
 * @param idx_b The second layer index.
 * @return void
 */
void swap_multitrack_tracks(int idx_a, int idx_b);

/**
 * @brief Atomically swaps a multitrack layer with its dedicated A/B undo buffer.
 * @param track_idx The index of the layer to swap.
 * @return void
 */
void swap_undo_track(int track_idx);

/**
 * @brief Extracts the selected loop region from a layer and shifts the remaining audio left.
 * @param track_idx The index of the layer to cut.
 * @return void
 */
void cut_multitrack_track_selection(int track_idx);

/**
 * @brief Applies a smart destructive fade (In, Out, or V-Duck) to the active loop selection.
 * @param track_idx The index of the layer to fade.
 * @return void
 */
void apply_smart_fade_to_track(int track_idx);


/**
 * @brief Allows fade points to blend instead of touch
 * @param track_idx The index of the layer to restore.
 * @return void
 */
void apply_blend_fade_to_track(int track_idx);

/**
 * @brief Restores a track's audio buffer from its undo snapshot.
 * @param track_idx The index of the track to restore.
 * @return void
 */
void undo_track_edit(int track_idx);

void backup_track_for_edit(int track_idx);
void amplify_track_selection(int track_idx, float db_delta);

#endif
