/**
 * Audio Monitor ACAP
 *
 * Captures from the C1110-E's audio output sink (monitor mode) and logs
 * RMS level every 2 seconds to syslog. Used to verify that audio arriving
 * via transmit.cgi is actually flowing through PipeWire and playing.
 *
 * Log lines:
 *   [audio_monitor] level: rms=0.0123 peak=0.0456 samples=8192  <- audio present
 *   [audio_monitor] level: rms=0.0000 peak=0.0000 samples=8192  <- silence
 */

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <syslog.h>
#include <math.h>
#include <string.h>
#include <signal.h>

#define APP_NAME "audio_monitor"
#define REPORT_INTERVAL_SEC 2

struct monitor {
    struct pw_main_loop *loop;
    struct pw_stream *stream;
    struct spa_hook stream_listener;

    /* Accumulator for RMS over the reporting interval */
    double sum_sq;
    float  peak;
    uint64_t n_samples;

    /* Timer for periodic reporting */
    struct spa_source *report_timer;
    int capture_rate;
};

/* ── helpers ── */

static float rms(double sum_sq, uint64_t n) {
    if (n == 0) return 0.0f;
    return (float)sqrt(sum_sq / (double)n);
}

/* ── report timer ── */

static void on_report_timer(void *data, uint64_t expirations) {
    (void)expirations;
    struct monitor *m = data;

    float r = rms(m->sum_sq, m->n_samples);
    syslog(LOG_INFO, "[%s] level: rms=%.4f peak=%.4f samples=%" PRIu64 " rate=%d",
           APP_NAME, r, m->peak, m->n_samples, m->capture_rate);

    /* Reset accumulators */
    m->sum_sq   = 0.0;
    m->peak     = 0.0f;
    m->n_samples = 0;
}

/* ── PipeWire stream callbacks ── */

static void on_param_changed(void *data, uint32_t id, const struct spa_pod *param) {
    struct monitor *m = data;
    if (!param || id != SPA_PARAM_Format)
        return;
    struct spa_audio_info_raw info;
    if (spa_format_audio_raw_parse(param, &info) >= 0) {
        m->capture_rate = (int)info.rate;
        syslog(LOG_INFO, "[%s] stream format: rate=%d channels=%d",
               APP_NAME, info.rate, info.channels);
    }
}

static void on_process(void *data) {
    struct monitor *m = data;
    struct pw_buffer *b = pw_stream_dequeue_buffer(m->stream);
    if (!b) return;

    struct spa_buffer *buf = b->buffer;
    if (buf->datas[0].data) {
        uint32_t n = buf->datas[0].chunk->size / sizeof(float);
        const float *samples = buf->datas[0].data;
        for (uint32_t i = 0; i < n; i++) {
            float s = samples[i];
            m->sum_sq += (double)s * (double)s;
            float a = s < 0.0f ? -s : s;
            if (a > m->peak) m->peak = a;
        }
        m->n_samples += n;
    }

    pw_stream_queue_buffer(m->stream, b);
}

static void on_state_changed(void *data,
                              enum pw_stream_state old,
                              enum pw_stream_state state,
                              const char *error) {
    (void)old;
    struct monitor *m = data;
    syslog(LOG_INFO, "[%s] stream state: %s%s",
           APP_NAME, pw_stream_state_as_string(state),
           error ? error : "");
    if (state == PW_STREAM_STATE_ERROR)
        pw_main_loop_quit(m->loop);
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .param_changed = on_param_changed,
    .process       = on_process,
    .state_changed = on_state_changed,
};

/* ── signal handler ── */

static void on_signal(void *data, int sig) {
    (void)sig;
    struct monitor *m = data;
    pw_main_loop_quit(m->loop);
}

/* ── main ── */

int main(int argc, char *argv[]) {
    struct monitor m = {0};

    openlog(APP_NAME, LOG_PID, LOG_LOCAL4);
    syslog(LOG_INFO, "[%s] Starting...", APP_NAME);

    pw_init(&argc, &argv);

    m.loop = pw_main_loop_new(NULL);
    if (!m.loop) {
        syslog(LOG_ERR, "[%s] Failed to create main loop", APP_NAME);
        return 1;
    }

    struct pw_loop *loop = pw_main_loop_get_loop(m.loop);
    pw_loop_add_signal(loop, SIGINT,  on_signal, &m);
    pw_loop_add_signal(loop, SIGTERM, on_signal, &m);

    /* Periodic report timer */
    m.report_timer = pw_loop_add_timer(loop, on_report_timer, &m);
    struct timespec interval = { REPORT_INTERVAL_SEC, 0 };
    pw_loop_update_timer(loop, m.report_timer, &interval, &interval, false);

    /* Capture stream: monitor the output sink so we see what's playing */
    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE,          "Audio",
        PW_KEY_MEDIA_CATEGORY,      "Capture",
        PW_KEY_STREAM_CAPTURE_SINK, "true",
        PW_KEY_NODE_NAME,           "audio-monitor-capture",
        NULL);

    m.stream = pw_stream_new_simple(
        loop, "audio-monitor-capture", props,
        &stream_events, &m);

    if (!m.stream) {
        syslog(LOG_ERR, "[%s] Failed to create stream", APP_NAME);
        return 1;
    }

    uint8_t buf[1024];
    struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    const struct spa_pod *params[1];
    params[0] = spa_format_audio_raw_build(
        &builder, SPA_PARAM_EnumFormat,
        &SPA_AUDIO_INFO_RAW_INIT(.channels = 1, .format = SPA_AUDIO_FORMAT_F32P));

    pw_stream_connect(m.stream,
                      PW_DIRECTION_INPUT, PW_ID_ANY,
                      PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS,
                      params, 1);

    syslog(LOG_INFO, "[%s] Running — reporting every %ds", APP_NAME, REPORT_INTERVAL_SEC);
    pw_main_loop_run(m.loop);

    pw_stream_destroy(m.stream);
    pw_main_loop_destroy(m.loop);
    pw_deinit();

    syslog(LOG_INFO, "[%s] Stopped.", APP_NAME);
    closelog();
    return 0;
}
