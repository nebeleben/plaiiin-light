#include "image_api.h"
#include "image_store.h"
#include "config_store.h"
#include "frame_store.h"
#include "led_control.h"
#include "light_api.h"
#include "pairing.h"

#include "cJSON.h"
#include "esp_log.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "image_api";

static esp_err_t send_err_json(httpd_req_t *req, int status, const char *msg)
{
    httpd_resp_set_status(req, status == 404 ? "404 Not Found"
                              : status == 400 ? "400 Bad Request"
                              : status == 409 ? "409 Conflict"
                              : "500 Internal Server Error");
    httpd_resp_set_type(req, "application/json");
    char resp[128];
    snprintf(resp, sizeof(resp), "{\"status\":\"error\",\"message\":\"%s\"}", msg);
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

/** Extract the image name from "/api/images/<name>", ".../<name>/show" or
 *  ".../<name>/label" (is_label may be NULL where that sub-resource isn't
 *  served). Same shape as js_api's split_uri. Returns false on a malformed URI. */
static bool split_uri_ex(const char *uri, char *out, size_t out_cap, bool *is_show,
                         bool *is_label)
{
    *is_show = false;
    if (is_label) *is_label = false;
    if (out_cap == 0) return false;
    out[0] = '\0';
    const char *prefix = "/api/images/";
    if (strncmp(uri, prefix, strlen(prefix)) != 0) return false;
    const char *name = uri + strlen(prefix);
    if (*name == 0) return false;
    size_t len = strlen(name);
    const char *suffix = "/show";
    size_t slen = strlen(suffix);
    const char *lsuffix = "/label";
    size_t lslen = strlen(lsuffix);
    if (len > slen && strcmp(name + len - slen, suffix) == 0) {
        *is_show = true;
        len -= slen;
    } else if (is_label && len > lslen && strcmp(name + len - lslen, lsuffix) == 0) {
        *is_label = true;
        len -= lslen;
    }
    if (len + 1 > out_cap) return false;
    memcpy(out, name, len);
    out[len] = '\0';
    return out[0] != '\0';
}

static bool split_uri(const char *uri, char *out, size_t out_cap, bool *is_show)
{
    return split_uri_ex(uri, out, out_cap, is_show, NULL);
}

static char *recv_body(httpd_req_t *req, size_t max)
{
    int len = req->content_len;
    if (len <= 0 || (size_t)len > max) return NULL;
    char *buf = malloc((size_t)len + 1);
    if (!buf) return NULL;
    int got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, buf + got, len - got);
        if (r <= 0) { free(buf); return NULL; }
        got += r;
    }
    buf[len] = 0;
    return buf;
}

/* Optional {"label":"…"} body. *out_label is "" without a body or label.
 * Returns false for a body that isn't valid JSON or has a non-string label. */
static bool read_label_body(httpd_req_t *req, char *out_label, size_t cap)
{
    out_label[0] = '\0';
    if (req->content_len <= 0) return true;
    char *body = recv_body(req, 256);
    if (!body) return false;
    cJSON *doc = cJSON_Parse(body);
    free(body);
    if (!doc) return false;
    const cJSON *l = cJSON_GetObjectItemCaseSensitive(doc, "label");
    bool ok = true;
    if (cJSON_IsString(l) && l->valuestring) {
        if (strlen(l->valuestring) >= cap) ok = false;
        else strcpy(out_label, l->valuestring);
    } else if (l && !cJSON_IsNull(l)) {
        ok = false;
    }
    cJSON_Delete(doc);
    return ok;
}

