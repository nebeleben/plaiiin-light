#include "reset_key_api.h"
#include "config_store.h"
#include "factory_reset.h"
#include "pairing.h"

#include "cJSON.h"
#include "mbedtls/sha256.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "reset_key_api";

// Lowercase-hex SHA-256 of `in` into `out` (needs 65 bytes incl. NUL).
static void sha256_hex(const char *in, char out[65])
{
    unsigned char digest[32];
    // is224 = 0 → SHA-256. One-shot; returns 0 on success (mbedTLS 3.x).
    (void)mbedtls_sha256((const unsigned char *)in, strlen(in), digest, 0);
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[i * 2]     = hx[digest[i] >> 4];
        out[i * 2 + 1] = hx[digest[i] & 0x0f];
    }
    out[64] = '\0';
}

// True when a non-empty recovery-key hash is stored.
static bool reset_key_armed(char *hash_out, size_t out_len)
{
    return config_store_get_str(CONFIG_KEY_RESET_KEY, hash_out, out_len) == ESP_OK
           && hash_out[0] != '\0';
}

// POST /api/reset-key — admin, must be paired. Mint + store hash, return once.
static esp_err_t reset_key_post_handler(httpd_req_t *req)
{
    if (pairing_http_check(req, PL_ROLE_ADMIN) != ESP_OK) return ESP_FAIL;
    if (!pairing_is_paired()) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"not claimed\"}");
        return ESP_OK;
    }
    char key[64];
    pl_token_generate(key, sizeof(key));
    char hash[65];
    sha256_hex(key, hash);
    if (config_store_set_str(CONFIG_KEY_RESET_KEY, hash) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "store failed");
        return ESP_FAIL;
    }
    ESP_LOGW(TAG, "Recovery key armed (hash stored)");   // never logs the key
    char resp[160];
    snprintf(resp, sizeof(resp),
             "{\"key\":\"%s\",\"note\":\"store this now; it cannot be shown again\"}",
             key);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

// DELETE /api/reset-key — admin. Clear the armed key (idempotent).
static esp_err_t reset_key_delete_handler(httpd_req_t *req)
{
    if (pairing_http_check(req, PL_ROLE_ADMIN) != ESP_OK) return ESP_FAIL;
    static const char *const keys[] = { CONFIG_KEY_RESET_KEY };
    config_store_erase_keys(keys, 1);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"cleared\"}");
    return ESP_OK;
}

// GET /api/reset-key — public. Boolean availability only.
static esp_err_t reset_key_get_handler(httpd_req_t *req)
{
    char hash[80] = {0};
    bool armed = reset_key_armed(hash, sizeof(hash));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, armed ? "{\"available\":true}" : "{\"available\":false}");
    return ESP_OK;
}

// POST /api/reset-key/redeem — public. The key IS the auth.
static esp_err_t reset_key_redeem_handler(httpd_req_t *req)
{
    char buf[128] = {0};
    int r = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (r <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body"); return ESP_FAIL; }

    cJSON *root = cJSON_Parse(buf);
    const cJSON *k = root ? cJSON_GetObjectItem(root, "key") : NULL;
    if (!cJSON_IsString(k) || k->valuestring[0] == '\0') {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing key");
        return ESP_FAIL;
    }

    char stored[80] = {0};
    char given[65];
    sha256_hex(k->valuestring, given);
    cJSON_Delete(root);

    // 401 for both "nothing armed" and "wrong key" — don't distinguish.
    if (!reset_key_armed(stored, sizeof(stored)) || !pairing_ct_strcmp(given, stored)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "invalid key");
        return ESP_FAIL;
    }

    ESP_LOGW(TAG, "Recovery key redeemed — running full factory reset");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"scope\":\"full\",\"note\":\"rebooting\"}");
    // Response is flushed above; this wipes pairing + personal data and reboots.
    factory_reset_full(/*reboot=*/true);
    return ESP_OK;
}

esp_err_t reset_key_api_register(httpd_handle_t server)
{
    httpd_uri_t post = {.uri = "/api/reset-key", .method = HTTP_POST,   .handler = reset_key_post_handler};
    httpd_register_uri_handler(server, &post);
    httpd_uri_t del  = {.uri = "/api/reset-key", .method = HTTP_DELETE, .handler = reset_key_delete_handler};
    httpd_register_uri_handler(server, &del);
    httpd_uri_t get  = {.uri = "/api/reset-key", .method = HTTP_GET,    .handler = reset_key_get_handler};
    httpd_register_uri_handler(server, &get);
    httpd_uri_t redeem = {.uri = "/api/reset-key/redeem", .method = HTTP_POST, .handler = reset_key_redeem_handler};
    httpd_register_uri_handler(server, &redeem);
    ESP_LOGI(TAG, "Recovery-key endpoints registered");
    return ESP_OK;
}
