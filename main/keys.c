#include "keys.h"
#include "config_store.h"
#include "esp_log.h"
#include "esp_random.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>

static const char *TAG = "keys";

// Cap so the serialized JSON array stays well under the NVS 4000-byte string
// limit (~140 bytes/entry).
#define MAX_SHARE_KEYS 24
#define KEYS_BUF_SZ    4096

// --- store helpers ----------------------------------------------------------

// Load the share-key array from NVS. Always returns a valid cJSON array (empty
// when the key is absent or unparseable). Caller must cJSON_Delete it.
static cJSON *load_array(void)
{
    char *buf = malloc(KEYS_BUF_SZ);
    if (!buf) return cJSON_CreateArray();
    cJSON *arr = NULL;
    if (config_store_get_str(CONFIG_KEY_SHARE_KEYS, buf, KEYS_BUF_SZ) == ESP_OK && buf[0]) {
        arr = cJSON_Parse(buf);
    }
    free(buf);
    if (!arr || !cJSON_IsArray(arr)) {
        if (arr) cJSON_Delete(arr);
        arr = cJSON_CreateArray();
    }
    return arr;
}

static esp_err_t save_array(cJSON *arr)
{
    char *json = cJSON_PrintUnformatted(arr);
    if (!json) return ESP_ERR_NO_MEM;
    esp_err_t err = config_store_set_str(CONFIG_KEY_SHARE_KEYS, json);
    free(json);
    return err;
}

static pl_role_t role_from_name(const char *name)
{
    if (!name) return PL_ROLE_NONE;
    if (strcmp(name, "creator") == 0) return PL_ROLE_CREATOR;
    if (strcmp(name, "user") == 0)    return PL_ROLE_USER;
    return PL_ROLE_NONE;
}

// --- public API -------------------------------------------------------------

esp_err_t keys_init(void)
{
    cJSON *arr = load_array();
    ESP_LOGI(TAG, "%d share key(s) stored", cJSON_GetArraySize(arr));
    cJSON_Delete(arr);
    return ESP_OK;
}

pl_role_t keys_role_for(const char *token)
{
    if (!token || !*token) return PL_ROLE_NONE;
    cJSON *arr = load_array();
    pl_role_t result = PL_ROLE_NONE;
    cJSON *e = NULL;
    cJSON_ArrayForEach(e, arr) {
        cJSON *k  = cJSON_GetObjectItem(e, "key");
        cJSON *rv = cJSON_GetObjectItem(e, "revoked");
        if (!cJSON_IsString(k) || !k->valuestring) continue;
        if (cJSON_IsTrue(rv)) continue;
        if (pairing_ct_strcmp(k->valuestring, token)) {
            cJSON *r = cJSON_GetObjectItem(e, "role");
            result = role_from_name(cJSON_IsString(r) ? r->valuestring : NULL);
            break;
        }
    }
    cJSON_Delete(arr);
    return result;
}

void keys_clear_all(void)
{
    const char *k[] = { CONFIG_KEY_SHARE_KEYS };
    config_store_erase_keys(k, 1);
}

// --- HTTP handlers ----------------------------------------------------------

// Read the request body into a heap buffer (NUL-terminated). NULL on failure
// or empty body. Caller frees.
static char *recv_body(httpd_req_t *req, int max_len)
{
    int len = req->content_len;
    if (len <= 0 || len > max_len) return NULL;
    char *buf = malloc(len + 1);
    if (!buf) return NULL;
    int got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, buf + got, len - got);
        if (r <= 0) { free(buf); return NULL; }
        got += r;
    }
    buf[len] = '\0';
    return buf;
}

static esp_err_t send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    return ESP_OK;
}

// Extract the <id> segment from a URI like /api/keys/<id> or
// /api/keys/<id>/revoke into `out`. Returns false if absent.
static bool parse_key_id(const char *uri, char *out, size_t out_len)
{
    const char *p = strstr(uri, "/api/keys/");
    if (!p) return false;
    p += strlen("/api/keys/");
    size_t n = 0;
    while (p[n] && p[n] != '/' && p[n] != '?' && n + 1 < out_len) {
        out[n] = p[n];
        n++;
    }
    out[n] = '\0';
    return n > 0;
}

// GET /api/keys — admin. Returns {"keys":[ ...full entries incl. secrets... ]}.
static esp_err_t keys_get_handler(httpd_req_t *req)
{
    if (pairing_http_check(req, PL_ROLE_ADMIN) != ESP_OK) return ESP_FAIL;
    cJSON *arr = load_array();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "keys", arr);  // transfers ownership of arr
    char *json = cJSON_PrintUnformatted(root);
    esp_err_t ret = json ? send_json(req, json)
                         : httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    free(json);
    cJSON_Delete(root);
    return ret;
}