// GET /api/images -> {"status":"ok","images":[{name,w,h[,label]}...],"count":N,"max":30}
// "label" is present only when the image has one.
static esp_err_t images_list_handler(httpd_req_t *req)
{
    if (pairing_http_check(req, PL_ROLE_USER) != ESP_OK) return ESP_FAIL;
    image_store_entry_t *entries = malloc(sizeof(image_store_entry_t) * IMAGE_STORE_MAX);
    if (!entries) return send_err_json(req, 500, "out of memory");
    int count = image_store_list(entries, IMAGE_STORE_MAX);

    cJSON *doc = cJSON_CreateObject();
    cJSON_AddStringToObject(doc, "status", "ok");
    cJSON *arr = cJSON_AddArrayToObject(doc, "images");
    for (int i = 0; i < count; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", entries[i].name);
        cJSON_AddNumberToObject(o, "w", entries[i].w);
        cJSON_AddNumberToObject(o, "h", entries[i].h);
        if (entries[i].label[0]) cJSON_AddStringToObject(o, "label", entries[i].label);
        cJSON_AddItemToArray(arr, o);
    }
    cJSON_AddNumberToObject(doc, "count", count);
    cJSON_AddNumberToObject(doc, "max", IMAGE_STORE_MAX);
    free(entries);
    char *out = cJSON_PrintUnformatted(doc);
    cJSON_Delete(doc);
    if (!out) return send_err_json(req, 500, "out of memory");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out);
    cJSON_free(out);
    return ESP_OK;
}

