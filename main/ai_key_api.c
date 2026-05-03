#include "ai_key_api.h"
#include "config_store.h"
#include "factory_reset.h"
#include "mdns_service.h"
#include "pairing.h"
#include "plaiiin_mqtt.h"

#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "ai_key_api";

#define AI_KEY_MAX_LEN 256

static esp_err_t key_get_handler(httpd_req_t *req)
{
    if (pairing_http_check(req) != ESP_OK) return ESP_FAIL;
    // Phase 9 — never return the raw key over the network. The on-device
    // /compose page reads it from a server-injected <meta> tag instead, so
    // a snooper getting on the WiFi can no longer just GET /api/ai/key to
    // walk away with someone's Anthropic key.
    char key[AI_KEY_MAX_LEN] = {0};
    esp_err_t err = config_store_get_str(CONFIG_KEY_AI_API_KEY, key, sizeof(key));
    bool has = (err == ESP_OK && key[0] != 0);
    char resp[64];
    snprintf(resp, sizeof(resp), "{\"hasKey\":%s,\"len\":%u}",
             has ? "true" : "false", has ? (unsigned)strlen(key) : 0u);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static esp_err_t key_put_handler(httpd_req_t *req)
{
    if (pairing_http_check(req) != ESP_OK) return ESP_FAIL;
    int n = req->content_len;
    if (n <= 0 || n > AI_KEY_MAX_LEN - 1) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "key length 1..255");
        return ESP_FAIL;
    }
    char *buf = malloc(n + 1);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    int recv = 0;
    while (recv < n) {
        int r = httpd_req_recv(req, buf + recv, n - recv);
        if (r <= 0) { free(buf); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv"); return ESP_FAIL; }
        recv += r;
    }
    buf[n] = 0;
    // Trim trailing whitespace — pasted keys often pick up a trailing newline.
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r' || buf[n-1] == ' ' || buf[n-1] == '\t')) {
        buf[--n] = 0;
    }
    esp_err_t err = config_store_set_str(CONFIG_KEY_AI_API_KEY, buf);
    free(buf);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs write failed");
        return ESP_FAIL;
    }
    return key_get_handler(req);
}

