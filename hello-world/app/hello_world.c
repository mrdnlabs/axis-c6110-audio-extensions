#include <syslog.h>
#include <signal.h>
#include <unistd.h>

#define APP_NAME "hello_world"

static volatile int running = 1;

static void signal_handler(int sig) {
    (void)sig;
    running = 0;
}

int main(void) {
    openlog(APP_NAME, LOG_PID, LOG_LOCAL4);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    syslog(LOG_INFO, "Hello World ACAP started successfully!");
    syslog(LOG_INFO, "This confirms the build-deploy-run cycle works.");

    while (running) {
        sleep(60);
    }

    syslog(LOG_INFO, "Hello World ACAP shutting down.");
    closelog();
    return 0;
}
