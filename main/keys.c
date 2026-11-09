#include "keys.h"
#include "ai_access.h"
#include "config_store.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>

static const char *TAG = "keys";

// Cap so the serialized JSON array stays well under the NVS 4000-byte string
// limit (~140 bytes/entry).
#define MAX_SHARE_KEYS 24
// AI keys are capped separately (revoked ones count until deleted) — they
// carry three extra fields (~25 bytes) on top of a regular entry.
#define MAX_AI_KEYS    4
// Request body cap for POST/PATCH /api/keys.
#define KEYS_BODY_MAX  512
#define KEYS_BUF_SZ    4096

// --- store helpers ----------------------------------------------------------

// Bench diagnostics for the "share key rejected / can't be deleted until a
// reboot" reports: a failed load used to look exactly like an empty key list.
static void log_heap(const char *what)
{
    ESP_LOGE(TAG, "%s (free=%u largest=%u)", what,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

// Load the share-key array from NVS. Always returns a valid cJSON array (empty
// when the key is absent or unparseable). Caller must cJSON_Delete it.
static cJSON *load_array(void)
{
    char *buf = malloc(KEYS_BUF_SZ);
    if (!buf) {
        log_heap("load: no memory for the key buffer, treating list as empty");
        return cJSON_CreateArray();
    }
    cJSON *arr = NULL;
    esp_err_t err = config_store_get_str(CONFIG_KEY_SHARE_KEYS, buf, KEYS_BUF_SZ);
    if (err == ESP_OK && buf[0]) {
        arr = cJSON_Parse(buf);
        if (!arr) {
            ESP_LOGE(TAG, "load: %u stored bytes didn't parse", (unsigned)strlen(buf));
            log_heap("load: parse failed, treating list as empty");
        }
    } else if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "load: NVS read failed: %s", esp_err_to_name(err));
        log_heap("load: treating list as empty");
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
    if (!json) {
        log_heap("save: no memory to serialize the key list");
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = config_store_set_str(CONFIG_KEY_SHARE_KEYS, json);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save: NVS write of %u bytes failed: %s",
                 (unsigned)strlen(json), esp_err_to_name(err));
    }
    free(json);
    return err;
}

static pl_role_t role_from_name(const char *name)
{
    if (!name) return PL_ROLE_NONE;
    if (strcmp(name, "creator") == 0) return PL_ROLE_CREATOR;
    if (strcmp(name, "user") == 0)    return PL_ROLE_USER;
    if (strcmp(name, "ai") == 0)      return PL_ROLE_AI;
    return PL_ROLE_NONE;
}

static bool entry_is_ai(const cJSON *e)
{
    const cJSON *r = cJSON_GetObjectItem(e, "role");
    return cJSON_IsString(r) && r->valuestring && strcmp(r->valuestring, "ai") == 0;
}

// --- public API -------------------------------------------------------------

esp_err_t keys_init(void)
{
    cJSON *arr = load_array();
    ESP_LOGI(TAG, "%d share key(s) stored", cJSON_GetArraySize(arr));
    cJSON_Delete(arr);
    return ESP_OK;
}

