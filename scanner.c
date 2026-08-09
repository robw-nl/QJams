#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>

#include "scanner.h"

/**
 * @brief Scans the system for available V4L2 video capture devices.
 * @param devices Array to store the discovered video devices.
 * @param max_devices Maximum number of devices to scan.
 * @return The number of discovered video devices.
 */
int scan_video_devices(VideoDevice *devices, int max_devices) {
    int count = 0;

    for (int i = 0; i < 64 && count < max_devices; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/video%d", i);

        // Suppress error output, as we expect many of these opens to fail
        int temp_fd = open(path, O_RDWR | O_NONBLOCK, 0);
        if (temp_fd == -1) continue;

        struct v4l2_capability cap;
        if (ioctl(temp_fd, VIDIOC_QUERYCAP, &cap) != -1) {
            uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;

            if (caps & V4L2_CAP_VIDEO_CAPTURE) {
                strncpy(devices[count].device_path, path, sizeof(devices[count].device_path) - 1);
                strncpy(devices[count].device_name, (char *)cap.card, sizeof(devices[count].device_name) - 1);

                printf("Discovered Camera: %s at %s\n", devices[count].device_name, devices[count].device_path);
                count++;
            }
        }
        close(temp_fd);
    }

    return count;
}

/**
 * @brief Internal helper to scan the JACK server for physical audio ports matching specific flags.
 * Centralizes the port string tokenization and deduplication logic.
 * @param client Pointer to the active JACK client.
 * @param devices Array to store the discovered audio devices.
 * @param max_devices Maximum number of devices to scan.
 * @param port_flags JACK port flags (e.g., JackPortIsInput or JackPortIsOutput).
 * @return The number of discovered audio devices.
 */
static int scan_jack_ports(jack_client_t *client, AudioDevice *devices, int max_devices, unsigned long port_flags) {
    if (!client) return 0;
    int count = 0;

    const char **ports = jack_get_ports(client, NULL, NULL, port_flags);

    if (ports) {
        for (int i = 0; ports[i] != NULL && count < max_devices; i++) {
            char prefix[128] = {0};

            const char *colon = strchr(ports[i], ':');
            if (colon) {
                int len = colon - ports[i];
                if (len >= (int)sizeof(prefix)) len = sizeof(prefix) - 1;
                strncpy(prefix, ports[i], len);
            } else {
                strncpy(prefix, ports[i], sizeof(prefix) - 1);
            }

            int found = 0;
            for (int j = 0; j < count; j++) {
                if (strcmp(devices[j].device_id, prefix) == 0) {
                    devices[j].channel_count++;
                    found = 1;
                    break;
                }
            }

            if (!found) {
                strncpy(devices[count].device_id, prefix, sizeof(devices[count].device_id) - 1);
                strncpy(devices[count].display_name, prefix, sizeof(devices[count].display_name) - 1);
                devices[count].channel_count = 1;
                count++;
            }
        }
        jack_free(ports);
    }
    return count;
}

/**
 * @brief Scans the JACK server for physical audio capture devices.
 * @param client Pointer to the active JACK client.
 * @param devices Array to store the discovered audio devices.
 * @param max_devices Maximum number of devices to scan.
 * @return The number of discovered audio devices.
 */
int scan_audio_devices(jack_client_t *client, AudioDevice *devices, int max_devices) {
    // Capture all hardware and software loopbacks that output audio into JACK
    return scan_jack_ports(client, devices, max_devices, JackPortIsOutput);
}

/**
 * @brief Scans the JACK server for physical audio playback devices.
 * @param client Pointer to the active JACK client.
 * @param devices Array to store the discovered playback devices.
 * @param max_devices Maximum number of devices to scan.
 * @return The number of discovered playback devices.
 */
int scan_playback_devices(jack_client_t *client, AudioDevice *devices, int max_devices) {
    // Capture physical hardware that accepts audio FROM JACK (Speakers/Monitors)
    return scan_jack_ports(client, devices, max_devices, JackPortIsPhysical | JackPortIsInput);
}

/**
 * @brief Queries the highest supported compatible format (MJPEG or YUYV) and resolution for a device.
 * @param device_path The video device node (e.g., /dev/video0).
 * @param out_pixelformat Pointer to store the selected V4L2_PIX_FMT.
 * @param out_width Pointer to store the selected width.
 * @param out_height Pointer to store the selected height.
 * @return 1 on success finding a compatible format, 0 on failure.
 */
int get_best_video_format(const char* device_path, uint32_t *out_pixelformat, int *out_width, int *out_height) {
    int fd_temp = open(device_path, O_RDWR | O_NONBLOCK, 0);
    if (fd_temp == -1) return 0;

    // Discovery ladder prioritizing MJPEG at highest standard resolutions, then falling back to YUYV
    struct { uint32_t fmt; int w; int h; } ladder[] = {
        { V4L2_PIX_FMT_MJPEG, 1920, 1080 },
        { V4L2_PIX_FMT_MJPEG, 1280, 720 },
        { V4L2_PIX_FMT_YUYV,  1280, 720 },
        { V4L2_PIX_FMT_MJPEG, 854, 480 },
        { V4L2_PIX_FMT_YUYV,  854, 480 },
        { V4L2_PIX_FMT_MJPEG, 640, 480 },
        { V4L2_PIX_FMT_YUYV,  640, 480 }
    };

    for (int i = 0; i < 7; i++) {
        struct v4l2_format fmt = {0};
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.pixelformat = ladder[i].fmt;
        fmt.fmt.pix.width = ladder[i].w;
        fmt.fmt.pix.height = ladder[i].h;
        fmt.fmt.pix.field = V4L2_FIELD_ANY;

        // Try the format: the driver alters the struct to the closest match if it doesn't support the exact request
        if (ioctl(fd_temp, VIDIOC_TRY_FMT, &fmt) == 0) {
            if (fmt.fmt.pix.pixelformat == ladder[i].fmt) {
                *out_pixelformat = fmt.fmt.pix.pixelformat;
                *out_width = fmt.fmt.pix.width;
                *out_height = fmt.fmt.pix.height;
                close(fd_temp);
                return 1;
            }
        }
    }

    close(fd_temp);
    return 0;
}
