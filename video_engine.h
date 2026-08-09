#ifndef QJAMS_VIDEO_ENGINE_H
#define QJAMS_VIDEO_ENGINE_H

#include "video_ringbuffer.h"
#include <jack/types.h>

/**
 * @brief Initializes the V4L2 device and starts the video capture thread.
 * @param device_path The hardware path of the video device (e.g., /dev/video0).
 * @param queue Pointer to the lock-free queue for routing frames to the encoder.
 * @return 0 on success, -1 on failure.
 */
int init_and_start_video_engine(const char* device_path, SPSC_Video_Queue* queue);
/**
 * @brief Shuts down the video capture thread and releases V4L2 hardware resources.
 * @return void
 */
void stop_video_engine();

#endif // QJAMS_VIDEO_ENGINE_H
