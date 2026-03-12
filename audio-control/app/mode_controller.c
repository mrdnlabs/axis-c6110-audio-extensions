/**
 * Mode Controller implementation.
 *
 * Polls ActiveMode ACAP parameter every second; on change, spawns a thread
 * to apply VAPIX mute/unmute calls without blocking the PipeWire event loop.
 */

#include "mode_controller.h"
#include <axsdk/axparameter.h>
#include <glib.h>
#include <jansson.h>
#include <syslog.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>

#define APP_NAME "audio_control"

/* ========== VAPIX mute helpers ========== */

/*
 * Output mute specification: describes the exact nested path required by
 * setDevicesSettings to reach the channel-level mute control.
 *
 * Discovered by querying getDevicesSettings on each device:
 *   C6110 OUT0 (headphone) : connType="headphone", sigType="unbalanced", channels 0+1
 *   C6110 OUT1 (speaker)   : connType="internal",  sigType="unbalanced", channel  0
 *   C1110-E OUT0 (speaker) : connType="internal",  sigType="unbalanced", channel  0
 */
struct output_spec {
    const char *dev_id;
    const char *out_id;
    const char *conn_type;
    const char *sig_type;
    const char *channel_ids[4]; /* NULL-terminated */
};

static const struct output_spec C6110_HEADPHONE = {
    "0", "0", "headphone", "unbalanced", {"0", "1", NULL}
};
static const struct output_spec C6110_SPEAKER = {
    "0", "1", "internal", "unbalanced", {"0", NULL}
};
static const struct output_spec C1110_SPEAKER = {
    "0", "0", "internal", "unbalanced", {"0", NULL}
};

/* Build setDevicesSettings params using the full channel-level nested path */
static json_t *build_channel_mute_params(const struct output_spec *spec, int muted) {
    json_t *channels = json_array();
    for (int i = 0; spec->channel_ids[i]; i++) {
        json_t *ch = json_object();
        json_object_set_new(ch, "id",   json_string(spec->channel_ids[i]));
        json_object_set_new(ch, "mute", muted ? json_true() : json_false());
        json_array_append_new(channels, ch);
    }

    json_t *sig_type = json_object();
    json_object_set_new(sig_type, "id",       json_string(spec->sig_type));
    json_object_set_new(sig_type, "channels", channels);

    json_t *conn_type = json_object();
    json_object_set_new(conn_type, "id",              json_string(spec->conn_type));
    json_object_set_new(conn_type, "signalingTypes",  json_pack("[o]", sig_type));

    json_t *output = json_object();
    json_object_set_new(output, "id",              json_string(spec->out_id));
    json_object_set_new(output, "connectionTypes", json_pack("[o]", conn_type));

    json_t *device = json_object();
    json_object_set_new(device, "id",      json_string(spec->dev_id));
    json_object_set_new(device, "outputs", json_pack("[o]", output));

    json_t *params = json_object();
    json_object_set_new(params, "devices", json_pack("[o]", device));
    return params;
}

static int set_local_output_muted(struct vapix_client *vapix,
                                   const struct output_spec *spec, int muted) {
    json_t *params = build_channel_mute_params(spec, muted);
    json_t *resp = vapix_call(vapix, "audiodevicecontrol.cgi",
                               "setDevicesSettings", params);
    json_decref(params);
    if (!resp)
        return -1;
    json_decref(resp);
    return 0;
}

static int set_remote_output_muted(const struct mode_controller_config *cfg,
                                    const struct output_spec *spec, int muted) {
    json_t *params = build_channel_mute_params(spec, muted);
    json_t *resp = vapix_call_remote(cfg->remote_ip, cfg->remote_user, cfg->remote_pass,
                                      "audiodevicecontrol.cgi",
                                      "setDevicesSettings", params);
    json_decref(params);
    if (!resp)
        return -1;
    json_decref(resp);
    return 0;
}

/* ========== Apply thread ========== */

struct apply_args {
    struct vapix_client *vapix;
    struct mode_controller_config config; /* copied by value */
    char mode[32];
};

