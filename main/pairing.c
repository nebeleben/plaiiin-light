#include "pairing.h"
#include "config_store.h"
#include "wifi.h"

#include "esp_log.h"
#include "esp_random.h"

#include <string.h>

static const char *TAG = "pairing";

// 32 random bytes → 43 chars URL-safe base64. We add 2 chars of headroom +
// NUL so callers always have room for a clean copy.
#define TOKEN_BYTES 32
#define TOKEN_B64_LEN 64

static bool s_paired_cached = false;

static const char b64_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static void base64_url_encode(const uint8_t *in, size_t in_len, char *out)
{
    size_t i, o = 0;
    for (i = 0; i + 3 <= in_len; i += 3) {
        uint32_t triple = ((uint32_t)in[i] << 16) | ((uint32_t)in[i+1] << 8) | in[i+2];
        out[o++] = b64_alphabet[(triple >> 18) & 0x3F];
        out[o++] = b64_alphabet[(triple >> 12) & 0x3F];
        out[o++] = b64_alphabet[(triple >>  6) & 0x3F];
        out[o++] = b64_alphabet[ triple        & 0x3F];
    }
    if (i < in_len) {
        uint32_t remaining = in_len - i;
        uint32_t triple = (uint32_t)in[i] << 16;
        if (remaining > 1) triple |= (uint32_t)in[i+1] << 8;
        out[o++] = b64_alphabet[(triple >> 18) & 0x3F];
        out[o++] = b64_alphabet[(triple >> 12) & 0x3F];
        if (remaining > 1) out[o++] = b64_alphabet[(triple >>  6) & 0x3F];
    }
    out[o] = 0;
}

esp_err_t pairing_init(void)
{
    char mode[16] = {0};
    config_get_str_or(CONFIG_KEY_PAIR_MODE, mode, sizeof(mode), "unpaired");
    s_paired_cached = (strcmp(mode, "paired") == 0);
    ESP_LOGI(TAG, "pair_mode=%s", s_paired_cached ? "paired" : "unpaired");
    return ESP_OK;
}

bool pairing_is_paired(void) { return s_paired_cached; }

// Constant-time compare. Bails the loop variable's count, not the conditional,
// so a network-side attacker can't time-distinguish an early mismatch from a
// late one and binary-search the token.
static bool ct_strcmp(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    if (la != lb) return false;
    uint8_t diff = 0;
    for (size_t i = 0; i < la; i++) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

bool pairing_check(const char *token)
{
    if (!s_paired_cached) return true;
    if (!token || !*token) return false;
    char stored[TOKEN_B64_LEN];
    if (config_store_get_str(CONFIG_KEY_PAIR_TOKEN, stored, sizeof(stored)) != ESP_OK) {
        // Paired mode but no token? Treat as unpaired to avoid bricking the
        // device — and warn loudly so the discrepancy gets noticed.
        ESP_LOGW(TAG, "paired without token; permitting + flipping back to unpaired");
        config_store_set_str(CONFIG_KEY_PAIR_MODE, "unpaired");
        s_paired_cached = false;
        return true;
    }
    return ct_strcmp(stored, token);
}

esp_err_t pairing_pair(char *out_token, size_t out_len)
{
    if (out_len < TOKEN_B64_LEN) return ESP_ERR_INVALID_ARG;
    uint8_t raw[TOKEN_BYTES];
    esp_fill_random(raw, sizeof(raw));
    char buf[TOKEN_B64_LEN];
    base64_url_encode(raw, sizeof(raw), buf);
    esp_err_t err = config_store_set_str(CONFIG_KEY_PAIR_TOKEN, buf);
    if (err != ESP_OK) return err;
    config_store_set_str(CONFIG_KEY_PAIR_MODE, "paired");
    s_paired_cached = true;
    snprintf(out_token, out_len, "%s", buf);
    ESP_LOGI(TAG, "Device paired (token rotated)");
    return ESP_OK;
}

esp_err_t pairing_unpair(void)
{
    config_store_set_str(CONFIG_KEY_PAIR_TOKEN, "");
    config_store_set_str(CONFIG_KEY_PAIR_MODE, "unpaired");
    s_paired_cached = false;
    ESP_LOGI(TAG, "Device unpaired");
    return ESP_OK;
}

esp_err_t pairing_get_token(char *out, size_t out_len)
{
    if (!s_paired_cached) return ESP_ERR_NOT_FOUND;
    return config_store_get_str(CONFIG_KEY_PAIR_TOKEN, out, out_len);
}

static esp_err_t send_401(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Bearer realm=\"plaiiinlight\"");
    httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"pairing token required\"}");
    return ESP_FAIL;
}

esp_err_t pairing_http_check(httpd_req_t *req)
{
    if (!s_paired_cached) return ESP_OK;
    // Provisioning bypass: when the device is in AP mode (no WiFi creds, or
    // WiFi reset), the user is on the captive portal and has no way to
    // produce a token. Auth would lock them out of recovery. The trade is
    // small — to reach AP mode you have to be physically present at the
    // device or power-cycle it to drop into provisioning.
    if (wifi_get_mode() == PLAIIIN_WIFI_AP) return ESP_OK;
    char hdr[128] = {0};
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr) - 1) != ESP_OK) {
        return send_401(req);
    }
    // Accept "Bearer <token>" — case sensitive on the prefix is fine, RFC 6750.
    const char *prefix = "Bearer ";
    size_t pl = strlen(prefix);
    if (strncmp(hdr, prefix, pl) != 0) return send_401(req);
    if (!pairing_check(hdr + pl)) return send_401(req);
    return ESP_OK;
}

esp_err_t pairing_ws_check(httpd_req_t *req)
{
    if (!s_paired_cached) return ESP_OK;
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen == 0) return send_401(req);
    char *q = malloc(qlen + 1);
    if (!q) return send_401(req);
    if (httpd_req_get_url_query_str(req, q, qlen + 1) != ESP_OK) { free(q); return send_401(req); }
    char tok[TOKEN_B64_LEN] = {0};
    esp_err_t got = httpd_query_key_value(q, "token", tok, sizeof(tok));
    free(q);
    if (got != ESP_OK) return send_401(req);
    if (!pairing_check(tok)) return send_401(req);
    return ESP_OK;
}
