#ifndef AUDIO_ROUTING_H
#define AUDIO_ROUTING_H

#include "scanner.h" // Provides the QJAudioDevice struct
#include <jack/jack.h>

int scan_audio_devices(jack_client_t *client, QJAudioDevice *devices, int max_devices);
int scan_playback_devices(jack_client_t *client, QJAudioDevice *devices, int max_devices);

int patch_audio_ports(const char *target_audio_device);
int patch_playback_ports(const char *target_playback_device);

void qjams_drop_connections(void);
void jack_port_registration_cb(jack_port_id_t port_id, int register_port, void *arg);

#endif // AUDIO_ROUTING_H
