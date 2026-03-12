#ifndef SPEAKER_GUARD_H
#define SPEAKER_GUARD_H

#include "vapix_client.h"
#include <pipewire/pipewire.h>

/**
 * Speaker Guard — Goal 1
 *
 * Keeps the built-in speaker active even when headphones are plugged in.
 *
 * Two approaches implemented:
 *
 * Approach A (VAPIX polling):
 *   Periodically calls getDevicesSettings to check speaker mute state,
 *   and calls setDevicesSettings to unmute if muted by headphone insertion.
 *
 * Approach B (PipeWire loopback):
 *   Captures audio from the headphone output sink (monitor mode) and plays
 *   it to the speaker output node, bypassing firmware mute entirely.
 */

/* Configuration for speaker guard */
struct speaker_guard_config {
    /* Approach selection: 'A' for VAPIX polling, 'B' for PipeWire loopback */
    char approach;

    /* Approach A settings */
    int poll_interval_ms;     /* How often to check/unmute (default 2000) */
    int speaker_device_id;    /* Audio device ID (usually 0) */
    int speaker_output_id;    /* Speaker output ID (from investigation) */

    /* Approach B settings */
    const char *headphone_node_name;  /* e.g. "AudioDevice0Output1" */
    const char *speaker_node_name;    /* e.g. "AudioDevice0Output0" */
};

struct speaker_guard {
    struct speaker_guard_config config;
    struct vapix_client *vapix;       /* Shared VAPIX client (Approach A) */
    struct pw_main_loop *pw_loop;     /* Shared PipeWire loop (Approach B) */

    /* Approach A state */
    struct spa_source *poll_timer;

    /* Approach B state */
    struct pw_stream *capture_stream;
    struct pw_stream *playback_stream;
    struct spa_hook capture_listener;
    struct spa_hook playback_listener;
    float *loopback_buffer;
    uint32_t loopback_buffer_size;
};

/**
 * Initialize speaker guard with the given approach.
 * For Approach A: requires vapix client.
 * For Approach B: requires PipeWire loop.
 */
int speaker_guard_init(struct speaker_guard *sg,
                       struct speaker_guard_config *config,
                       struct vapix_client *vapix,
                       struct pw_main_loop *pw_loop);

/* Start the speaker guard */
int speaker_guard_start(struct speaker_guard *sg);

/* Stop and cleanup */
void speaker_guard_cleanup(struct speaker_guard *sg);

#endif /* SPEAKER_GUARD_H */
