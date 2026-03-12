/**
 * VAPIX client implementation.
 *
 * Uses configured credentials and libcurl to call local and remote
 * VAPIX JSON-RPC endpoints over HTTP Digest authentication.
 */

#include "vapix_client.h"
#include <syslog.h>
#include <stdlib.h>
#include <string.h>

#define APP_NAME "audio_control"
#define LOCAL_BASE_URL "http://127.0.0.1/axis-cgi/"

/* Curl write callback: append to a dynamically allocated buffer */
struct response_buf {
    char *data;
    size_t size;
};

static size_t write_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    struct response_buf *buf = userdata;
    size_t bytes = size * nmemb;
    char *new_data = realloc(buf->data, buf->size + bytes + 1);
    if (!new_data) {
        syslog(LOG_ERR, "[%s] realloc failed in write_callback", APP_NAME);
        return 0;
    }
    buf->data = new_data;
    memcpy(buf->data + buf->size, ptr, bytes);
    buf->size += bytes;
    buf->data[buf->size] = '\0';
    return bytes;
}


int vapix_client_init(struct vapix_client *client, const char *credentials) {
    memset(client, 0, sizeof(*client));

    client->credentials = strdup(credentials);
    if (!client->credentials) {
        syslog(LOG_ERR, "[%s] strdup failed for credentials", APP_NAME);
        return -1;
    }

    client->handle = curl_easy_init();
    if (!client->handle) {
        syslog(LOG_ERR, "[%s] curl_easy_init failed", APP_NAME);
        free(client->credentials);
        client->credentials = NULL;
        return -1;
    }

    syslog(LOG_INFO, "[%s] VAPIX client initialized", APP_NAME);
    return 0;
}

void vapix_client_cleanup(struct vapix_client *client) {
    if (client->handle) {
        curl_easy_cleanup(client->handle);
        client->handle = NULL;
    }
    free(client->credentials);
    client->credentials = NULL;
}

