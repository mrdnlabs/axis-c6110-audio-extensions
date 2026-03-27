#ifndef VAPIX_CREDENTIALS_H
#define VAPIX_CREDENTIALS_H

/**
 * Retrieve VAPIX service account credentials via D-Bus.
 *
 * Uses the com.axis.HTTPConf1.VAPIXServiceAccounts1.GetCredentials method
 * to obtain transient credentials for local VAPIX calls on loopback.
 *
 * @param service_name  Service account name (e.g. "audio_control")
 * @return "user:password" string (caller must free), or NULL on failure
 */
char *vapix_get_credentials(const char *service_name);

#endif /* VAPIX_CREDENTIALS_H */
