#ifndef QJAMS_SCANNER_H
#define QJAMS_SCANNER_H

#include <jack/jack.h>
#include <jack/types.h>
#include <stdint.h>

#define MAX_CAMERAS 8
#define MAX_AUDIO_DEVICES 16

typedef struct {
    char device_path[32]; // e.g., "/dev/video1"
    char device_name[64]; // e.g., "Logitech Webcam C925e"
} VideoDevice;

typedef struct {
    char device_id[128];    // e.g., "alsa_input.usb-Neural_DSP_Quad_Cortex..."
    char display_name[128]; // Mapped UI name
    int channel_count;      // Used for Mono vs Stereo routing
} AudioDevice;

/**
 * @brief Scans the system for available V4L2 video capture devices.
 * @param devices Array to store the discovered video devices.
 * @param max_devices Maximum number of devices to scan.
 * @return The number of discovered video devices.
 */
int scan_video_devices(VideoDevice *devices, int max_devices);
/**
 * @brief Scans the JACK server for physical audio capture devices.
 * @param client Pointer to the active JACK client.
 * @param devices Array to store the discovered audio devices.
 * @param max_devices Maximum number of devices to scan.
 * @return The number of discovered audio devices.
 */
int scan_audio_devices(jack_client_t *client, AudioDevice *devices, int max_devices);
/**
 * @brief Scans the JACK server for physical audio playback devices.
 * @param client Pointer to the active JACK client.
 * @param devices Array to store the discovered playback devices.
 * @param max_devices Maximum number of devices to scan.
 * @return The number of discovered playback devices.
 */
int scan_playback_devices(jack_client_t *client, AudioDevice *devices, int max_devices);

/**
 * @brief Queries the highest supported compatible format (MJPEG or YUYV) and resolution for a device.
 * @param device_path The video device node (e.g., /dev/video0).
 * @param out_pixelformat Pointer to store the selected V4L2_PIX_FMT.
 * @param out_width Pointer to store the selected width.
 * @param out_height Pointer to store the selected height.
 * @return 1 on success finding a compatible format, 0 on failure.
 */
int get_best_video_format(const char* device_path, uint32_t *out_pixelformat, int *out_width, int *out_height);

#endif // QJAMS_SCANNER_H
