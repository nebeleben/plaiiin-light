#include "pairing.h"
#include "keys.h"
#include "ai_access.h"
#include "config_store.h"
#include "led_control.h"
#include "wifi.h"

#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "pairing";

// 32 random bytes → 43 chars URL-safe base64. We add 2 chars of headroom +
// NUL so callers always have room for a clean copy. TOKEN_B64_LEN is declared
// in pairing.h so other modules (e.g. reset_key_api.c) size buffers from it too.
#define TOKEN_BYTES 32

static bool s_paired_cached = false;
// Sticky "has been claimed before" flag — see CONFIG_KEY_PROVISIONED. Cached at
// init; set on the first claim and only ever cleared by a factory reset (which
// reboots, so we reload it fresh rather than mutate this in the reset path).
static bool s_provisioned_cached = false;

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

void pl_token_generate(char *out, size_t out_len)
{
    if (out_len < TOKEN_B64_LEN) { if (out_len) out[0] = 0; return; }
    uint8_t raw[TOKEN_BYTES];
    esp_fill_random(raw, sizeof(raw));
    base64_url_encode(raw, sizeof(raw), out);
}

const char *pl_role_name(pl_role_t role)
{
    switch (role) {
        case PL_ROLE_ADMIN:   return "admin";
        case PL_ROLE_CREATOR: return "creator";
        case PL_ROLE_USER:    return "user";
        case PL_ROLE_AI:      return "ai";
        default:              return "none";
    }
}

esp_err_t pairing_init(void)
{
    char mode[16] = {0};
    config_get_str_or(CONFIG_KEY_PAIR_MODE, mode, sizeof(mode), "unpaired");
    s_paired_cached = (strcmp(mode, "paired") == 0);
    char prov[8] = {0};
    config_get_str_or(CONFIG_KEY_PROVISIONED, prov, sizeof(prov), "0");
    s_provisioned_cached = (prov[0] == '1');
    ESP_LOGI(TAG, "pair_mode=%s provisioned=%d",
             s_paired_cached ? "paired" : "unpaired", s_provisioned_cached);
    return ESP_OK;
}

bool pairing_is_paired(void) { return s_paired_cached; }

bool pairing_is_provisioned(void) { return s_provisioned_cached; }

// Constant-time compare — bails on the loop count, not the conditional, so a
// network attacker can't time-distinguish an early mismatch and binary-search.
bool pairing_ct_strcmp(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    if (la != lb) return false;
    uint8_t diff = 0;
    for (size_t i = 0; i < la; i++) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

static const pl_access_t k_admin_access = {
    .role = PL_ROLE_ADMIN, .perms = 0, .max_brightness = -1, .only_when_on = false,
};
static const pl_access_t k_no_access = {
    .role = PL_ROLE_NONE, .perms = 0, .max_brightness = -1, .only_when_on = false,
};

static pl_access_t access_for_token(const char *token)
{
    if (!s_paired_cached) return k_admin_access;
    if (!token || !*token) return k_no_access;

    char stored[TOKEN_B64_LEN];
    esp_err_t err = config_store_get_str(CONFIG_KEY_PAIR_TOKEN, stored, sizeof(stored));
    if (err != ESP_OK) {
        // Paired mode but no admin token? Self-heal back to unpaired so the
        // device can't brick — and warn loudly.
        ESP_LOGW(TAG, "paired without token; reverting to unpaired");
        config_store_set_str(CONFIG_KEY_PAIR_MODE, "unpaired");
        s_paired_cached = false;
        return k_admin_access;
    }
    if (pairing_ct_strcmp(stored, token)) return k_admin_access;

    // Not the admin token — try the limited-role share keys (user, creator,
    // ai). Looked up on every request, so a revoke/delete bites immediately.
    pl_access_t acc;
    if (!keys_lookup(token, &acc)) return k_no_access;
    return acc;
}

pl_role_t pairing_role_for_token(const char *token)
{
    return access_for_token(token).role;
}

bool pairing_check(const char *token)
{
    // AI keys are excluded: the one caller (http_server.c page meta) embeds
    // the admin token for an authenticated page load.
    pl_role_t role = pairing_role_for_token(token);
    return role != PL_ROLE_NONE && role != PL_ROLE_AI;
}

pl_access_t pairing_resolve_access(httpd_req_t *req)
{
    if (!s_paired_cached) return k_admin_access;
    // Provisioning bypass: in AP mode the user is on the captive portal with
    // no way to produce a token — auth would lock them out of recovery.
    if (wifi_get_mode() == PLAIIIN_WIFI_AP) return k_admin_access;

    char hdr[160] = {0};
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr) - 1) != ESP_OK) {
        return k_no_access;
    }
    const char *prefix = "Bearer ";
    size_t pl = strlen(prefix);
    if (strncmp(hdr, prefix, pl) != 0) return k_no_access;
    return access_for_token(hdr + pl);
}

