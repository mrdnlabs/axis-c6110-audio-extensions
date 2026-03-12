/**
 * Audio Forwarder implementation.
 *
 * Captures from PipeWire output sink monitor, converts F32P → G.711 u-law,
 * buffers in a ring buffer, and a separate thread streams to transmit.cgi.
 */

#include "audio_forwarder.h"
#include <syslog.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <curl/curl.h>

#include <spa/param/audio/format-utils.h>

#define APP_NAME "audio_control"
#define RING_BUFFER_SIZE (8000 * 2)  /* 2 seconds of G.711 u-law at 8kHz */

/* ========== G.711 u-law encoding ========== */

/* Encode a single 16-bit linear PCM sample to u-law */
static uint8_t linear_to_ulaw(int16_t sample) {
    const int BIAS = 0x84;
    const int CLIP = 32635;
    static const int exp_lut[256] = {
        0,0,1,1,2,2,2,2,3,3,3,3,3,3,3,3,
        4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
        5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,
        5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,
        6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,
        6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,
        6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,
        6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,
        7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
        7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
        7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
        7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
        7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
        7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
        7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
        7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7
    };

    int sign, exponent, mantissa;
    uint8_t ulaw_byte;

    sign = (sample >> 8) & 0x80;
    if (sign)
        sample = -sample;
    if (sample > CLIP)
        sample = CLIP;
    sample += BIAS;
    exponent = exp_lut[(sample >> 7) & 0xFF];
    mantissa = (sample >> (exponent + 3)) & 0x0F;
    ulaw_byte = ~(sign | (exponent << 4) | mantissa);

    return ulaw_byte;
}

/* ========== Ring Buffer ========== */

static int ring_buffer_init(struct ring_buffer *rb, size_t size) {
    rb->data = calloc(1, size);
    if (!rb->data)
        return -1;
    rb->size = size;
    rb->read_pos = 0;
    rb->write_pos = 0;
    pthread_mutex_init(&rb->mutex, NULL);
    pthread_cond_init(&rb->cond, NULL);
    return 0;
}

static void ring_buffer_cleanup(struct ring_buffer *rb) {
    free(rb->data);
    rb->data = NULL;
    pthread_mutex_destroy(&rb->mutex);
    pthread_cond_destroy(&rb->cond);
}

static size_t ring_buffer_available(struct ring_buffer *rb) {
    if (rb->write_pos >= rb->read_pos)
        return rb->write_pos - rb->read_pos;
    return rb->size - rb->read_pos + rb->write_pos;
}

static void ring_buffer_write(struct ring_buffer *rb, const uint8_t *data, size_t len) {
    pthread_mutex_lock(&rb->mutex);
    for (size_t i = 0; i < len; i++) {
        rb->data[rb->write_pos] = data[i];
        rb->write_pos = (rb->write_pos + 1) % rb->size;
        /* If we overrun, advance read position (drop oldest data) */
        if (rb->write_pos == rb->read_pos)
            rb->read_pos = (rb->read_pos + 1) % rb->size;
    }
    pthread_cond_signal(&rb->cond);
    pthread_mutex_unlock(&rb->mutex);
}

/* Discard all buffered data so the next read delivers live audio.
 * Called before each new HTTP connection to avoid playing stale backlog. */
static void ring_buffer_flush(struct ring_buffer *rb) {
    pthread_mutex_lock(&rb->mutex);
    rb->read_pos = rb->write_pos;
    pthread_mutex_unlock(&rb->mutex);
}

static size_t ring_buffer_read(struct ring_buffer *rb, uint8_t *data, size_t max_len) {
    pthread_mutex_lock(&rb->mutex);
    size_t avail = ring_buffer_available(rb);
    size_t to_read = (avail < max_len) ? avail : max_len;
    for (size_t i = 0; i < to_read; i++) {
        data[i] = rb->data[rb->read_pos];
        rb->read_pos = (rb->read_pos + 1) % rb->size;
    }
    pthread_mutex_unlock(&rb->mutex);
    return to_read;
}

/* ========== PipeWire Capture Callbacks ========== */

static void fwd_on_param_changed(void *data, uint32_t id,
                                 const struct spa_pod *param) {
    struct audio_forwarder *af = data;

    if (!param || id != SPA_PARAM_Format)
        return;

    struct spa_audio_info_raw info;
    if (spa_format_audio_raw_parse(param, &info) >= 0) {
        af->capture_rate = info.rate;
        syslog(LOG_INFO, "[%s] Audio forwarder: capture rate = %d Hz",
               APP_NAME, info.rate);
    }
}