json_t *vapix_call(struct vapix_client *client,
                   const char *endpoint,
                   const char *method,
                   json_t *params) {
    char url[256];
    snprintf(url, sizeof(url), "%s%s", LOCAL_BASE_URL, endpoint);

    /* Build JSON-RPC request */
    json_t *request = json_object();
    json_object_set_new(request, "apiVersion", json_string("1.0"));
    json_object_set_new(request, "method", json_string(method));
    if (params) {
        json_object_set(request, "params", params);
    }

    char *request_str = json_dumps(request, JSON_COMPACT);
    json_decref(request);

    if (!request_str) {
        syslog(LOG_ERR, "[%s] Failed to serialize JSON request", APP_NAME);
        return NULL;
    }

    struct response_buf response = {0};

    curl_easy_reset(client->handle);
    curl_easy_setopt(client->handle, CURLOPT_URL, url);
    curl_easy_setopt(client->handle, CURLOPT_USERPWD, client->credentials);
    curl_easy_setopt(client->handle, CURLOPT_HTTPAUTH, CURLAUTH_DIGEST);
    curl_easy_setopt(client->handle, CURLOPT_POSTFIELDS, request_str);
    curl_easy_setopt(client->handle, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(client->handle, CURLOPT_WRITEDATA, &response);

    CURLcode res = curl_easy_perform(client->handle);
    free(request_str);

    if (res != CURLE_OK) {
        syslog(LOG_ERR, "[%s] VAPIX call %s/%s failed: %s",
               APP_NAME, endpoint, method, curl_easy_strerror(res));
        free(response.data);
        return NULL;
    }

    long http_code;
    curl_easy_getinfo(client->handle, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code != 200) {
        syslog(LOG_ERR, "[%s] VAPIX %s/%s returned HTTP %ld: %s",
               APP_NAME, endpoint, method, http_code,
               response.data ? response.data : "(empty)");
        free(response.data);
        return NULL;
    }

    json_error_t parse_error;
    json_t *json_response = json_loads(response.data, 0, &parse_error);
    free(response.data);

    if (!json_response) {
        syslog(LOG_ERR, "[%s] JSON parse error: %s", APP_NAME, parse_error.text);
        return NULL;
    }

    /* Check for API-level error */
    json_t *api_error = json_object_get(json_response, "error");
    if (api_error) {
        const char *msg = json_string_value(json_object_get(api_error, "message"));
        syslog(LOG_ERR, "[%s] VAPIX API error in %s/%s: %s",
               APP_NAME, endpoint, method, msg ? msg : "(unknown)");
        json_decref(json_response);
        return NULL;
    }

    return json_response;
}

json_t *vapix_local_get(struct vapix_client *client, const char *url) {
    struct response_buf response = {0};

    curl_easy_reset(client->handle);
    curl_easy_setopt(client->handle, CURLOPT_URL, url);
    curl_easy_setopt(client->handle, CURLOPT_USERPWD, client->credentials);
    curl_easy_setopt(client->handle, CURLOPT_HTTPAUTH, CURLAUTH_DIGEST);
    curl_easy_setopt(client->handle, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(client->handle, CURLOPT_WRITEDATA, &response);

    CURLcode res = curl_easy_perform(client->handle);
    if (res != CURLE_OK) {
        syslog(LOG_ERR, "[%s] GET %s failed: %s", APP_NAME, url, curl_easy_strerror(res));
        free(response.data);
        return NULL;
    }

    json_error_t err;
    json_t *json = json_loads(response.data ? response.data : "", 0, &err);
    free(response.data);
    if (!json)
        syslog(LOG_ERR, "[%s] GET %s: JSON parse error: %s", APP_NAME, url, err.text);
    return json;
}

json_t *vapix_local_post(struct vapix_client *client, const char *url, json_t *body) {
    char *body_str = json_dumps(body, JSON_COMPACT);
    if (!body_str)
        return NULL;

    struct response_buf response = {0};
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_reset(client->handle);
    curl_easy_setopt(client->handle, CURLOPT_URL, url);
    curl_easy_setopt(client->handle, CURLOPT_USERPWD, client->credentials);
    curl_easy_setopt(client->handle, CURLOPT_HTTPAUTH, CURLAUTH_DIGEST);
    curl_easy_setopt(client->handle, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(client->handle, CURLOPT_POSTFIELDS, body_str);
    curl_easy_setopt(client->handle, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(client->handle, CURLOPT_WRITEDATA, &response);

    CURLcode res = curl_easy_perform(client->handle);
    free(body_str);
    curl_slist_free_all(headers);

    if (res != CURLE_OK) {
        syslog(LOG_ERR, "[%s] POST %s failed: %s", APP_NAME, url, curl_easy_strerror(res));
        free(response.data);
        return NULL;
    }

    json_error_t err;
    json_t *json = json_loads(response.data ? response.data : "", 0, &err);
    free(response.data);
    if (!json)
        syslog(LOG_ERR, "[%s] POST %s: JSON parse error: %s", APP_NAME, url, err.text);
    return json;
}

json_t *vapix_call_remote(const char *remote_ip,
                           const char *remote_user,
                           const char *remote_pass,
                           const char *endpoint,
                           const char *method,
                           json_t *params) {
    char url[256];
    snprintf(url, sizeof(url), "http://%s/axis-cgi/%s", remote_ip, endpoint);

    char userpwd[128];
    snprintf(userpwd, sizeof(userpwd), "%s:%s", remote_user, remote_pass);

    json_t *request = json_object();
    json_object_set_new(request, "apiVersion", json_string("1.0"));
    json_object_set_new(request, "method", json_string(method));
    if (params)
        json_object_set(request, "params", params);

    char *request_str = json_dumps(request, JSON_COMPACT);
    json_decref(request);
    if (!request_str)
        return NULL;

    struct response_buf response = {0};

    CURL *handle = curl_easy_init();
    if (!handle) {
        free(request_str);
        return NULL;
    }

    curl_easy_setopt(handle, CURLOPT_URL, url);
    curl_easy_setopt(handle, CURLOPT_USERPWD, userpwd);
    curl_easy_setopt(handle, CURLOPT_HTTPAUTH, CURLAUTH_DIGEST);
    curl_easy_setopt(handle, CURLOPT_POSTFIELDS, request_str);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &response);

    CURLcode res = curl_easy_perform(handle);
    free(request_str);
    curl_easy_cleanup(handle);

    if (res != CURLE_OK) {
        syslog(LOG_ERR, "[%s] Remote VAPIX call %s/%s to %s failed: %s",
               APP_NAME, endpoint, method, remote_ip, curl_easy_strerror(res));
        free(response.data);
        return NULL;
    }

    json_error_t parse_error;
    json_t *json_response = json_loads(response.data ? response.data : "", 0, &parse_error);
    free(response.data);

    if (!json_response) {
        syslog(LOG_ERR, "[%s] JSON parse error from %s: %s", APP_NAME, remote_ip, parse_error.text);
        return NULL;
    }

    json_t *api_error = json_object_get(json_response, "error");
    if (api_error) {
        const char *msg = json_string_value(json_object_get(api_error, "message"));
        syslog(LOG_ERR, "[%s] Remote VAPIX error %s/%s on %s: %s",
               APP_NAME, endpoint, method, remote_ip, msg ? msg : "(unknown)");
        json_decref(json_response);
        return NULL;
    }

    return json_response;
}

int vapix_post_remote(const char *remote_ip,
                      const char *remote_user,
                      const char *remote_pass,
                      const char *path,
                      const char *content_type,
                      const void *data,
                      size_t data_len) {
    CURL *handle = curl_easy_init();
    if (!handle)
        return -1;

    char url[256];
    snprintf(url, sizeof(url), "http://%s/axis-cgi/%s", remote_ip, path);

    char userpwd[128];
    snprintf(userpwd, sizeof(userpwd), "%s:%s", remote_user, remote_pass);

    struct curl_slist *headers = NULL;
    char ct_header[128];
    snprintf(ct_header, sizeof(ct_header), "Content-Type: %s", content_type);
    headers = curl_slist_append(headers, ct_header);

    curl_easy_setopt(handle, CURLOPT_URL, url);
    curl_easy_setopt(handle, CURLOPT_USERPWD, userpwd);
    curl_easy_setopt(handle, CURLOPT_HTTPAUTH, CURLAUTH_DIGEST);
    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(handle, CURLOPT_POSTFIELDS, data);
    curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE, (long)data_len);

    CURLcode res = curl_easy_perform(handle);
    int ret = (res == CURLE_OK) ? 0 : -1;

    if (res != CURLE_OK) {
        syslog(LOG_ERR, "[%s] Remote POST to %s failed: %s",
               APP_NAME, url, curl_easy_strerror(res));
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(handle);
    return ret;
}