pl_role_t pairing_resolve_role(httpd_req_t *req)
{
    return pairing_resolve_access(req).role;
}

esp_err_t pairing_pair(char *out_token, size_t out_len)
{
    if (out_len < TOKEN_B64_LEN) return ESP_ERR_INVALID_ARG;
    char buf[TOKEN_B64_LEN];
    pl_token_generate(buf, sizeof(buf));
    esp_err_t err = config_store_set_str(CONFIG_KEY_PAIR_TOKEN, buf);
    if (err != ESP_OK) return err;
    config_store_set_str(CONFIG_KEY_PAIR_MODE, "paired");
    s_paired_cached = true;
    // Sticky: once claimed, the lamp never auto-reopens its provisioning AP on a
    // later unpair. Survives pairing_unpair(); only a factory reset clears it.
    config_store_set_str(CONFIG_KEY_PROVISIONED, "1");
    s_provisioned_cached = true;
    snprintf(out_token, out_len, "%s", buf);
    ESP_LOGI(TAG, "Device paired (admin token rotated)");
    return ESP_OK;
}

esp_err_t pairing_unpair(void)
{
    config_store_set_str(CONFIG_KEY_PAIR_TOKEN, "");
    config_store_set_str(CONFIG_KEY_PAIR_MODE, "unpaired");
    // Sharing only exists on a paired lamp — drop every share key too.
    keys_clear_all();
    // NOTE: the recovery key (CONFIG_KEY_RESET_KEY) is deliberately NOT cleared
    // here. It's a durable, owner-minted recovery secret that outlives claim,
    // unclaim, and factory resets — only an explicit generate/DELETE on
    // /api/reset-key replaces or removes it. See reset_key_api.c.
    s_paired_cached = false;
    // NOTE: CONFIG_KEY_PROVISIONED is deliberately NOT cleared here. Releasing
    // ownership from the owner's app must not silently reopen the unauthenticated
    // provisioning AP — the lamp stays BLE-only (and re-claimable over BLE) until
    // an explicit factory reset. See wifi_init() + SECURITY.md.
    ESP_LOGI(TAG, "Device unpaired (provisioned flag retained — no AP fallback)");
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

static esp_err_t send_403_msg(httpd_req_t *req, const char *msg)
{
    char body[96];
    snprintf(body, sizeof(body), "{\"status\":\"error\",\"message\":\"%s\"}", msg);
    httpd_resp_set_status(req, "403 Forbidden");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, body);
    return ESP_FAIL;
}

static esp_err_t send_403(httpd_req_t *req)
{
    return send_403_msg(req, "insufficient role");
}

// Brightness cap of the AI key that authorised the request now running on
// the httpd task. Cleared at the start of every pairing_http_check /
// pairing_ws_check, set only when a capped AI key passes. Bound to the task
// that set it, so a brightness change on any other task (buttons, swarm,
// MQTT, scheduler, BLE) is never clamped. Only the httpd task writes it.
static volatile int16_t      s_cap_value = -1;
static volatile TaskHandle_t s_cap_task  = NULL;

