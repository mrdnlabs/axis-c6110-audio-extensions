#ifndef VAPIX_CLIENT_H
#define VAPIX_CLIENT_H

#include <curl/curl.h>
#include <jansson.h>

/**
 * VAPIX client for making API calls to the local device (127.0.0.1)
 * and remote Axis devices.
 *
 * Uses explicit credentials ("user:pass") for the local VAPIX API.
 */

struct vapix_client {
    CURL *handle;
    char *credentials;  /* "user:password" */
};

/* Initialize VAPIX client with explicit credentials (e.g. "root:pass") */
int vapix_client_init(struct vapix_client *client, const char *credentials);

/* Cleanup */
void vapix_client_cleanup(struct vapix_client *client);

/**
 * Make a JSON-RPC POST to a local VAPIX endpoint.
 * @param endpoint  e.g. "audiodevicecontrol.cgi"
 * @param method    e.g. "getDevicesSettings"
 * @param params    JSON object for params (can be NULL for empty params)
 * @return JSON response (caller must json_decref), or NULL on error
 */
json_t *vapix_call(struct vapix_client *client,
                   const char *endpoint,
                   const char *method,
                   json_t *params);

/**
 * Plain HTTP GET to a local URL; returns parsed JSON or NULL on error.
 * Caller must json_decref() the result.
 */
json_t *vapix_local_get(struct vapix_client *client, const char *url);

/**
 * Plain HTTP POST with JSON body to a local URL; returns parsed JSON or NULL.
 * Caller must json_decref() the result.
 */
json_t *vapix_local_post(struct vapix_client *client, const char *url, json_t *body);

/**
 * Make a JSON-RPC POST to a remote VAPIX endpoint.
 * Same as vapix_call() but targets a remote device with explicit credentials.
 */
json_t *vapix_call_remote(const char *remote_ip,
                           const char *remote_user,
                           const char *remote_pass,
                           const char *endpoint,
                           const char *method,
                           json_t *params);

/**
 * Make a raw POST to a remote device.
 * @param remote_ip  e.g. "192.168.1.219"
 * @param remote_user  e.g. "root"
 * @param remote_pass  e.g. "pass"
 * @param path  e.g. "audio/transmit.cgi"
 * @param content_type  e.g. "audio/basic"
 * @param data  raw data to POST
 * @param data_len  length of data
 * @return 0 on success, -1 on error
 */
int vapix_post_remote(const char *remote_ip,
                      const char *remote_user,
                      const char *remote_pass,
                      const char *path,
                      const char *content_type,
                      const void *data,
                      size_t data_len);

#endif /* VAPIX_CLIENT_H */
