/**
 * Audio Control ACAP — Main Entry Point
 *
 * Long-running daemon that:
 *   1. Keeps the C6110's built-in speaker active when headphones are plugged in
 *   2. Forwards/duplicates audio to a remote Axis network speaker
 *
 * Architecture:
 *   - PipeWire event loop (main thread)
 *   - VAPIX client for local device API calls
 *   - Speaker guard module (Approach A or B)
 *   - Audio forwarder module (PipeWire capture → G.711 → HTTP POST)
 */

#include <pipewire/pipewire.h>
#include <curl/curl.h>
#include <axsdk/axparameter.h>
#include <glib.h>
#include <syslog.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "vapix_client.h"
#include "speaker_guard.h"
#include "audio_forwarder.h"
#include "mode_controller.h"

#define APP_NAME "audio_control"

/* ========== Fixed hardware configuration ========== */

#define SPEAKER_GUARD_APPROACH 'B'
#define SPEAKER_DEVICE_ID  0
#define SPEAKER_OUTPUT_ID  1
#define HEADPHONE_NODE_NAME "AudioDevice0Output0"
#define SPEAKER_NODE_NAME   "AudioDevice0Output1"
#define CAPTURE_NODE_NAME   "AudioDevice0Outputs"

/* ========== Runtime configuration (from ACAP parameters) ========== */

struct runtime_config {
    int  enable_speaker_guard;
    int  enable_audio_forward;
    int  enable_dual_audio;
    char remote_ip[64];
    char remote_user[64];
    char remote_pass[64];
    char local_user[64];
    char local_pass[64];
    char headphone_node[64];
    char speaker_node[64];
    char capture_node[64];
};

static void load_params(struct runtime_config *cfg) {
    /* Defaults */
    cfg->enable_speaker_guard = 1;
    cfg->enable_audio_forward = 1;
    cfg->enable_dual_audio    = 0;
    snprintf(cfg->remote_ip,   sizeof(cfg->remote_ip),   "192.168.1.219");
    snprintf(cfg->remote_user, sizeof(cfg->remote_user), "admin");
    snprintf(cfg->remote_pass, sizeof(cfg->remote_pass), "admin");
    snprintf(cfg->local_user,  sizeof(cfg->local_user),  "admin");
    snprintf(cfg->local_pass,  sizeof(cfg->local_pass),  "admin");
    snprintf(cfg->headphone_node, sizeof(cfg->headphone_node), HEADPHONE_NODE_NAME);
    snprintf(cfg->speaker_node,   sizeof(cfg->speaker_node),   SPEAKER_NODE_NAME);
    snprintf(cfg->capture_node,   sizeof(cfg->capture_node),   CAPTURE_NODE_NAME);

    AXParameter *ax = ax_parameter_new(APP_NAME, NULL);
    if (!ax) {
        syslog(LOG_WARNING, "[%s] ax_parameter_new failed, using defaults", APP_NAME);
        return;
    }

    gchar *val = NULL;

    if (ax_parameter_get(ax, "EnableSpeakerGuard", &val, NULL) && val)
        { cfg->enable_speaker_guard = (strcmp(val, "yes") == 0); g_free(val); val = NULL; }

    if (ax_parameter_get(ax, "EnableAudioForward", &val, NULL) && val)
        { cfg->enable_audio_forward = (strcmp(val, "yes") == 0); g_free(val); val = NULL; }

    if (ax_parameter_get(ax, "EnableDualAudio", &val, NULL) && val)
        { cfg->enable_dual_audio = (strcmp(val, "yes") == 0); g_free(val); val = NULL; }

    if (ax_parameter_get(ax, "RemoteIP", &val, NULL) && val && *val)
        { snprintf(cfg->remote_ip, sizeof(cfg->remote_ip), "%s", val); g_free(val); val = NULL; }

    if (ax_parameter_get(ax, "RemoteUser", &val, NULL) && val && *val)
        { snprintf(cfg->remote_user, sizeof(cfg->remote_user), "%s", val); g_free(val); val = NULL; }

    if (ax_parameter_get(ax, "RemotePass", &val, NULL) && val)
        { snprintf(cfg->remote_pass, sizeof(cfg->remote_pass), "%s", val); g_free(val); val = NULL; }

    if (ax_parameter_get(ax, "LocalUser", &val, NULL) && val && *val)
        { snprintf(cfg->local_user, sizeof(cfg->local_user), "%s", val); g_free(val); val = NULL; }

    if (ax_parameter_get(ax, "LocalPass", &val, NULL) && val)
        { snprintf(cfg->local_pass, sizeof(cfg->local_pass), "%s", val); g_free(val); val = NULL; }

    if (ax_parameter_get(ax, "HeadphoneNodeName", &val, NULL) && val && *val)
        { snprintf(cfg->headphone_node, sizeof(cfg->headphone_node), "%s", val); g_free(val); val = NULL; }

    if (ax_parameter_get(ax, "SpeakerNodeName", &val, NULL) && val && *val)
        { snprintf(cfg->speaker_node, sizeof(cfg->speaker_node), "%s", val); g_free(val); val = NULL; }

    if (ax_parameter_get(ax, "CaptureNodeName", &val, NULL) && val && *val)
        { snprintf(cfg->capture_node, sizeof(cfg->capture_node), "%s", val); g_free(val); val = NULL; }

    ax_parameter_free(ax);

    syslog(LOG_INFO, "[%s] Config: speaker_guard=%s audio_forward=%s local=%s remote=%s@%s nodes=%s/%s/%s",
           APP_NAME,
           cfg->enable_speaker_guard ? "yes" : "no",
           cfg->enable_audio_forward ? "yes" : "no",
           cfg->local_user,
           cfg->remote_user, cfg->remote_ip,
           cfg->headphone_node, cfg->speaker_node, cfg->capture_node);
}