static esp_err_t key_delete_handler(httpd_req_t *req)
{
    if (pairing_http_check(req) != ESP_OK) return ESP_FAIL;
    config_store_set_str(CONFIG_KEY_AI_API_KEY, "");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

// /api/pair — three verbs:
//   GET    →  status, no auth required
//   POST   →  generate token. If currently unpaired, anyone may call (this
//             is the bootstrap path). If already paired, requires auth — the
//             holder of the existing token can rotate it.
//   DELETE →  unpair, requires auth.
static esp_err_t pair_get_handler(httpd_req_t *req)
{
    char resp[96];
    snprintf(resp, sizeof(resp), "{\"mode\":\"%s\"}",
             pairing_is_paired() ? "paired" : "unpaired");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static esp_err_t pair_post_handler(httpd_req_t *req)
{
    if (pairing_is_paired()) {
        // Phase 12 — pair-once: a second device cannot claim a lamp that's
        // already owned. Holders of the existing token can still rotate it.
        // Surfaces as 409 Conflict so clients can show "already paired"
        // instead of the generic auth-error UI.
        size_t hdr_len = httpd_req_get_hdr_value_len(req, "Authorization");
        if (hdr_len == 0) {
            httpd_resp_set_status(req, "409 Conflict");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req,
                "{\"status\":\"error\",\"code\":\"already_paired\","
                "\"message\":\"This lamp is paired to another device. "
                "Unpair it from the original owner first.\"}");
            return ESP_FAIL;
        }
        // Auth header present — let pairing_http_check validate it (rotates
        // for legitimate holders, 401 for impostors who guessed wrong).
        if (pairing_http_check(req) != ESP_OK) return ESP_FAIL;
    }
    char tok[64] = {0};
    if (pairing_pair(tok, sizeof(tok)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "pair failed");
        return ESP_FAIL;
    }
    mdns_service_set_paired(true);
    char resp[160];
    snprintf(resp, sizeof(resp), "{\"mode\":\"paired\",\"token\":\"%s\"}", tok);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static esp_err_t pair_delete_handler(httpd_req_t *req)
{
    if (pairing_http_check(req) != ESP_OK) return ESP_FAIL;
    pairing_unpair();
    mdns_service_set_paired(false);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"mode\":\"unpaired\"}");
    return ESP_OK;
}

// /api/reset POST {"scope":"wifi"|"full"}
static esp_err_t reset_handler(httpd_req_t *req)
{
    if (pairing_http_check(req) != ESP_OK) return ESP_FAIL;
    char buf[96] = {0};
    int r = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (r <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body"); return ESP_FAIL; }
    bool full = strstr(buf, "\"full\"") != NULL;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req,
        full ? "{\"status\":\"ok\",\"scope\":\"full\",\"note\":\"rebooting\"}"
             : "{\"status\":\"ok\",\"scope\":\"wifi\",\"note\":\"rebooting\"}");
    // Send the response *before* rebooting so the client doesn't see a
    // hung connection. We pass reboot=true after a short courtesy delay.
    if (full) factory_reset_full(/*reboot=*/true);
    else      factory_reset_wifi(/*reboot=*/true);
    return ESP_OK;
}

// /api/mqtt — GET returns current settings; POST writes them and restarts
// the MQTT client in-place so the user doesn't have to reboot the lamp to
// flip a toggle. Both verbs require the bearer when paired.
static esp_err_t mqtt_get_handler(httpd_req_t *req)
{
    if (pairing_http_check(req) != ESP_OK) return ESP_FAIL;
    int32_t enabled = config_get_i32_or(CONFIG_KEY_MQTT_ACTIVE, 0);
    char host[128] = {0};
    config_get_str_or(CONFIG_KEY_MQTT_HOST, host, sizeof(host), "");
    int32_t port = config_get_i32_or(CONFIG_KEY_MQTT_PORT, 1883);

    char resp[256];
    snprintf(resp, sizeof(resp),
             "{\"enabled\":%s,\"host\":\"%s\",\"port\":%ld}",
             enabled ? "true" : "false", host, (long)port);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

// Tiny tolerant parser — pulls a quoted string for "<key>": "..." or a bool
// "<key>": true/false. Just enough for our flat 3-field payload.
static bool extract_string_field(const char *body, const char *key, char *out, size_t out_len)
{
    char needle[32];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *k = strstr(body, needle); if (!k) return false;
    const char *q1 = strchr(k + strlen(needle), '"'); if (!q1) return false;
    const char *q2 = strchr(q1 + 1, '"'); if (!q2) return false;
    size_t n = (size_t)(q2 - q1 - 1);
    if (n >= out_len) n = out_len - 1;
    memcpy(out, q1 + 1, n); out[n] = '\0';
    return true;
}

static esp_err_t mqtt_post_handler(httpd_req_t *req)
{
    if (pairing_http_check(req) != ESP_OK) return ESP_FAIL;
    int n = req->content_len;
    if (n <= 0 || n > 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body length 1..1024");
        return ESP_FAIL;
    }
    char *buf = malloc(n + 1);
    if (!buf) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM"); return ESP_FAIL; }
    int got = 0;
    while (got < n) {
        int r = httpd_req_recv(req, buf + got, n - got);
        if (r <= 0) { free(buf); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv"); return ESP_FAIL; }
        got += r;
    }
    buf[n] = '\0';

    // Optional fields — only update what the client sent. Lets the UI PATCH
    // a single value without re-sending the whole config.
    char host[128];
    if (extract_string_field(buf, "host", host, sizeof(host))) {
        config_store_set_str(CONFIG_KEY_MQTT_HOST, host);
    }
    if (strstr(buf, "\"enabled\"")) {
        bool en = strstr(buf, "\"enabled\":true") != NULL ||
                  strstr(buf, "\"enabled\": true") != NULL;
        config_store_set_i32(CONFIG_KEY_MQTT_ACTIVE, en ? 1 : 0);
    }
    {
        const char *p = strstr(buf, "\"port\"");
        if (p) {
            const char *colon = strchr(p, ':');
            if (colon) {
                long port = strtol(colon + 1, NULL, 10);
                if (port > 0 && port < 65536) {
                    config_store_set_i32(CONFIG_KEY_MQTT_PORT, (int32_t)port);
                }
            }
        }
    }
    free(buf);

    // Restart client to pick up the new settings without rebooting the lamp.
    // mqtt_client_restart logs but doesn't surface failures to the caller —
    // a misconfigured host (e.g. unreachable broker) shouldn't 500 the API.
    mqtt_client_restart();
    return mqtt_get_handler(req);
}

esp_err_t ai_key_api_register(httpd_handle_t server)
{
    httpd_uri_t get = {.uri="/api/ai/key", .method=HTTP_GET, .handler=key_get_handler};
    httpd_register_uri_handler(server, &get);
    httpd_uri_t put = {.uri="/api/ai/key", .method=HTTP_PUT, .handler=key_put_handler};
    httpd_register_uri_handler(server, &put);
    httpd_uri_t del = {.uri="/api/ai/key", .method=HTTP_DELETE, .handler=key_delete_handler};
    httpd_register_uri_handler(server, &del);
    httpd_uri_t reset = {.uri="/api/reset", .method=HTTP_POST, .handler=reset_handler};
    httpd_register_uri_handler(server, &reset);

    httpd_uri_t pg = {.uri="/api/pair", .method=HTTP_GET, .handler=pair_get_handler};
    httpd_register_uri_handler(server, &pg);
    httpd_uri_t pp = {.uri="/api/pair", .method=HTTP_POST, .handler=pair_post_handler};
    httpd_register_uri_handler(server, &pp);
    httpd_uri_t pd = {.uri="/api/pair", .method=HTTP_DELETE, .handler=pair_delete_handler};
    httpd_register_uri_handler(server, &pd);

    httpd_uri_t mg = {.uri="/api/mqtt", .method=HTTP_GET,  .handler=mqtt_get_handler};
    httpd_register_uri_handler(server, &mg);
    httpd_uri_t mp = {.uri="/api/mqtt", .method=HTTP_POST, .handler=mqtt_post_handler};
    httpd_register_uri_handler(server, &mp);

    ESP_LOGI(TAG, "AI key + reset + pair + mqtt endpoints registered");
    return ESP_OK;
}