static void fwd_on_process(void *data) {
    struct audio_forwarder *af = data;
    struct pw_buffer *b;
    struct spa_buffer *buf;

    b = pw_stream_dequeue_buffer(af->capture_stream);
    if (!b)
        return;

    buf = b->buffer;
    if (!buf->datas[0].data)
        goto done;

    uint32_t n_samples = buf->datas[0].chunk->size / sizeof(float);
    const float *samples = buf->datas[0].data;

    /* Downsample from capture_rate to 8kHz using integer decimation */
    int decimate = (af->capture_rate > 0) ? (af->capture_rate / 8000) : 1;
    if (decimate < 1) decimate = 1;

    uint32_t out_samples = 0;
    uint8_t *ulaw_buf = malloc(n_samples); /* upper bound */
    if (ulaw_buf) {
        for (uint32_t i = 0; i < n_samples; i++) {
            if (af->resample_phase == 0) {
                float s = samples[i];
                if (s > 1.0f) s = 1.0f;
                if (s < -1.0f) s = -1.0f;
                int16_t pcm = (int16_t)(s * 32767.0f);
                ulaw_buf[out_samples++] = linear_to_ulaw(pcm);
            }
            af->resample_phase = (af->resample_phase + 1) % decimate;
        }
        if (out_samples > 0)
            ring_buffer_write(&af->ring, ulaw_buf, out_samples);
        free(ulaw_buf);
    }

done:
    pw_stream_queue_buffer(af->capture_stream, b);
}

static void fwd_on_state_changed(void *data,
                                 enum pw_stream_state old,
                                 enum pw_stream_state state,
                                 const char *error) {
    (void)data;
    (void)old;
    if (state == PW_STREAM_STATE_ERROR) {
        syslog(LOG_ERR, "[%s] Audio forwarder stream error: %s", APP_NAME, error);
    }
}

static const struct pw_stream_events fwd_stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .param_changed = fwd_on_param_changed,
    .process = fwd_on_process,
    .state_changed = fwd_on_state_changed,
};

/* ========== HTTP POST Thread ========== */

/* Discard curl response body (prevent it from going to stdout/syslog) */
static size_t discard_write(char *ptr, size_t size, size_t nmemb, void *data) {
    (void)ptr; (void)data;
    return size * nmemb;
}

/* Curl read callback: feeds audio data from ring buffer into a streaming POST.
 * Returns 0 when af->running is cleared (signals end of stream to curl). */
static size_t stream_read(char *buf, size_t size, size_t nmemb, void *data) {
    struct audio_forwarder *af = data;
    size_t want = size * nmemb;

    while (af->running) {
        size_t got = ring_buffer_read(&af->ring, (uint8_t *)buf, want);
        if (got > 0)
            return got;
        /* No data yet — sleep briefly and retry */
        struct timespec ts = {0, 5000000}; /* 5ms */
        nanosleep(&ts, NULL);
    }
    return 0; /* signals curl to end the transfer */
}

