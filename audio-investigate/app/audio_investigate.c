/**
 * Audio Investigation ACAP
 *
 * Connects to PipeWire, listens for registry events, and logs every node's
 * properties to syslog. Useful for discovering the audio topology when
 * SSH-based pw-cli investigation is insufficient (e.g., ACAP apps see a
 * different PipeWire context).
 *
 * Exits after 10 seconds of enumeration.
 */

#include <pipewire/pipewire.h>
#include <spa/utils/dict.h>
#include <signal.h>
#include <syslog.h>

#define APP_NAME "audio_investigate"

struct impl {
    struct pw_main_loop *loop;
    struct pw_context *context;
    struct pw_core *core;
    struct pw_registry *registry;
    struct spa_hook registry_listener;
    int node_count;
};

static void on_signal(void *data, int signal_num) {
    struct impl *impl = data;
    pw_log_info("Got signal %d, quitting.", signal_num);
    pw_main_loop_quit(impl->loop);
}

static void registry_event_global(void *data,
                                  uint32_t id,
                                  uint32_t permissions,
                                  const char *type,
                                  uint32_t version,
                                  const struct spa_dict *props) {
    (void)permissions;
    (void)version;
    struct impl *impl = data;
    const struct spa_dict_item *item;

    syslog(LOG_INFO, "[%s] Object id=%u type=%s", APP_NAME, id, type);

    if (props) {
        spa_dict_for_each(item, props) {
            syslog(LOG_INFO, "[%s]   id=%u %s = %s", APP_NAME, id, item->key, item->value);
        }
    }

    /* Count nodes specifically */
    if (spa_streq(type, PW_TYPE_INTERFACE_Node)) {
        impl->node_count++;
        const char *name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
        const char *media_class = spa_dict_lookup(props, "media.class");
        const char *desc = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);

        syslog(LOG_INFO, "[%s] === NODE id=%u name=%s class=%s desc=%s ===",
               APP_NAME, id,
               name ? name : "(null)",
               media_class ? media_class : "(null)",
               desc ? desc : "(null)");
    }
}

static void registry_event_global_remove(void *data, uint32_t id) {
    (void)data;
    syslog(LOG_INFO, "[%s] Object removed id=%u", APP_NAME, id);
}

static const struct pw_registry_events registry_events = {
    PW_VERSION_REGISTRY_EVENTS,
    .global = registry_event_global,
    .global_remove = registry_event_global_remove,
};

/* Timer callback to quit after enumeration period */
static void on_timeout(void *data, uint64_t expirations) {
    (void)expirations;
    struct impl *impl = data;
    syslog(LOG_INFO, "[%s] Enumeration complete. Found %d nodes. Exiting.",
           APP_NAME, impl->node_count);
    pw_main_loop_quit(impl->loop);
}

int main(int argc, char *argv[]) {
    struct impl impl = {0};
    struct pw_loop *loop;
    struct spa_source *timer;
    struct timespec timeout = {10, 0}; /* 10 seconds */

    openlog(APP_NAME, LOG_PID, LOG_LOCAL4);
    syslog(LOG_INFO, "[%s] Starting PipeWire audio investigation...", APP_NAME);

    setenv("PIPEWIRE_DEBUG", APP_NAME ":5,3", 1);
    pw_init(&argc, &argv);

    impl.loop = pw_main_loop_new(NULL);
    if (!impl.loop) {
        syslog(LOG_ERR, "[%s] Failed to create main loop", APP_NAME);
        return 1;
    }
    loop = pw_main_loop_get_loop(impl.loop);

    pw_loop_add_signal(loop, SIGINT, on_signal, &impl);
    pw_loop_add_signal(loop, SIGTERM, on_signal, &impl);

    /* Add a timer to quit after 10 seconds */
    timer = pw_loop_add_timer(loop, on_timeout, &impl);
    pw_loop_update_timer(loop, timer, &timeout, NULL, false);

    impl.context = pw_context_new(loop, NULL, 0);
    if (!impl.context) {
        syslog(LOG_ERR, "[%s] Failed to create context", APP_NAME);
        return 1;
    }

    impl.core = pw_context_connect(impl.context, NULL, 0);
    if (!impl.core) {
        syslog(LOG_ERR, "[%s] Failed to connect to PipeWire", APP_NAME);
        return 1;
    }

    impl.registry = pw_core_get_registry(impl.core, PW_VERSION_REGISTRY, 0);
    pw_registry_add_listener(impl.registry, &impl.registry_listener,
                             &registry_events, &impl);

    syslog(LOG_INFO, "[%s] Connected to PipeWire. Enumerating objects for 10s...", APP_NAME);

    pw_main_loop_run(impl.loop);

    /* Cleanup */
    spa_hook_remove(&impl.registry_listener);
    pw_proxy_destroy((struct pw_proxy *)impl.registry);
    pw_core_disconnect(impl.core);
    pw_context_destroy(impl.context);
    pw_main_loop_destroy(impl.loop);
    pw_deinit();

    syslog(LOG_INFO, "[%s] Done.", APP_NAME);
    closelog();
    return 0;
}