// POST /api/keys — admin. Body {"role":"user|creator","label":"..."}.
// Creates a key, returns the new entry.
static esp_err_t keys_post_handler(httpd_req_t *req)
{
    if (pairing_http_check(req, PL_ROLE_ADMIN) != ESP_OK) return ESP_FAIL;

    char *body = recv_body(req, 512);
    cJSON *in = body ? cJSON_Parse(body) : NULL;
    free(body);
    if (!in) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad request body");
        return ESP_FAIL;
    }
    cJSON *jrole = cJSON_GetObjectItem(in, "role");
    cJSON *jlabel = cJSON_GetObjectItem(in, "label");
    // Copy role + label into local buffers — they point into `in`, which is
    // freed below; using them afterwards would be a use-after-free.
    char role[16] = {0};
    if (cJSON_IsString(jrole) && jrole->valuestring) {
        strncpy(role, jrole->valuestring, sizeof(role) - 1);
    }
    char label[33] = {0};
    if (cJSON_IsString(jlabel) && jlabel->valuestring) {
        strncpy(label, jlabel->valuestring, sizeof(label) - 1);
    }
    cJSON_Delete(in);

    // Only user/creator can be minted here — admin is the pairing token only.
    if (strcmp(role, "user") != 0 && strcmp(role, "creator") != 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "role must be user or creator");
        return ESP_FAIL;
    }

    cJSON *arr = load_array();
    if (cJSON_GetArraySize(arr) >= MAX_SHARE_KEYS) {
        cJSON_Delete(arr);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "share-key limit reached");
        return ESP_FAIL;
    }

    // 8-hex id, 43-char base64url key.
    uint8_t idraw[4];
    esp_fill_random(idraw, sizeof(idraw));
    char id[9];
    snprintf(id, sizeof(id), "%02x%02x%02x%02x", idraw[0], idraw[1], idraw[2], idraw[3]);
    char key[64];
    pl_token_generate(key, sizeof(key));

    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "id", id);
    cJSON_AddStringToObject(e, "key", key);
    cJSON_AddStringToObject(e, "role", role);
    cJSON_AddStringToObject(e, "label", label);
    cJSON_AddNumberToObject(e, "created", (double)time(NULL));
    cJSON_AddBoolToObject(e, "revoked", false);
    cJSON_AddItemToArray(arr, e);

    esp_err_t serr = save_array(arr);
    cJSON_Delete(arr);
    if (serr != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "share key created: id=%s role=%s", id, role);

    char out[256];
    snprintf(out, sizeof(out),
        "{\"id\":\"%s\",\"key\":\"%s\",\"role\":\"%s\",\"label\":\"%s\",\"revoked\":false}",
        id, key, role, label);
    return send_json(req, out);
}

// POST /api/keys/<id>/revoke — admin. Marks the entry revoked (kept in list).
static esp_err_t keys_revoke_handler(httpd_req_t *req)
{
    if (pairing_http_check(req, PL_ROLE_ADMIN) != ESP_OK) return ESP_FAIL;
    char id[16] = {0};
    if (!parse_key_id(req->uri, id, sizeof(id))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing key id");
        return ESP_FAIL;
    }
    cJSON *arr = load_array();
    bool found = false;
    cJSON *e = NULL;
    cJSON_ArrayForEach(e, arr) {
        cJSON *jid = cJSON_GetObjectItem(e, "id");
        if (cJSON_IsString(jid) && strcmp(jid->valuestring, id) == 0) {
            cJSON_ReplaceItemInObject(e, "revoked", cJSON_CreateBool(true));
            found = true;
            break;
        }
    }
    if (found) save_array(arr);
    cJSON_Delete(arr);
    if (!found) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such key");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "share key revoked: id=%s", id);
    return send_json(req, "{\"status\":\"ok\"}");
}

// DELETE /api/keys/<id> — admin. Drops the entry entirely.
static esp_err_t keys_delete_handler(httpd_req_t *req)
{
    if (pairing_http_check(req, PL_ROLE_ADMIN) != ESP_OK) return ESP_FAIL;
    char id[16] = {0};
    if (!parse_key_id(req->uri, id, sizeof(id))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing key id");
        return ESP_FAIL;
    }
    cJSON *arr = load_array();
    bool found = false;
    int idx = 0;
    cJSON *e = NULL;
    cJSON_ArrayForEach(e, arr) {
        cJSON *jid = cJSON_GetObjectItem(e, "id");
        if (cJSON_IsString(jid) && strcmp(jid->valuestring, id) == 0) {
            found = true;
            break;
        }
        idx++;
    }
    if (found) {
        cJSON_DeleteItemFromArray(arr, idx);
        save_array(arr);
    }
    cJSON_Delete(arr);
    if (!found) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such key");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "share key removed: id=%s", id);
    return send_json(req, "{\"status\":\"ok\"}");
}

// GET /api/whoami — any caller. Reports the role the request's token grants.
static esp_err_t whoami_handler(httpd_req_t *req)
{
    pl_role_t role = pairing_resolve_role(req);
    char out[64];
    snprintf(out, sizeof(out), "{\"role\":\"%s\",\"paired\":%s}",
             pl_role_name(role), pairing_is_paired() ? "true" : "false");
    return send_json(req, out);
}

void keys_api_register(httpd_handle_t server)
{
    httpd_uri_t whoami = { .uri = "/api/whoami", .method = HTTP_GET, .handler = whoami_handler };
    httpd_register_uri_handler(server, &whoami);

    httpd_uri_t kget = { .uri = "/api/keys", .method = HTTP_GET, .handler = keys_get_handler };
    httpd_register_uri_handler(server, &kget);
    httpd_uri_t kpost = { .uri = "/api/keys", .method = HTTP_POST, .handler = keys_post_handler };
    httpd_register_uri_handler(server, &kpost);
    // Wildcard routes for the per-id operations. POST /api/keys/<id>/revoke,
    // DELETE /api/keys/<id>. The exact /api/keys routes above are registered
    // first so the matcher prefers them.
    httpd_uri_t krevoke = { .uri = "/api/keys/*", .method = HTTP_POST, .handler = keys_revoke_handler };
    httpd_register_uri_handler(server, &krevoke);
    httpd_uri_t kdel = { .uri = "/api/keys/*", .method = HTTP_DELETE, .handler = keys_delete_handler };
    httpd_register_uri_handler(server, &kdel);
}