static void *post_thread_func(void *data) {
    struct audio_forwarder *af = data;

    char url[256];
    snprintf(url, sizeof(url), "http://%s/axis-cgi/audio/transmit.cgi",
             af->config.remote_ip);

    char userpwd[128];
    snprintf(userpwd, sizeof(userpwd), "%s:%s",
             af->config.remote_user, af->config.remote_pass);

    syslog(LOG_INFO, "[%s] POST thread started: %s (streaming)", APP_NAME, url);

    /*
     * Stream audio as a single long-running chunked POST.
     * Digest auth is negotiated once per connection; on disconnect,
     * we reconnect and re-authenticate.
     */
    while (af->running) {
        /* Flush stale buffered audio so we always stream live data */
        ring_buffer_flush(&af->ring);

        CURL *handle = curl_easy_init();
        if (!handle) {
            syslog(LOG_ERR, "[%s] curl_easy_init failed", APP_NAME);
            struct timespec ts = {1, 0};
            nanosleep(&ts, NULL);
            continue;
        }

        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: audio/basic");
        headers = curl_slist_append(headers, "Expect:"); /* suppress 100-continue */

        curl_easy_setopt(handle, CURLOPT_URL, url);
        curl_easy_setopt(handle, CURLOPT_USERPWD, userpwd);
        curl_easy_setopt(handle, CURLOPT_HTTPAUTH, CURLAUTH_DIGEST);
        curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(handle, CURLOPT_POST, 1L);
        curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE, -1L); /* chunked */
        curl_easy_setopt(handle, CURLOPT_READFUNCTION, stream_read);
        curl_easy_setopt(handle, CURLOPT_READDATA, af);
        curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, discard_write);

        CURLcode res = curl_easy_perform(handle);

        curl_slist_free_all(headers);
        curl_easy_cleanup(handle);

        if (!af->running)
            break;

        if (res != CURLE_OK) {
            syslog(LOG_WARNING, "[%s] Stream to %s ended: %s — reconnecting in 1s",
                   APP_NAME, url, curl_easy_strerror(res));
        } else {
            syslog(LOG_INFO, "[%s] Stream to %s closed — reconnecting",
                   APP_NAME, url);
        }

        struct timespec ts = {1, 0};
        nanosleep(&ts, NULL);
    }

    syslog(LOG_INFO, "[%s] POST thread stopped", APP_NAME);
    return NULL;
}

/* ========== Public API ========== */

int audio_forwarder_init(struct audio_forwarder *af,
                         struct audio_forwarder_config *config,
                         struct pw_main_loop *pw_loop) {
    memset(af, 0, sizeof(*af));
    af->config = *config;
    af->pw_loop = pw_loop;

    if (af->config.sample_rate <= 0)
        af->config.sample_rate = 8000;
    if (af->config.chunk_ms <= 0)
        af->config.chunk_ms = 20;

    if (ring_buffer_init(&af->ring, RING_BUFFER_SIZE) < 0) {
        syslog(LOG_ERR, "[%s] Failed to init ring buffer", APP_NAME);
        return -1;
    }

    return 0;
}

int audio_forwarder_start(struct audio_forwarder *af) {
    uint8_t buf[1024];
    struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    const struct spa_pod *params[1];

    /* Create PipeWire capture stream (monitor mode on output sink) */
    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_STREAM_CAPTURE_SINK, "true",
        PW_KEY_TARGET_OBJECT, af->config.capture_node_name,
        PW_KEY_NODE_NAME, "audio-forwarder-capture",
        NULL);

    af->capture_stream = pw_stream_new_simple(
        pw_main_loop_get_loop(af->pw_loop),
        "audio-forwarder-capture",
        props,
        &fwd_stream_events,
        af);

    if (!af->capture_stream) {
        syslog(LOG_ERR, "[%s] Failed to create forwarder capture stream", APP_NAME);
        return -1;
    }

    params[0] = spa_format_audio_raw_build(
        &builder, SPA_PARAM_EnumFormat,
        &SPA_AUDIO_INFO_RAW_INIT(.channels = 1, .format = SPA_AUDIO_FORMAT_F32P));

    int res = pw_stream_connect(af->capture_stream,
                                PW_DIRECTION_INPUT, PW_ID_ANY,
                                PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS,
                                params, 1);
    if (res < 0) {
        syslog(LOG_ERR, "[%s] Failed to connect forwarder stream: %s",
               APP_NAME, strerror(-res));
        return -1;
    }

    /* Start HTTP POST thread */
    af->running = 1;
    if (pthread_create(&af->post_thread, NULL, post_thread_func, af) != 0) {
        syslog(LOG_ERR, "[%s] Failed to create POST thread", APP_NAME);
        af->running = 0;
        return -1;
    }

    syslog(LOG_INFO, "[%s] Audio forwarder started: %s -> %s",
           APP_NAME, af->config.capture_node_name, af->config.remote_ip);
    return 0;
}

void audio_forwarder_cleanup(struct audio_forwarder *af) {
    af->running = 0;

    /* Wake up the POST thread if it's waiting */
    pthread_mutex_lock(&af->ring.mutex);
    pthread_cond_signal(&af->ring.cond);
    pthread_mutex_unlock(&af->ring.mutex);

    if (af->post_thread)
        pthread_join(af->post_thread, NULL);

    if (af->capture_stream) {
        pw_stream_destroy(af->capture_stream);
        af->capture_stream = NULL;
    }

    ring_buffer_cleanup(&af->ring);
}