/* ========== Global State ========== */

struct app {
    struct pw_main_loop *pw_loop;
    struct vapix_client vapix;
    struct speaker_guard sg;
    struct audio_forwarder af;
    struct mode_controller mc;
};

/* ========== Action pre-configuration ========== */

#define ACTIONS_URL "http://127.0.0.1/config/rest/paging-console-actions/v1/actions"

static json_t *build_action(const char *label, const char *description,
                             const char *mode, const char *user, const char *pass) {
    char param_url[256];
    snprintf(param_url, sizeof(param_url),
             "http://127.0.0.1/axis-cgi/param.cgi"
             "?action=update&root.audio_control.ActiveMode=%s", mode);

    json_t *recipient = json_object();
    json_object_set_new(recipient, "authenticationMethod", json_string("digest"));
    json_object_set_new(recipient, "hasPassword", json_true());
    json_object_set_new(recipient, "password", json_string(pass));
    json_object_set_new(recipient, "port", json_integer(80));
    json_object_set_new(recipient, "url", json_string(param_url));
    json_object_set_new(recipient, "user", json_string(user));

    json_t *http_req = json_object();
    json_object_set_new(http_req, "body", json_string(""));
    json_object_set_new(http_req, "headers", json_array());
    json_object_set_new(http_req, "method", json_string("GET"));
    json_object_set_new(http_req, "proxy", json_null());
    json_object_set_new(http_req, "recipient", recipient);

    json_t *params = json_object();
    json_object_set_new(params, "httpRequest", http_req);

    json_t *data = json_object();
    json_object_set_new(data, "description", json_string(description));
    json_object_set_new(data, "label", json_string(label));
    json_object_set_new(data, "params", params);
    json_object_set_new(data, "type", json_string("httpRequest"));

    json_t *body = json_object();
    json_object_set_new(body, "data", data);
    return body;
}

static void setup_mode_actions(struct vapix_client *vapix) {
    /* Parse "user:pass" from the vapix client credentials */
    char user[64] = "root", pass[64] = "";
    if (vapix->credentials) {
        char *colon = strchr(vapix->credentials, ':');
        if (colon) {
            size_t ulen = (size_t)(colon - vapix->credentials);
            if (ulen < sizeof(user)) {
                memcpy(user, vapix->credentials, ulen);
                user[ulen] = '\0';
            }
            snprintf(pass, sizeof(pass), "%s", colon + 1);
        }
    }

    /* Fetch existing actions to avoid duplicates */
    int has_classroom = 0, has_instructor = 0;
    json_t *list = vapix_local_get(vapix, ACTIONS_URL);
    if (list) {
        json_t *data = json_object_get(list, "data");
        size_t i;
        json_t *action;
        json_array_foreach(data, i, action) {
            const char *lbl = json_string_value(json_object_get(action, "label"));
            if (!lbl) continue;
            if (strcmp(lbl, "Classroom Mode") == 0)  has_classroom = 1;
            if (strcmp(lbl, "Instructor Mode") == 0) has_instructor = 1;
        }
        json_decref(list);
    } else {
        syslog(LOG_WARNING, "[%s] Could not list paging-console actions — skipping setup", APP_NAME);
        return;
    }

    static const struct { const char *label; const char *desc; const char *mode; } defs[2] = {
        { "Classroom Mode",  "Unmute remote speaker and headphone, mute C6110 speaker", "classroom" },
        { "Instructor Mode", "Mute remote speaker, unmute C6110 speaker",               "instructor" },
    };

    for (int i = 0; i < 2; i++) {
        int exists = (i == 0) ? has_classroom : has_instructor;
        if (exists) {
            syslog(LOG_INFO, "[%s] Action already exists: %s", APP_NAME, defs[i].label);
            continue;
        }

        json_t *body = build_action(defs[i].label, defs[i].desc, defs[i].mode, user, pass);
        json_t *resp = vapix_local_post(vapix, ACTIONS_URL, body);
        json_decref(body);

        if (resp) {
            syslog(LOG_INFO, "[%s] Created action: %s", APP_NAME, defs[i].label);
            json_decref(resp);
        } else {
            syslog(LOG_WARNING, "[%s] Failed to create action: %s", APP_NAME, defs[i].label);
        }
    }
}

