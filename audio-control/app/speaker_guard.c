/**
 * Speaker Guard implementation.
 *
 * Approach A: VAPIX polling — check/unmute speaker every N seconds.
 * Approach B: PipeWire loopback — capture from headphone sink, play to speaker.
 */

#include "speaker_guard.h"
#include <syslog.h>
#include <string.h>
#include <stdlib.h>

#include <spa/param/audio/format-utils.h>

#define APP_NAME "audio_control"

/* ========== Approach A: VAPIX Polling ========== */

static void poll_and_unmute(void *data, uint64_t expirations) {
    (void)expirations;
    struct speaker_guard *sg = data;

    /* Get current device settings */
    json_t *response = vapix_call(sg->vapix,
                                  "audiodevicecontrol.cgi",
                                  "getDevicesSettings", NULL);
    if (!response)
        return;

    /* Check if speaker output is muted */
    json_t *data_obj = json_object_get(response, "data");
    json_t *devices = json_object_get(data_obj, "devices");
    if (!json_is_array(devices)) {
        json_decref(response);
        return;
    }

    /* VAPIX returns device/output IDs as strings */
    char speaker_dev_str[16], speaker_out_str[16];
    snprintf(speaker_dev_str, sizeof(speaker_dev_str), "%d", sg->config.speaker_device_id);
    snprintf(speaker_out_str, sizeof(speaker_out_str), "%d", sg->config.speaker_output_id);

    size_t dev_idx;
    json_t *device;
    json_array_foreach(devices, dev_idx, device) {
        const char *dev_id_str = json_string_value(json_object_get(device, "id"));
        if (!dev_id_str || strcmp(dev_id_str, speaker_dev_str) != 0)
            continue;

        json_t *outputs = json_object_get(device, "outputs");
        if (!json_is_array(outputs))
            continue;

        size_t out_idx;
        json_t *output;
        json_array_foreach(outputs, out_idx, output) {
            const char *out_id_str = json_string_value(json_object_get(output, "id"));
            if (!out_id_str || strcmp(out_id_str, speaker_out_str) != 0)
                continue;

            /* Mute is nested: output.connectionTypes[0].signalingTypes[0].channels[0].mute */
            json_t *conn_types = json_object_get(output, "connectionTypes");
            json_t *conn_type  = json_is_array(conn_types) ? json_array_get(conn_types, 0) : NULL;
            json_t *sig_types  = conn_type ? json_object_get(conn_type, "signalingTypes") : NULL;
            json_t *sig_type   = json_is_array(sig_types) ? json_array_get(sig_types, 0) : NULL;
            json_t *channels   = sig_type ? json_object_get(sig_type, "channels") : NULL;
            json_t *channel    = json_is_array(channels) ? json_array_get(channels, 0) : NULL;
            json_t *mute       = channel ? json_object_get(channel, "mute") : NULL;

            if (json_is_true(mute)) {
                syslog(LOG_INFO, "[%s] Speaker muted (device=%s, output=%s), unmuting...",
                       APP_NAME, dev_id_str, out_id_str);

                /* Build unmute request — IDs must be strings */
                json_t *params = json_pack(
                    "{s:[{s:s,s:[{s:s,s:b}]}]}",
                    "devices",
                    "id", speaker_dev_str,
                    "outputs",
                    "id", speaker_out_str,
                    "mute", 0);

                json_t *unmute_resp = vapix_call(sg->vapix,
                                                 "audiodevicecontrol.cgi",
                                                 "setDevicesSettings",
                                                 params);
                if (unmute_resp) {
                    syslog(LOG_INFO, "[%s] Speaker unmuted successfully", APP_NAME);
                    json_decref(unmute_resp);
                } else {
                    syslog(LOG_WARNING, "[%s] Failed to unmute speaker", APP_NAME);
                }
                json_decref(params);
            }
        }
    }

    json_decref(response);
}

static int speaker_guard_start_approach_a(struct speaker_guard *sg) {
    struct pw_loop *loop = pw_main_loop_get_loop(sg->pw_loop);

    sg->poll_timer = pw_loop_add_timer(loop, poll_and_unmute, sg);
    if (!sg->poll_timer) {
        syslog(LOG_ERR, "[%s] Failed to create poll timer", APP_NAME);
        return -1;
    }

    struct timespec interval = {
        .tv_sec = sg->config.poll_interval_ms / 1000,
        .tv_nsec = (sg->config.poll_interval_ms % 1000) * 1000000L
    };
    pw_loop_update_timer(loop, sg->poll_timer, &interval, &interval, false);

    syslog(LOG_INFO, "[%s] Speaker guard (Approach A) started: polling every %dms",
           APP_NAME, sg->config.poll_interval_ms);
    return 0;
}

/* ========== Approach B: PipeWire Loopback ========== */

/* Capture stream process callback: copy buffer for playback */
static void capture_on_process(void *data) {
    struct speaker_guard *sg = data;
    struct pw_buffer *b;
    struct spa_buffer *buf;

    b = pw_stream_dequeue_buffer(sg->capture_stream);
    if (!b)
        return;

    buf = b->buffer;
    if (buf->datas[0].data) {
        uint32_t size = buf->datas[0].chunk->size;
        if (size > sg->loopback_buffer_size) {
            float *new_buf = realloc(sg->loopback_buffer, size);
            if (new_buf) {
                sg->loopback_buffer = new_buf;
                sg->loopback_buffer_size = size;
            } else {
                size = sg->loopback_buffer_size;
            }
        }
        memcpy(sg->loopback_buffer, buf->datas[0].data, size);
    }

    pw_stream_queue_buffer(sg->capture_stream, b);

    /* Trigger playback */
    struct pw_buffer *pb = pw_stream_dequeue_buffer(sg->playback_stream);
    if (pb) {
        struct spa_buffer *pbuf = pb->buffer;
        if (pbuf->datas[0].data && sg->loopback_buffer) {
            uint32_t size = buf->datas[0].chunk->size;
            if (size > pbuf->datas[0].maxsize)
                size = pbuf->datas[0].maxsize;
            memcpy(pbuf->datas[0].data, sg->loopback_buffer, size);
            pbuf->datas[0].chunk->offset = 0;
            pbuf->datas[0].chunk->stride = sizeof(float);
            pbuf->datas[0].chunk->size = size;
        }
        pw_stream_queue_buffer(sg->playback_stream, pb);
    }
}

