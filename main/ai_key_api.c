#include "ai_key_api.h"
#include "config_store.h"
#include "factory_reset.h"
#include "pairing.h"

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
        // Rotate — must hold the old token to do so.
        if (pairing_http_check(req) != ESP_OK) return ESP_FAIL;
    }
    char tok[64] = {0};
    if (pairing_pair(tok, sizeof(tok)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "pair failed");
        return ESP_FAIL;
    }
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
    ESP_LOGI(TAG, "AI key + reset + pair endpoints registered");
    return ESP_OK;
}