static void cap_clear(void)
{
    s_cap_task  = NULL;
    s_cap_value = -1;
}

uint8_t pairing_clamp_brightness(uint8_t value)
{
    TaskHandle_t task = s_cap_task;
    int16_t cap = s_cap_value;
    if (task == NULL || task != xTaskGetCurrentTaskHandle() || cap < 0) return value;
    return value > cap ? (uint8_t)cap : value;
}

static ai_method_t method_of(httpd_req_t *req)
{
    switch (req->method) {
        case HTTP_GET:    return AI_M_GET;
        case HTTP_POST:   return AI_M_POST;
        case HTTP_PUT:    return AI_M_PUT;
        case HTTP_PATCH:  return AI_M_PATCH;
        case HTTP_DELETE: return AI_M_DELETE;
        default:          return AI_M_OTHER;
    }
}

// POST /api/power, query and one trailing slash ignored (as ai_route_perm).
static bool is_power_post(ai_method_t m, const char *uri)
{
    if (m != AI_M_POST) return false;
    size_t n = strcspn(uri, "?");
    if (n > 1 && uri[n - 1] == '/') n--;
    return n == 10 && strncmp(uri, "/api/power", 10) == 0;
}

// The AI-key branch of both checks. `acc->role` is PL_ROLE_AI. The role
// ladder does not apply: the route's permission decides (ai_access.c).
static esp_err_t ai_check(httpd_req_t *req, const pl_access_t *acc, pl_role_t min_role)
{
    ai_method_t m = method_of(req);
    uint32_t p = ai_route_perm(m, req->uri);
    if (p == AI_ROUTE_DENIED) {
        ESP_LOGI(TAG, "403 %s (ai: denied route)", req->uri);
        return send_403(req);
    }
    bool is_read = (p == AI_ROUTE_READ);
    if (is_read) {
        // A plain read is fine only where any user may read; admin/creator
        // reads (key list, Wi-Fi, ...) stay closed.
        if (min_role > PL_ROLE_USER) {
            ESP_LOGI(TAG, "403 %s (ai: read needs %s)", req->uri, pl_role_name(min_role));
            return send_403(req);
        }
    } else if ((acc->perms & p) == 0) {
        ESP_LOGI(TAG, "403 %s (ai: missing %s)", req->uri, ai_perm_name(p) ? ai_perm_name(p) : "?");
        return send_403_msg(req, "not permitted for this AI key");
    }
    // Only-while-on: a switched-off lamp accepts reads and power-on only.
    if (acc->only_when_on && !is_read && !is_power_post(m, req->uri)
        && !led_control_is_on()) {
        ESP_LOGI(TAG, "403 %s (ai: lamp is off)", req->uri);
        return send_403_msg(req, "lamp is off");
    }
    if (acc->max_brightness >= 0) {
        s_cap_value = acc->max_brightness;
        s_cap_task  = xTaskGetCurrentTaskHandle();
    }
    return ESP_OK;
}

esp_err_t pairing_http_check(httpd_req_t *req, pl_role_t min_role)
{
    cap_clear();
    pl_access_t acc = pairing_resolve_access(req);
    if (acc.role == PL_ROLE_NONE) {
        ESP_LOGI(TAG, "401 %s", req->uri);
        return send_401(req);
    }
    if (acc.role == PL_ROLE_AI) return ai_check(req, &acc, min_role);
    if (acc.role < min_role) {
        ESP_LOGI(TAG, "403 %s (role=%s)", req->uri, pl_role_name(acc.role));
        return send_403(req);
    }
    return ESP_OK;
}

esp_err_t pairing_ws_check(httpd_req_t *req, pl_role_t min_role)
{
    cap_clear();
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
    pl_access_t acc = access_for_token(tok);
    if (acc.role == PL_ROLE_NONE) return send_401(req);
    if (acc.role == PL_ROLE_AI)   return ai_check(req, &acc, min_role);
    if (acc.role < min_role)      return send_403(req);
    return ESP_OK;
}