// POST /api/images [{"label":"…"}] — snapshot the current stored frame into a
// new slot, optionally labelled.
static esp_err_t images_save_handler(httpd_req_t *req)
{
    if (pairing_http_check(req, PL_ROLE_CREATOR) != ESP_OK) return ESP_FAIL;
    char label[IMAGE_STORE_LABEL_MAX + 1];
    if (!read_label_body(req, label, sizeof(label))) return send_err_json(req, 400, "bad label");
    char name[IMAGE_STORE_NAME_CAP];
    esp_err_t err = image_store_save_current(label, name, sizeof(name));
    if (err == ESP_ERR_INVALID_ARG) return send_err_json(req, 400, "bad label");
    if (err == ESP_ERR_NOT_FOUND) return send_err_json(req, 404, "no frame stored");
    if (err == ESP_ERR_NO_MEM)    return send_err_json(req, 409, "image store full");
    if (err != ESP_OK)            return send_err_json(req, 500, "save failed");
    char resp[64];
    snprintf(resp, sizeof(resp), "{\"status\":\"ok\",\"name\":\"%s\"}", name);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

// GET /api/images/<name> — raw RGB + X-Frame-W/H (same shape as GET /api/frame).
static esp_err_t images_get_handler(httpd_req_t *req)
{
    if (pairing_http_check(req, PL_ROLE_USER) != ESP_OK) return ESP_FAIL;
    char name[16];
    bool is_show = false;
    if (!split_uri(req->uri, name, sizeof(name), &is_show) || is_show) {
        return send_err_json(req, 404, "no such image");
    }
    int w = 0, h = 0;
    uint8_t *buf = NULL;
    size_t len = 0;
    if (image_store_load(name, &w, &h, &buf, &len) != ESP_OK) {
        return send_err_json(req, 404, "no such image");
    }
    char ws[8], hs[8];
    snprintf(ws, sizeof(ws), "%d", w);
    snprintf(hs, sizeof(hs), "%d", h);
    httpd_resp_set_hdr(req, "X-Frame-W", ws);
    httpd_resp_set_hdr(req, "X-Frame-H", hs);
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_send(req, (const char *)buf, len);
    free(buf);
    return ESP_OK;
}

// POST /api/images/<name>/label {"label":"…"} — rename (an empty or null
// label clears it). Served by the POST wildcard below: the httpd handler table
// is nearly full, so no extra registration.
static esp_err_t images_label(httpd_req_t *req, const char *name)
{
    char label[IMAGE_STORE_LABEL_MAX + 1];
    if (!read_label_body(req, label, sizeof(label))) return send_err_json(req, 400, "bad label");
    esp_err_t err = image_store_set_label(name, label);
    if (err == ESP_ERR_NOT_FOUND) return send_err_json(req, 404, "no such image");
    if (err == ESP_ERR_INVALID_ARG) return send_err_json(req, 400, "bad label");
    if (err != ESP_OK) return send_err_json(req, 500, "label write failed");
    ESP_LOGI(TAG, "Labelled %s", name);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

// POST /api/images/<name>/show — copy into the frame store, display, persist
// mode "frame". Identical end state to a client push (POST /api/frame + mode).
// Also serves POST /api/images/<name>/label (see images_label).
static esp_err_t images_show_handler(httpd_req_t *req)
{
    if (pairing_http_check(req, PL_ROLE_CREATOR) != ESP_OK) return ESP_FAIL;
    char name[16];
    bool is_show = false, is_label = false;
    if (!split_uri_ex(req->uri, name, sizeof(name), &is_show, &is_label)) {
        return send_err_json(req, 404, "no such image");
    }
    if (is_label) return images_label(req, name);
    if (!is_show) return send_err_json(req, 404, "no such image");
    int w = 0, h = 0;
    uint8_t *buf = NULL;
    size_t len = 0;
    if (image_store_load(name, &w, &h, &buf, &len) != ESP_OK) {
        return send_err_json(req, 404, "no such image");
    }
    if (w != led_control_get_logical_w() || h != led_control_get_logical_h()) {
        free(buf);
        return send_err_json(req, 400, "geometry mismatch");
    }
    esp_err_t err = frame_store_save(w, h, buf, len);
    free(buf);
    if (err != ESP_OK) return send_err_json(req, 500, "frame write failed");
    /* apply_mode("frame") stops any runtime, persists the mode, displays the
     * frame when the lamp is on, and notifies MQTT + swarm — one call gives
     * the exact push semantics. */
    int mode_err = light_api_apply_mode("frame");
    if (mode_err != 0) return send_err_json(req, 500, "mode apply failed");
    config_store_set_str(CONFIG_KEY_CUR_IMAGE, name);   // so a scene capture can record it
    ESP_LOGI(TAG, "Showing %s", name);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

// DELETE /api/images/<name>
static esp_err_t images_delete_handler(httpd_req_t *req)
{
    if (pairing_http_check(req, PL_ROLE_CREATOR) != ESP_OK) return ESP_FAIL;
    char name[16];
    bool is_show = false;
    if (!split_uri(req->uri, name, sizeof(name), &is_show) || is_show) {
        return send_err_json(req, 404, "no such image");
    }
    esp_err_t err = image_store_delete(name);
    if (err != ESP_OK) return send_err_json(req, 404, "no such image");
    ESP_LOGI(TAG, "Deleted %s", name);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

static void register_or_warn(httpd_handle_t server, const httpd_uri_t *u)
{
    esp_err_t err = httpd_register_uri_handler(server, u);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register %s failed: %s", u->uri, esp_err_to_name(err));
    }
}

esp_err_t image_api_register(httpd_handle_t server)
{
    httpd_uri_t list = {.uri = "/api/images", .method = HTTP_GET, .handler = images_list_handler};
    register_or_warn(server, &list);
    httpd_uri_t save = {.uri = "/api/images", .method = HTTP_POST, .handler = images_save_handler};
    register_or_warn(server, &save);
    httpd_uri_t get_one = {.uri = "/api/images/*", .method = HTTP_GET, .handler = images_get_handler};
    register_or_warn(server, &get_one);
    /* POST wildcard serves the /show and /label sub-resources; split_uri_ex
     * rejects everything else with 404. */
    httpd_uri_t show = {.uri = "/api/images/*", .method = HTTP_POST, .handler = images_show_handler};
    register_or_warn(server, &show);
    httpd_uri_t del = {.uri = "/api/images/*", .method = HTTP_DELETE, .handler = images_delete_handler};
    register_or_warn(server, &del);
    return ESP_OK;
}
