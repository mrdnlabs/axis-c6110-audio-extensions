/**
 * VAPIX credential retrieval via D-Bus.
 *
 * Follows the official Axis SDK pattern (acap-native-sdk-examples/vapix).
 * Credentials are transient — re-fetch on each ACAP startup.
 */

#include "vapix_credentials.h"
#include <gio/gio.h>
#include <syslog.h>
#include <stdlib.h>
#include <string.h>

#define APP_NAME "audio_control"

char *vapix_get_credentials(const char *service_name) {
    GError *error = NULL;
    GDBusConnection *connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (!connection) {
        syslog(LOG_ERR, "[%s] D-Bus connection failed: %s", APP_NAME,
               error ? error->message : "(unknown)");
        if (error) g_error_free(error);
        return NULL;
    }

    GVariant *result = g_dbus_connection_call_sync(
        connection,
        "com.axis.HTTPConf1",
        "/com/axis/HTTPConf1/VAPIXServiceAccounts1",
        "com.axis.HTTPConf1.VAPIXServiceAccounts1",
        "GetCredentials",
        g_variant_new("(s)", service_name),
        NULL,
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        NULL,
        &error);

    if (!result) {
        syslog(LOG_ERR, "[%s] D-Bus GetCredentials failed: %s", APP_NAME,
               error ? error->message : "(unknown)");
        if (error) g_error_free(error);
        g_object_unref(connection);
        return NULL;
    }

    /* Result is a tuple containing a single string "user:password" */
    const char *cred_string = NULL;
    g_variant_get(result, "(&s)", &cred_string);

    char *credentials = NULL;
    if (cred_string && *cred_string) {
        credentials = strdup(cred_string);
        syslog(LOG_INFO, "[%s] VAPIX service account credentials retrieved via D-Bus",
               APP_NAME);
    } else {
        syslog(LOG_ERR, "[%s] D-Bus returned empty credentials", APP_NAME);
    }

    g_variant_unref(result);
    g_object_unref(connection);
    return credentials;
}
