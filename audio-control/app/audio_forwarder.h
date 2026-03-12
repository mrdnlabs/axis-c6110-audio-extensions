#ifndef AUDIO_FORWARDER_H
#define AUDIO_FORWARDER_H

#include <pipewire/pipewire.h>
#include <stdint.h>
#include <pthread.h>

/**
 * Audio Forwarder — Goal 2
 *
 * Captures audio from the C6110's output sink via PipeWire monitor mode,
 * converts F32P to G.711 u-law, writes to a thread-safe ring buffer,
 * and a separate thread POSTs to the remote device's transmit.cgi.
 *
 * Pipeline:
 *   PipeWire sink monitor → F32P capture → G.711 u-law encode → ring buffer → HTTP POST thread
 */

struct audio_forwarder_config {
    /* PipeWire source: which output sink to capture from */
    const char *capture_node_name;  /* e.g. "AudioDevice0Output0" */

    /* Remote device */
    const char *remote_ip;          /* e.g. "192.168.1.219" */
    const char *remote_user;        /* e.g. "root" */
    const char *remote_pass;        /* e.g. "pass" */

    /* Audio parameters */
    int sample_rate;                /* Expected rate (8000 for G.711) */
    int chunk_ms;                   /* How many ms of audio per HTTP POST chunk (default 20) */
};

/* Ring buffer for thread-safe audio transfer */
struct ring_buffer {
    uint8_t *data;
    size_t size;
    size_t read_pos;
    size_t write_pos;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
};

struct audio_forwarder {
    struct audio_forwarder_config config;
    struct pw_main_loop *pw_loop;

    /* PipeWire capture */
    struct pw_stream *capture_stream;
    struct spa_hook capture_listener;
    int capture_rate;    /* Actual rate negotiated with PipeWire */
    int resample_phase;  /* Decimation counter for rate conversion */

    /* Ring buffer */
    struct ring_buffer ring;

    /* HTTP POST thread */
    pthread_t post_thread;
    volatile int running;
};

/**
 * Initialize the audio forwarder.
 * @param af  Audio forwarder instance
 * @param config  Configuration
 * @param pw_loop  Shared PipeWire main loop
 */
int audio_forwarder_init(struct audio_forwarder *af,
                         struct audio_forwarder_config *config,
                         struct pw_main_loop *pw_loop);

/* Start capturing and forwarding */
int audio_forwarder_start(struct audio_forwarder *af);

/* Stop and cleanup */
void audio_forwarder_cleanup(struct audio_forwarder *af);

#endif /* AUDIO_FORWARDER_H */