static void *apply_mode_thread(void *data) {
    struct apply_args *args = data;
    const char *mode = args->mode;
    int is_classroom = (strcmp(mode, "classroom") == 0);

    syslog(LOG_INFO, "[%s] Applying mode: %s", APP_NAME, mode);

    /*
     * classroom:   OUT1 unmuted, OUT2 muted, C1110-E unmuted
     * instructor:  OUT1 muted,   OUT2 unmuted, C1110-E muted
     */
    int out1_muted = is_classroom ? 0 : 1;
    int out2_muted = is_classroom ? 1 : 0;
    int c1110_muted = is_classroom ? 0 : 1;

    if (set_local_output_muted(args->vapix, &C6110_HEADPHONE, out1_muted) < 0)
        syslog(LOG_WARNING, "[%s] Failed to set C6110 OUT1 mute=%d", APP_NAME, out1_muted);
    else
        syslog(LOG_INFO, "[%s] C6110 OUT1 (headphone) muted=%d", APP_NAME, out1_muted);

    if (set_local_output_muted(args->vapix, &C6110_SPEAKER, out2_muted) < 0)
        syslog(LOG_WARNING, "[%s] Failed to set C6110 OUT2 mute=%d", APP_NAME, out2_muted);
    else
        syslog(LOG_INFO, "[%s] C6110 OUT2 (speaker) muted=%d", APP_NAME, out2_muted);

    if (set_remote_output_muted(&args->config, &C1110_SPEAKER, c1110_muted) < 0)
        syslog(LOG_WARNING, "[%s] Failed to set C1110-E mute=%d", APP_NAME, c1110_muted);
    else
        syslog(LOG_INFO, "[%s] C1110-E output muted=%d", APP_NAME, c1110_muted);

    free(args);
    return NULL;
}

static void trigger_apply(struct mode_controller *mc, const char *mode) {
    struct apply_args *args = malloc(sizeof(*args));
    if (!args)
        return;

    args->vapix = mc->vapix;
    args->config = mc->config;
    snprintf(args->mode, sizeof(args->mode), "%s", mode);

    pthread_t t;
    if (pthread_create(&t, NULL, apply_mode_thread, args) != 0) {
        syslog(LOG_ERR, "[%s] Failed to spawn mode apply thread", APP_NAME);
        free(args);
        return;
    }
    pthread_detach(t);
}

/* ========== PipeWire poll timer ========== */

static void on_poll_timer(void *data, uint64_t expirations) {
    (void)expirations;
    struct mode_controller *mc = data;

    AXParameter *ax = ax_parameter_new(APP_NAME, NULL);
    if (!ax)
        return;

    gchar *val = NULL;
    if (!ax_parameter_get(ax, "ActiveMode", &val, NULL) || !val) {
        ax_parameter_free(ax);
        return;
    }
    ax_parameter_free(ax);

    /* Only act if mode has changed */
    if (strcmp(val, mc->current_mode) != 0) {
        if (strcmp(val, "instructor") == 0 || strcmp(val, "classroom") == 0) {
            snprintf(mc->current_mode, sizeof(mc->current_mode), "%s", val);
            trigger_apply(mc, val);
        } else {
            syslog(LOG_WARNING, "[%s] Unknown ActiveMode value: '%s' (expected instructor or classroom)",
                   APP_NAME, val);
        }
    }

    g_free(val);
}

/* ========== Public API ========== */

int mode_controller_init(struct mode_controller *mc,
                          struct mode_controller_config *config,
                          struct vapix_client *vapix,
                          struct pw_main_loop *pw_loop) {
    memset(mc, 0, sizeof(*mc));
    mc->config = *config;
    mc->vapix   = vapix;
    mc->pw_loop = pw_loop;

    /* Read initial mode and apply it immediately */
    AXParameter *ax = ax_parameter_new(APP_NAME, NULL);
    if (ax) {
        gchar *val = NULL;
        if (ax_parameter_get(ax, "ActiveMode", &val, NULL) && val) {
            snprintf(mc->current_mode, sizeof(mc->current_mode), "%s", val);
            g_free(val);
        }
        ax_parameter_free(ax);
    }
    if (mc->current_mode[0] == '\0')
        snprintf(mc->current_mode, sizeof(mc->current_mode), "instructor");

    trigger_apply(mc, mc->current_mode);

    /* Set up 1-second poll timer */
    struct pw_loop *loop = pw_main_loop_get_loop(pw_loop);
    mc->poll_timer = pw_loop_add_timer(loop, on_poll_timer, mc);
    if (!mc->poll_timer) {
        syslog(LOG_ERR, "[%s] Failed to create mode poll timer", APP_NAME);
        return -1;
    }

    struct timespec interval = {1, 0};
    pw_loop_update_timer(loop, mc->poll_timer, &interval, &interval, false);

    syslog(LOG_INFO, "[%s] Mode controller active, initial mode: %s", APP_NAME, mc->current_mode);
    return 0;
}

void mode_controller_cleanup(struct mode_controller *mc) {
    if (mc->poll_timer && mc->pw_loop) {
        struct pw_loop *loop = pw_main_loop_get_loop(mc->pw_loop);
        pw_loop_destroy_source(loop, mc->poll_timer);
        mc->poll_timer = NULL;
    }
}