bool keys_lookup(const char *token, pl_access_t *out)
{
    pl_access_t acc = { .role = PL_ROLE_NONE, .perms = 0,
                        .max_brightness = -1, .only_when_on = false };
    if (!token || !*token) {
        if (out) *out = acc;
        return false;
    }
    cJSON *arr = load_array();
    cJSON *e = NULL;
    cJSON_ArrayForEach(e, arr) {
        cJSON *k  = cJSON_GetObjectItem(e, "key");
        cJSON *rv = cJSON_GetObjectItem(e, "revoked");
        if (!cJSON_IsString(k) || !k->valuestring) continue;
        if (cJSON_IsTrue(rv)) continue;
        if (pairing_ct_strcmp(k->valuestring, token)) {
            cJSON *r = cJSON_GetObjectItem(e, "role");
            acc.role = role_from_name(cJSON_IsString(r) ? r->valuestring : NULL);
            if (acc.role == PL_ROLE_AI) {
                cJSON *jp  = cJSON_GetObjectItem(e, "p");
                cJSON *jmb = cJSON_GetObjectItem(e, "mb");
                acc.perms = cJSON_IsNumber(jp) ? ((uint32_t)jp->valuedouble & AI_PERM_ALL) : 0;
                if (cJSON_IsNumber(jmb) && jmb->valueint >= 0 && jmb->valueint <= 255) {
                    acc.max_brightness = (int16_t)jmb->valueint;
                }
                acc.only_when_on = cJSON_IsTrue(cJSON_GetObjectItem(e, "ow"));
            }
            break;
        }
    }
    if (acc.role == PL_ROLE_NONE) {
        ESP_LOGI(TAG, "token matched none of %d share key(s)", cJSON_GetArraySize(arr));
    }
    cJSON_Delete(arr);
    if (out) *out = acc;
    return acc.role != PL_ROLE_NONE;
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

// Render a stored entry for the HTTP API. Non-AI entries come out unchanged;
// AI entries swap the compact p/mb/ow for perms (names, bit order),
// maxBrightness (number or null) and onlyWhenOn. Returns a new object (caller
// owns it) or NULL on OOM.
static cJSON *render_entry(const cJSON *stored)
{
    cJSON *out = cJSON_Duplicate(stored, true);
    if (!out || !entry_is_ai(stored)) return out;

    cJSON *jp  = cJSON_GetObjectItem(stored, "p");
    cJSON *jmb = cJSON_GetObjectItem(stored, "mb");
    uint32_t mask = cJSON_IsNumber(jp) ? (uint32_t)jp->valuedouble : 0;
    cJSON_DeleteItemFromObject(out, "p");
    cJSON_DeleteItemFromObject(out, "mb");
    cJSON_DeleteItemFromObject(out, "ow");

    cJSON *perms = cJSON_AddArrayToObject(out, "perms");
    for (int b = 0; b < 32; b++) {
        const char *n = (mask & (1u << b)) ? ai_perm_name(1u << b) : NULL;
        if (n && perms) cJSON_AddItemToArray(perms, cJSON_CreateString(n));
    }
    if (cJSON_IsNumber(jmb) && jmb->valueint >= 0 && jmb->valueint <= 255) {
        cJSON_AddNumberToObject(out, "maxBrightness", jmb->valueint);
    } else {
        cJSON_AddNullToObject(out, "maxBrightness");
    }
    cJSON_AddBoolToObject(out, "onlyWhenOn", cJSON_IsTrue(cJSON_GetObjectItem(stored, "ow")));
    return out;
}

static esp_err_t send_entry(httpd_req_t *req, const cJSON *stored)
{
    cJSON *r = render_entry(stored);
    char *json = r ? cJSON_PrintUnformatted(r) : NULL;
    cJSON_Delete(r);
    esp_err_t ret = json ? send_json(req, json)
                         : httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    free(json);
    return ret;
}

// AI-key fields from a POST/PATCH body. has_* say which were present.
typedef struct {
    bool     has_perms, has_mb, has_ow;
    uint32_t perms;
    int      mb;   /* -1 = no cap */
    bool     ow;
} ai_fields_t;

// Validate perms/maxBrightness/onlyWhenOn in `in`. On error writes the
// message into `err` and returns false. `perms` must be present when
// require_perms; when present it must be a non-empty array of known names.
static bool parse_ai_fields(const cJSON *in, bool require_perms, ai_fields_t *f,
                            char *err, size_t err_len)
{
    memset(f, 0, sizeof(*f));
    f->mb = -1;

    const cJSON *jp = cJSON_GetObjectItem(in, "perms");
    if (jp) {
        if (!cJSON_IsArray(jp) || cJSON_GetArraySize(jp) == 0) {
            snprintf(err, err_len, "perms must be a non-empty list");
            return false;
        }
        const cJSON *n = NULL;
        cJSON_ArrayForEach(n, jp) {
            if (!cJSON_IsString(n) || !n->valuestring) {
                snprintf(err, err_len, "perms must be a list of names");
                return false;
            }
            uint32_t bit = ai_perm_from_name(n->valuestring);
            if (!bit) {
                snprintf(err, err_len, "unknown permission: %.32s", n->valuestring);
                return false;
            }
            f->perms |= bit;
        }
        f->has_perms = true;
    } else if (require_perms) {
        snprintf(err, err_len, "perms required");
        return false;
    }

    const cJSON *jmb = cJSON_GetObjectItem(in, "maxBrightness");
    if (jmb) {
        if (cJSON_IsNull(jmb)) {
            f->mb = -1;
        } else if (cJSON_IsNumber(jmb) && jmb->valuedouble >= 0 && jmb->valuedouble <= 255
                   && jmb->valuedouble == (double)jmb->valueint) {
            f->mb = jmb->valueint;
        } else {
            snprintf(err, err_len, "maxBrightness must be 0..255 or null");
            return false;
        }
        f->has_mb = true;
    }

    const cJSON *jow = cJSON_GetObjectItem(in, "onlyWhenOn");
    if (jow) {
        if (!cJSON_IsBool(jow)) {
            snprintf(err, err_len, "onlyWhenOn must be true or false");
            return false;
        }
        f->ow = cJSON_IsTrue(jow);
        f->has_ow = true;
    }
    return true;
}

static void set_number(cJSON *e, const char *name, double v)
{
    if (cJSON_GetObjectItem(e, name)) cJSON_ReplaceItemInObject(e, name, cJSON_CreateNumber(v));
    else cJSON_AddNumberToObject(e, name, v);
}

static void set_bool(cJSON *e, const char *name, bool v)
{
    if (cJSON_GetObjectItem(e, name)) cJSON_ReplaceItemInObject(e, name, cJSON_CreateBool(v));
    else cJSON_AddBoolToObject(e, name, v);
}

static void set_string(cJSON *e, const char *name, const char *v)
{
    if (cJSON_GetObjectItem(e, name)) cJSON_ReplaceItemInObject(e, name, cJSON_CreateString(v));
    else cJSON_AddStringToObject(e, name, v);
}

// GET /api/keys — admin. Returns {"keys":[ ...full entries incl. secrets... ]},
// AI entries rendered (perms/maxBrightness/onlyWhenOn).
static esp_err_t keys_get_handler(httpd_req_t *req)
{
    if (pairing_http_check(req, PL_ROLE_ADMIN) != ESP_OK) return ESP_FAIL;
    cJSON *arr = load_array();
    cJSON *root = cJSON_CreateObject();
    cJSON *list = cJSON_AddArrayToObject(root, "keys");
    bool oom = !root || !list;
    cJSON *e = NULL;
    cJSON_ArrayForEach(e, arr) {
        if (oom) break;
        cJSON *r = render_entry(e);
        if (!r) { oom = true; break; }
        cJSON_AddItemToArray(list, r);
    }
    cJSON_Delete(arr);
    char *json = oom ? NULL : cJSON_PrintUnformatted(root);
    esp_err_t ret = json ? send_json(req, json)
                         : httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    free(json);
    cJSON_Delete(root);
    return ret;
}

// POST /api/keys — admin. Body {"role":"user|creator","label":"..."} or
// {"role":"ai","label":"...","perms":[...],"maxBrightness":n|null,
// "onlyWhenOn":bool}. Creates a key, returns the new (rendered) entry.
static esp_err_t keys_post_handler(httpd_req_t *req)
{
    if (pairing_http_check(req, PL_ROLE_ADMIN) != ESP_OK) return ESP_FAIL;

    char *body = recv_body(req, KEYS_BODY_MAX);
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
    bool is_ai = strcmp(role, "ai") == 0;
    ai_fields_t ai;
    char err[64] = {0};
    bool ai_ok = !is_ai || parse_ai_fields(in, true, &ai, err, sizeof(err));
    cJSON_Delete(in);

    // Only user/creator/ai can be minted here — admin is the pairing token only.
    if (!is_ai && strcmp(role, "user") != 0 && strcmp(role, "creator") != 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "role must be user, creator or ai");
        return ESP_FAIL;
    }
    if (!ai_ok) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err);
        return ESP_FAIL;
    }

    cJSON *arr = load_array();
    if (cJSON_GetArraySize(arr) >= MAX_SHARE_KEYS) {
        cJSON_Delete(arr);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "share-key limit reached");
        return ESP_FAIL;
    }
    if (is_ai) {
        int n_ai = 0;
        cJSON *it = NULL;
        cJSON_ArrayForEach(it, arr) if (entry_is_ai(it)) n_ai++;
        if (n_ai >= MAX_AI_KEYS) {
            cJSON_Delete(arr);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ai-key limit reached");
            return ESP_FAIL;
        }
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
    if (is_ai) {
        cJSON_AddNumberToObject(e, "p", ai.perms);
        cJSON_AddNumberToObject(e, "mb", ai.has_mb ? ai.mb : -1);
        cJSON_AddBoolToObject(e, "ow", ai.has_ow && ai.ow);
    }
    cJSON_AddItemToArray(arr, e);

    esp_err_t serr = save_array(arr);
    if (serr != ESP_OK) {
        cJSON_Delete(arr);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "share key created: id=%s role=%s", id, role);
    esp_err_t ret = send_entry(req, e);
    cJSON_Delete(arr);
    return ret;
}