static void on_stream_state_changed(void *data,
                                    enum pw_stream_state old,
                                    enum pw_stream_state state,
                                    const char *error) {
    (void)data;
    (void)old;
    if (state == PW_STREAM_STATE_ERROR) {
        syslog(LOG_ERR, "[%s] Speaker guard stream error: %s", APP_NAME, error);
    }
}

static const struct pw_stream_events capture_events = {
    PW_VERSION_STREAM_EVENTS,
    .process = capture_on_process,
    .state_changed = on_stream_state_changed,
};

/* Playback needs no process callback — filled by capture callback */
static void playback_on_process(void *data) {
    (void)data;
    /* Buffer filled by capture_on_process */
}

static const struct pw_stream_events playback_events = {
    PW_VERSION_STREAM_EVENTS,
    .process = playback_on_process,
    .state_changed = on_stream_state_changed,
};

static int speaker_guard_start_approach_b(struct speaker_guard *sg) {
    uint8_t buf[1024];
    struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    const struct spa_pod *params[1];

    /* Allocate initial loopback buffer */
    sg->loopback_buffer_size = 4096;
    sg->loopback_buffer = calloc(1, sg->loopback_buffer_size);
    if (!sg->loopback_buffer)
        return -1;

    /* Create capture stream: monitor the headphone output sink */
    struct pw_properties *cap_props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_STREAM_CAPTURE_SINK, "true",
        PW_KEY_TARGET_OBJECT, sg->config.headphone_node_name,
        PW_KEY_NODE_NAME, "speaker-guard-capture",
        NULL);

    sg->capture_stream = pw_stream_new_simple(
        pw_main_loop_get_loop(sg->pw_loop),
        "speaker-guard-capture",
        cap_props,
        &capture_events,
        sg);

    if (!sg->capture_stream) {
        syslog(LOG_ERR, "[%s] Failed to create capture stream", APP_NAME);
        return -1;
    }

    params[0] = spa_format_audio_raw_build(
        &builder, SPA_PARAM_EnumFormat,
        &SPA_AUDIO_INFO_RAW_INIT(.channels = 1, .format = SPA_AUDIO_FORMAT_F32P));

    pw_stream_connect(sg->capture_stream,
                      PW_DIRECTION_INPUT, PW_ID_ANY,
                      PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS,
                      params, 1);

    /* Create playback stream: output to speaker node */
    struct pw_properties *play_props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_TARGET_OBJECT, sg->config.speaker_node_name,
        PW_KEY_NODE_NAME, "speaker-guard-playback",
        NULL);

    builder = SPA_POD_BUILDER_INIT(buf, sizeof(buf));

    sg->playback_stream = pw_stream_new_simple(
        pw_main_loop_get_loop(sg->pw_loop),
        "speaker-guard-playback",
        play_props,
        &playback_events,
        sg);

    if (!sg->playback_stream) {
        syslog(LOG_ERR, "[%s] Failed to create playback stream", APP_NAME);
        return -1;
    }

    params[0] = spa_format_audio_raw_build(
        &builder, SPA_PARAM_EnumFormat,
        &SPA_AUDIO_INFO_RAW_INIT(.channels = 1, .format = SPA_AUDIO_FORMAT_F32P));

    pw_stream_connect(sg->playback_stream,
                      PW_DIRECTION_OUTPUT, PW_ID_ANY,
                      PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS,
                      params, 1);

    syslog(LOG_INFO, "[%s] Speaker guard (Approach B) started: %s -> %s",
           APP_NAME, sg->config.headphone_node_name, sg->config.speaker_node_name);
    return 0;
}

/* ========== Public API ========== */

int speaker_guard_init(struct speaker_guard *sg,
                       struct speaker_guard_config *config,
                       struct vapix_client *vapix,
                       struct pw_main_loop *pw_loop) {
    memset(sg, 0, sizeof(*sg));
    sg->config = *config;
    sg->vapix = vapix;
    sg->pw_loop = pw_loop;

    if (sg->config.poll_interval_ms <= 0)
        sg->config.poll_interval_ms = 2000;

    return 0;
}

int speaker_guard_start(struct speaker_guard *sg) {
    switch (sg->config.approach) {
    case 'A':
        return speaker_guard_start_approach_a(sg);
    case 'B':
        return speaker_guard_start_approach_b(sg);
    default:
        syslog(LOG_ERR, "[%s] Invalid speaker guard approach: %c",
               APP_NAME, sg->config.approach);
        return -1;
    }
}

void speaker_guard_cleanup(struct speaker_guard *sg) {
    if (sg->capture_stream) {
        pw_stream_destroy(sg->capture_stream);
        sg->capture_stream = NULL;
    }
    if (sg->playback_stream) {
        pw_stream_destroy(sg->playback_stream);
        sg->playback_stream = NULL;
    }
    free(sg->loopback_buffer);
    sg->loopback_buffer = NULL;
}
