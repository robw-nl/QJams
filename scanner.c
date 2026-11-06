#include <fcntl.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "config.h"
#include "scanner.h"

/**
 * @brief Scans the system for available V4L2 video capture devices.
 * Implements strict null-termination to prevent driver strings from overrunning boundaries.
 * @param devices Array to store the discovered video devices.
 * @param max_devices Maximum number of devices to scan.
 * @return The number of discovered video devices.
 */
int scan_video_devices(QJVideoDevice *devices, int max_devices) {
    int count = 0;

    for (int i = 0; i < 64 && count < max_devices; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/video%d", i);

        // Suppress error output, as we expect many of these opens to fail
        int temp_fd = open(path, O_RDWR | O_NONBLOCK, 0);
        if (temp_fd == -1)
            continue;

        struct v4l2_capability cap;
        if (ioctl(temp_fd, VIDIOC_QUERYCAP, &cap) != -1) {
            uint32_t caps =
                (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;

            if (caps & V4L2_CAP_VIDEO_CAPTURE) {
                strncpy(devices[count].device_path, path, sizeof(devices[count].device_path) - 1);
                devices[count].device_path[sizeof(devices[count].device_path) - 1] = '\0';

                strncpy(devices[count].device_name, (char *)cap.card,
                        sizeof(devices[count].device_name) - 1);
                devices[count].device_name[sizeof(devices[count].device_name) - 1] = '\0';

                log_and_notify(0, "Discovered Camera: %s at %s\n", devices[count].device_name,
                               devices[count].device_path);
                count++;
            }
        }
        close(temp_fd);
    }

    return count;
}

/**
 * @brief Queries the highest supported compatible format (MJPEG or YUYV) and resolution for a
 * device.
 * @param device_path The video device node (e.g., /dev/video0).
 * @param out_pixelformat Pointer to store the selected V4L2_PIX_FMT.
 * @param out_width Pointer to store the selected width.
 * @param out_height Pointer to store the selected height.
 * @return 1 on success finding a compatible format, 0 on failure.
 */
int get_best_video_format(const char *device_path, uint32_t *out_pixelformat, int *out_width,
                          int *out_height) {
    int fd_temp = open(device_path, O_RDWR | O_NONBLOCK, 0);
    if (fd_temp == -1)
        return 0;

    // Discovery ladder prioritizing MJPEG at highest standard resolutions, then falling back to
    // YUYV
    struct {
        uint32_t fmt;
        int w;
        int h;
    } ladder[] = {{V4L2_PIX_FMT_MJPEG, 1920, 1080}, {V4L2_PIX_FMT_MJPEG, 1280, 720},
                  {V4L2_PIX_FMT_YUYV, 1280, 720},   {V4L2_PIX_FMT_MJPEG, 854, 480},
                  {V4L2_PIX_FMT_YUYV, 854, 480},    {V4L2_PIX_FMT_MJPEG, 640, 480},
                  {V4L2_PIX_FMT_YUYV, 640, 480}};

    for (int i = 0; i < 7; i++) {
        struct v4l2_format fmt = {0};
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.pixelformat = ladder[i].fmt;
        fmt.fmt.pix.width = ladder[i].w;
        fmt.fmt.pix.height = ladder[i].h;
        fmt.fmt.pix.field = V4L2_FIELD_ANY;

        // Try the format: the driver alters the struct to the closest match if it doesn't support
        // the exact request
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

/**
 * @brief Executes lsusb to extract physical hardware identifiers present on the USB bus.
 * @note Safety [Moderate]: Utilizes popen for system calls. Buffer sizes and string tokenization
 * are strictly bounded to prevent memory leaks or buffer overruns.
 * @param out_keywords Array to store the discovered USB descriptions.
 * @param max_keywords Maximum number of devices to scan.
 * @return The number of discovered physical USB devices.
 */
int get_physical_usb_hardware(char out_keywords[][128], int max_keywords) {
    FILE *fp = popen("lsusb", "r");
    if (!fp)
        return 0;

    char line[256];
    int count = 0;

    while (fgets(line, sizeof(line), fp) && count < max_keywords) {
        char *id_ptr = strstr(line, "ID ");
        if (id_ptr) {
            char *desc = strchr(id_ptr, ' ');
            if (desc) {
                desc = strchr(desc + 1, ' ');
                if (desc) {
                    desc++;
                    char *nl = strchr(desc, '\n');
                    if (nl)
                        *nl = '\0';

                    strncpy(out_keywords[count], desc, 127);
                    out_keywords[count][127] = '\0';
                    count++;
                }
            }
        }
    }
    pclose(fp);
    return count;
}