// PATCH /api/keys/<id> — admin. Edits an AI key: any of perms,
// maxBrightness, onlyWhenOn, label. Returns the updated (rendered) entry.
static esp_err_t keys_patch_handler(httpd_req_t *req)
{
    if (pairing_http_check(req, PL_ROLE_ADMIN) != ESP_OK) return ESP_FAIL;
    char id[16] = {0};
    if (!parse_key_id(req->uri, id, sizeof(id))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing key id");
        return ESP_FAIL;
    }

    char *body = recv_body(req, KEYS_BODY_MAX);
    cJSON *in = body ? cJSON_Parse(body) : NULL;
    free(body);
    if (!in || !cJSON_IsObject(in)) {
        cJSON_Delete(in);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad request body");
        return ESP_FAIL;
    }
    ai_fields_t ai;
    char err[64] = {0};
    bool ok = parse_ai_fields(in, false, &ai, err, sizeof(err));
    cJSON *jlabel = cJSON_GetObjectItem(in, "label");
    bool has_label = jlabel != NULL;
    char label[33] = {0};
    if (ok && has_label) {
        if (cJSON_IsString(jlabel) && jlabel->valuestring) {
            strncpy(label, jlabel->valuestring, sizeof(label) - 1);
        } else {
            snprintf(err, sizeof(err), "label must be a string");
            ok = false;
        }
    }
    cJSON_Delete(in);

    cJSON *arr = load_array();
    cJSON *e = NULL;
    cJSON_ArrayForEach(e, arr) {
        cJSON *jid = cJSON_GetObjectItem(e, "id");
        if (cJSON_IsString(jid) && strcmp(jid->valuestring, id) == 0) break;
    }
    if (!e) {
        int count = cJSON_GetArraySize(arr);
        cJSON_Delete(arr);
        ESP_LOGW(TAG, "patch: id=%s not among %d share key(s)", id, count);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such key");
        return ESP_FAIL;
    }
    if (!entry_is_ai(e)) {
        cJSON_Delete(arr);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "not an ai key");
        return ESP_FAIL;
    }
    if (!ok) {
        cJSON_Delete(arr);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err);
        return ESP_FAIL;
    }

    if (ai.has_perms) set_number(e, "p", ai.perms);
    if (ai.has_mb)    set_number(e, "mb", ai.mb);
    if (ai.has_ow)    set_bool(e, "ow", ai.ow);
    if (has_label)    set_string(e, "label", label);

    esp_err_t serr = save_array(arr);
    if (serr != ESP_OK) {
        cJSON_Delete(arr);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "ai key edited: id=%s", id);
    esp_err_t ret = send_entry(req, e);
    cJSON_Delete(arr);
    return ret;
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
    int count = cJSON_GetArraySize(arr);
    cJSON_Delete(arr);
    if (!found) {
        ESP_LOGW(TAG, "revoke: id=%s not among %d share key(s)", id, count);
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
    int count = cJSON_GetArraySize(arr);
    cJSON_Delete(arr);
    if (!found) {
        ESP_LOGW(TAG, "delete: id=%s not among %d share key(s)", id, count);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such key");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "share key removed: id=%s", id);
    return send_json(req, "{\"status\":\"ok\"}");
}

// GET /api/whoami — any caller. Reports the role the request's token grants.
static esp_err_t whoami_handler(httpd_req_t *req)
{
    pl_access_t acc = pairing_resolve_access(req);
    pl_role_t role = acc.role;
    ESP_LOGI(TAG, "whoami: role=%s", pl_role_name(role));
    if (role != PL_ROLE_AI) {
        char out[64];
        snprintf(out, sizeof(out), "{\"role\":\"%s\",\"paired\":%s}",
                 pl_role_name(role), pairing_is_paired() ? "true" : "false");
        return send_json(req, out);
    }
    // AI key: also report what it may do — perms (names, bit order),
    // maxBrightness (number or null) and onlyWhenOn.
    char out[256];
    int n = snprintf(out, sizeof(out), "{\"role\":\"ai\",\"paired\":true,\"perms\":[");
    bool first = true;
    for (int b = 0; b < 32 && n < (int)sizeof(out); b++) {
        const char *name = (acc.perms & (1u << b)) ? ai_perm_name(1u << b) : NULL;
        if (!name) continue;
        n += snprintf(out + n, sizeof(out) - n, "%s\"%s\"", first ? "" : ",", name);
        first = false;
    }
    if (n < (int)sizeof(out)) {
        if (acc.max_brightness >= 0) {
            n += snprintf(out + n, sizeof(out) - n, "],\"maxBrightness\":%d", acc.max_brightness);
        } else {
            n += snprintf(out + n, sizeof(out) - n, "],\"maxBrightness\":null");
        }
    }
    if (n < (int)sizeof(out)) {
        snprintf(out + n, sizeof(out) - n, ",\"onlyWhenOn\":%s}",
                 acc.only_when_on ? "true" : "false");
    }
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
    // PATCH /api/keys/<id> — edit an AI key. Last registration on the server:
    // log a full handler table instead of failing silently.
    httpd_uri_t kpatch = { .uri = "/api/keys/*", .method = HTTP_PATCH, .handler = keys_patch_handler };
    esp_err_t perr = httpd_register_uri_handler(server, &kpatch);
    if (perr != ESP_OK) {
        ESP_LOGE(TAG, "PATCH /api/keys/* not registered: %s", esp_err_to_name(perr));
    }
}
