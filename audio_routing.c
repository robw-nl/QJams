#include "audio_routing.h"
#include "audio_core.h" // Temporarily required for client and atomic flags
#include "config.h"     // Required for log_and_notify
#include <ctype.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Inherit global state variables from the core engine

/**
 * @brief Internal helper to scan the JACK server for physical audio ports matching specific flags.
 * Centralizes the port string tokenization and implements boundary protections.
 * @param client Pointer to the active JACK client.
 * @param devices Array to store the discovered audio devices.
 * @param max_devices Maximum number of devices to scan.
 * @param port_flags JACK port flags (e.g., JackPortIsInput or JackPortIsOutput).
 * @return The number of discovered audio devices.
 */
static int scan_jack_ports(jack_client_t *client, QJAudioDevice *devices, int max_devices,
                           unsigned long port_flags) {
    if (!client)
        return 0;
    int count = 0;

    const char **ports = jack_get_ports(client, NULL, NULL, port_flags);

    if (ports) {
        for (int i = 0; ports[i] != NULL && count < max_devices; i++) {
            if (strstr(ports[i], "QJams_Engine") != NULL)
                continue;

            char prefix[128] = {0};

            const char *colon = strchr(ports[i], ':');
            if (colon) {
                int len = colon - ports[i];
                if (len >= (int)sizeof(prefix))
                    len = sizeof(prefix) - 1;
                strncpy(prefix, ports[i], len);
                prefix[len] = '\0';
            } else {
                strncpy(prefix, ports[i], sizeof(prefix) - 1);
                prefix[sizeof(prefix) - 1] = '\0';
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
                devices[count].device_id[sizeof(devices[count].device_id) - 1] = '\0';

                strncpy(devices[count].display_name, prefix,
                        sizeof(devices[count].display_name) - 1);
                devices[count].display_name[sizeof(devices[count].display_name) - 1] = '\0';

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
int scan_audio_devices(jack_client_t *client, QJAudioDevice *devices, int max_devices) {
    return scan_jack_ports(client, devices, max_devices, JackPortIsOutput);
}

/**
 * @brief Scans the JACK server for physical audio playback devices.
 * @param client Pointer to the active JACK client.
 * @param devices Array to store the discovered playback devices.
 * @param max_devices Maximum number of devices to scan.
 * @return The number of discovered playback devices.
 */
int scan_playback_devices(jack_client_t *client, QJAudioDevice *devices, int max_devices) {
    return scan_jack_ports(client, devices, max_devices, JackPortIsPhysical | JackPortIsInput);
}

/**
 * @brief Polls the JACK server for a specific hardware device until it becomes visible or times
 * out.
 * @param client Pointer to the active JACK client.
 * @param target_device The substring identifier of the hardware.
 * @param flags The JACK port flags to filter by.
 * @return A null-terminated array of port strings, or NULL on timeout. Must be freed with
 * jack_free().
 */
static const char **wait_for_jack_ports(jack_client_t *client, const char *target_device,
                                        unsigned long flags) {
    const char **ports = NULL;
    int retries = 5;

    while (retries > 0) {
        ports = jack_get_ports(client, NULL, NULL, flags);
        if (ports) {
            for (int i = 0; ports[i] != NULL; i++) {
                if (strstr(ports[i], target_device) != NULL)
                    return ports; // Hardware is visible
            }
            jack_free(ports);
            ports = NULL;
        }
        usleep(100000); // 100ms polling delay
        retries--;
    }
    return NULL;
}

/**
 * @brief Immediately severs JACK graph links to clear PipeWire GC locks during an unplug.
 */
void qjams_drop_connections(void) {
    atomic_store_explicit(&audio_available, false, memory_order_release);
    if (client && input_port_1 && input_port_2) {
        jack_port_disconnect(client, input_port_1);
        jack_port_disconnect(client, input_port_2);
        jack_port_disconnect(client, output_port_1);
        jack_port_disconnect(client, output_port_2);
        log_and_notify(0, "[JACK DSP] Graph links severed to clear PipeWire GC locks.\n");
    }
}

/**
 * @brief Checks if a port's base name ends with or exactly matches a specific channel token,
 *        preventing prefix collisions (e.g., "AUX1" matching "AUX10" or "capture_1" matching "capture_10").
 * @param base_name Port name with client prefix and optional PipeWire "-N" instance suffix stripped.
 * @param token The target channel identifier (e.g., "AUX0", "AUX1", "capture_1", "FL").
 * @return true if the token matches on a non-digit boundary, false otherwise.
 */
static bool match_channel_token(const char *base_name, const char *token) {
    const char *p = base_name;
    size_t tok_len = strlen(token);
    while ((p = strstr(p, token)) != NULL) {
        char after = p[tok_len];
        if (after == '\0' || !isdigit((unsigned char)after)) {
            return true;
        }
        p += tok_len;
    }
    return false;
}

/**
 * @brief Connects physical USB hardware inputs to the internal QJams capture ports.
 * Integrates ghost-pruning to bypass dead file descriptors during rapid hot-plugging.
 * @param target_audio_device The name identifier of the hardware to connect.
 * @return 0 on success, 2 if the ports could not be located or routed.
 */
int patch_audio_ports(const char *target_audio_device) {
    if (!client)
        return 2;

    log_and_notify(0, "Attempting to patch capture device: %s\n", target_audio_device);

    atomic_store_explicit(&audio_available, false, memory_order_release);
    jack_port_disconnect(client, input_port_1);
    jack_port_disconnect(client, input_port_2);

    const char **ports = wait_for_jack_ports(client, target_audio_device, JackPortIsOutput);
    int connected = 0;

    if (ports) {
        const char *best_l = NULL;
        const char *best_r = NULL;
        int highest_l = -1, highest_r = -1;

        char prefix[256];
        snprintf(prefix, sizeof(prefix), "%s:", target_audio_device);
        size_t prefix_len = strlen(prefix);

        for (int i = 0; ports[i] != NULL; i++) {
            if (strncmp(ports[i], prefix, prefix_len) == 0) {
                const char *local_name = ports[i] + prefix_len;
                int suffix = 0;
                char base_name[256] = {0};
                strncpy(base_name, local_name, sizeof(base_name) - 1);
                base_name[sizeof(base_name) - 1] = '\0';
                char *dash = strrchr(base_name, '-');
                if (dash && isdigit((unsigned char)*(dash + 1))) {
                    suffix = atoi(dash + 1);
                    *dash = '\0';
                }

                bool is_l = (match_channel_token(base_name, "AUX0") ||
                match_channel_token(base_name, "FL") ||
                match_channel_token(base_name, "capture_1") ||
                match_channel_token(base_name, "left"));
                bool is_r = (match_channel_token(base_name, "AUX1") ||
                match_channel_token(base_name, "FR") ||
                match_channel_token(base_name, "capture_2") ||
                match_channel_token(base_name, "right"));

                if (!is_l && !is_r) {
                    if (!best_l)
                        is_l = true;
                    else if (!best_r)
                        is_r = true;
                }

                if (is_l && suffix > highest_l) {
                    highest_l = suffix;
                    best_l = ports[i];
                } else if (is_r && suffix > highest_r) {
                    highest_r = suffix;
                    best_r = ports[i];
                }
            }
        }

        if (best_l && jack_connect(client, best_l, jack_port_name(input_port_1)) == 0) {
            log_and_notify(0, "[JACK DSP] Connected Capture L: %s -> %s\n", best_l,
                           jack_port_name(input_port_1));
            connected++;
        }
        if (best_r && jack_connect(client, best_r, jack_port_name(input_port_2)) == 0) {
            log_and_notify(0, "[JACK DSP] Connected Capture R: %s -> %s\n", best_r,
                           jack_port_name(input_port_2));
            connected++;
        }
        jack_free(ports);
    }

    if (connected < 2) {
        log_and_notify(0, "Warning: Failure mapping complete stereo ports for audio device '%s'.\n",
                       target_audio_device);
        return 2;
    }

    atomic_store_explicit(&audio_available, true, memory_order_release);
    return 0;
}

/**
 * @brief Connects QJams output ports to the physical hardware playback ports.
 * @param target_playback_device The name identifier of the hardware to connect.
 * @return 0 on success, 2 if the ports could not be located or routed.
 */
int patch_playback_ports(const char *target_playback_device) {
    if (!client)
        return 2;

    log_and_notify(0, "Attempting to patch playback device: %s\n", target_playback_device);

    jack_port_disconnect(client, output_port_1);
    jack_port_disconnect(client, output_port_2);

    const char **ports =
    wait_for_jack_ports(client, target_playback_device, JackPortIsPhysical | JackPortIsInput);
    int connected = 0;

    if (ports) {
        const char *best_l = NULL;
        const char *best_r = NULL;
        int highest_l = -1, highest_r = -1;

        char prefix[256];
        snprintf(prefix, sizeof(prefix), "%s:", target_playback_device);
        size_t prefix_len = strlen(prefix);

        for (int i = 0; ports[i] != NULL; i++) {
            if (strncmp(ports[i], prefix, prefix_len) == 0) {
                const char *local_name = ports[i] + prefix_len;
                int suffix = 0;
                char base_name[256] = {0};
                strncpy(base_name, local_name, sizeof(base_name) - 1);
                base_name[sizeof(base_name) - 1] = '\0';
                char *dash = strrchr(base_name, '-');
                if (dash && isdigit((unsigned char)*(dash + 1))) {
                    suffix = atoi(dash + 1);
                    *dash = '\0';
                }

                bool is_l = (match_channel_token(base_name, "AUX0") ||
                match_channel_token(base_name, "FL") ||
                match_channel_token(base_name, "playback_1") ||
                match_channel_token(base_name, "left"));
                bool is_r = (match_channel_token(base_name, "AUX1") ||
                match_channel_token(base_name, "FR") ||
                match_channel_token(base_name, "playback_2") ||
                match_channel_token(base_name, "right"));

                if (!is_l && !is_r) {
                    if (!best_l)
                        is_l = true;
                    else if (!best_r)
                        is_r = true;
                }

                if (is_l && suffix > highest_l) {
                    highest_l = suffix;
                    best_l = ports[i];
                } else if (is_r && suffix > highest_r) {
                    highest_r = suffix;
                    best_r = ports[i];
                }
            }
        }

        if (best_l && jack_connect(client, jack_port_name(output_port_1), best_l) == 0) {
            log_and_notify(0, "[JACK DSP] Connected Playback L: %s -> %s\n",
                           jack_port_name(output_port_1), best_l);
            connected++;
        }
        if (best_r && jack_connect(client, jack_port_name(output_port_2), best_r) == 0) {
            log_and_notify(0, "[JACK DSP] Connected Playback R: %s -> %s\n",
                           jack_port_name(output_port_2), best_r);
            connected++;
        }
        jack_free(ports);
    }

    if (connected == 0) {
        log_and_notify(0, "Warning: Failure mapping ports for playback device '%s'.\n",
                       target_playback_device);
        return 2;
    }

    return 0;
}

/**
 * @brief Callback triggered when JACK detects a hardware port connection or disconnection.
 * Logs the specific hardware port name to the unified logger before alerting the UI.
 * @param port_id The ID of the port.
 * @param register_port Non-zero if registering, zero if unregistering.
 * @param arg Opaque user data.
 */
void jack_port_registration_cb(jack_port_id_t port_id, int register_port, void *arg) {
    (void)arg;
    if (!client)
        return;
    jack_port_t *port = jack_port_by_id(client, port_id);
    if (!port)
        return;
    int flags = jack_port_flags(port);
    if (!(flags & JackPortIsPhysical))
        return;
    if (event_pipe[1] != -1) {
        const uint8_t msg = register_port ? 2 : 3;
        ssize_t dummy = write(event_pipe[1], &msg, sizeof(msg));
        (void)dummy;
    }
}