static void on_signal(void *data, int signal_num) {
    struct app *app = data;
    syslog(LOG_INFO, "[%s] Got signal %d, shutting down...", APP_NAME, signal_num);
    pw_main_loop_quit(app->pw_loop);
}

int main(int argc, char *argv[]) {
    struct app app = {0};
    struct runtime_config cfg;
    struct pw_loop *loop;
    int ret = EXIT_FAILURE;

    openlog(APP_NAME, LOG_PID, LOG_LOCAL4);
    syslog(LOG_INFO, "[%s] Starting...", APP_NAME);

    load_params(&cfg);

    /* Initialize libraries */
    curl_global_init(CURL_GLOBAL_DEFAULT);
    pw_init(&argc, &argv);

    setenv("PIPEWIRE_DEBUG", APP_NAME ":4,2", 1);

    /* Create PipeWire main loop */
    app.pw_loop = pw_main_loop_new(NULL);
    if (!app.pw_loop) {
        syslog(LOG_ERR, "[%s] Failed to create PipeWire main loop", APP_NAME);
        goto cleanup;
    }
    loop = pw_main_loop_get_loop(app.pw_loop);

    /* Signal handlers */
    pw_loop_add_signal(loop, SIGINT, on_signal, &app);
    pw_loop_add_signal(loop, SIGTERM, on_signal, &app);

    /* Initialize VAPIX client */
    char local_creds[128];
    snprintf(local_creds, sizeof(local_creds), "%s:%s", cfg.local_user, cfg.local_pass);
    if (vapix_client_init(&app.vapix, local_creds) < 0) {
        syslog(LOG_WARNING, "[%s] VAPIX client init failed (speaker guard Approach A unavailable)",
               APP_NAME);
    }

    setup_mode_actions(&app.vapix);

    if (cfg.enable_speaker_guard) {
        struct speaker_guard_config sg_config = {
            .approach = SPEAKER_GUARD_APPROACH,
            .poll_interval_ms = 500,
            .speaker_device_id = SPEAKER_DEVICE_ID,
            .speaker_output_id = SPEAKER_OUTPUT_ID,
            .headphone_node_name = cfg.headphone_node,
            .speaker_node_name = cfg.speaker_node,
        };

        if (speaker_guard_init(&app.sg, &sg_config, &app.vapix, app.pw_loop) < 0) {
            syslog(LOG_ERR, "[%s] Speaker guard init failed", APP_NAME);
        } else if (speaker_guard_start(&app.sg) < 0) {
            syslog(LOG_ERR, "[%s] Speaker guard start failed", APP_NAME);
        } else {
            syslog(LOG_INFO, "[%s] Speaker guard active (Approach %c)",
                   APP_NAME, SPEAKER_GUARD_APPROACH);
        }
    }

    if (cfg.enable_audio_forward) {
        struct audio_forwarder_config af_config = {
            .capture_node_name = cfg.capture_node,
            .remote_ip   = cfg.remote_ip,
            .remote_user = cfg.remote_user,
            .remote_pass = cfg.remote_pass,
            .sample_rate = 8000,
            .chunk_ms    = 20,
        };

        if (audio_forwarder_init(&app.af, &af_config, app.pw_loop) < 0) {
            syslog(LOG_ERR, "[%s] Audio forwarder init failed", APP_NAME);
        } else if (audio_forwarder_start(&app.af) < 0) {
            syslog(LOG_ERR, "[%s] Audio forwarder start failed", APP_NAME);
        } else {
            syslog(LOG_INFO, "[%s] Audio forwarder active: %s -> %s",
                   APP_NAME, CAPTURE_NODE_NAME, cfg.remote_ip);
        }
    }

    {
        struct mode_controller_config mc_config = {
            .remote_ip   = cfg.remote_ip,
            .remote_user = cfg.remote_user,
            .remote_pass = cfg.remote_pass,
            .dual_audio  = cfg.enable_dual_audio,
        };
        if (mode_controller_init(&app.mc, &mc_config, &app.vapix, app.pw_loop) < 0)
            syslog(LOG_ERR, "[%s] Mode controller init failed", APP_NAME);
    }

    syslog(LOG_INFO, "[%s] Running. Speaker guard=%s, Audio forward=%s",
           APP_NAME,
           cfg.enable_speaker_guard ? "ON" : "OFF",
           cfg.enable_audio_forward ? "ON" : "OFF");

    /* Run the main loop */
    pw_main_loop_run(app.pw_loop);

    ret = EXIT_SUCCESS;
    syslog(LOG_INFO, "[%s] Main loop exited, cleaning up...", APP_NAME);

cleanup:
    mode_controller_cleanup(&app.mc);
    audio_forwarder_cleanup(&app.af);
    speaker_guard_cleanup(&app.sg);
    vapix_client_cleanup(&app.vapix);

    if (app.pw_loop)
        pw_main_loop_destroy(app.pw_loop);

    pw_deinit();
    curl_global_cleanup();

    syslog(LOG_INFO, "[%s] Shutdown complete.", APP_NAME);
    closelog();
    return ret;
}
