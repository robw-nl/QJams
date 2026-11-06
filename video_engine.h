#ifndef QJAMS_VIDEO_ENGINE_H
#define QJAMS_VIDEO_ENGINE_H

#include "video_ringbuffer.h"
#include <jack/types.h>
#include <stdbool.h>

/**
 * @brief Initializes the V4L2 device and starts the video capture thread.
 * @param device_path The hardware path of the video device (e.g., /dev/video0).
 * @param queue Pointer to the lock-free queue for routing frames to the encoder.
 * @return 0 on success, -1 on failure.
 */
int init_and_start_video_engine(const char *device_path, SPSC_Video_Queue *queue);

/**
 * @brief Checks if the V4L2 capture thread is actively running.
 * @return true if the video thread is active, false otherwise.
 */
bool is_video_engine_running(void);

/**
 * @brief Shuts down the video capture thread and releases V4L2 hardware resources.
 */
void stop_video_engine(void);

/** Global hardware capture dimensions exposed for the FFmpeg encoder pipeline */
extern int capture_width;
extern int capture_height;

void launch_mkv_video_preview(const char *filepath);
void abort_active_video_preview(bool stash);
void force_clear_mkv_video_preview(void);
void restore_stashed_video_preview(void);
void clear_video_stash_async(void);

#endif // QJAMS_VIDEO_ENGINE_H
