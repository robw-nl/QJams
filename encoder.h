#ifndef QJAMS_ENCODER_H
#define QJAMS_ENCODER_H

#include <libavcodec/avcodec.h>
#include <stdatomic.h>
#include <stdbool.h>
#include "video_ringbuffer.h"

extern AVCodecContext *video_enc_ctx;
extern _Atomic bool encoder_disk_error;

// Public API
/**
 * @brief Initializes hardware/software encoding via discovery ladder and starts the background thread.
 * @param output_filename The target output MKV file path.
 * @param width The width of the encoded video.
 * @param height The height of the encoded video.
 * @param sample_rate The dynamic hardware audio sample rate from JACK.
 * @param queue Pointer to the lock-free video frame queue.
 * @return 0 on success, -1 on failure.
 */
int init_and_start_encoder(const char* output_filename, int width, int height, int sample_rate, SPSC_Video_Queue* queue);
/**
 * @brief Signals the encoder thread to stop, flushes remaining frames, and frees contexts.
 * @return void
 */
void stop_encoder(void);
/**
 * @brief Wakes the encoder thread from sleep without waiting for a timeout.
 */
void wake_encoder(void);

#endif // QJAMS_ENCODER_H
