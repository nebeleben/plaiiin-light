#include "image_api.h"
#include "image_store.h"
#include "frame_store.h"
#include "led_control.h"
#include "light_api.h"
#include "pairing.h"

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

/** Extract the image name from "/api/images/<name>" or ".../<name>/show".
 *  Same shape as js_api's split_uri. Returns false on a malformed URI. */
static bool split_uri(const char *uri, char *out, size_t out_cap, bool *is_show)
{
    *is_show = false;
    if (out_cap == 0) return false;
    out[0] = '\0';
    const char *prefix = "/api/images/";
    if (strncmp(uri, prefix, strlen(prefix)) != 0) return false;
    const char *name = uri + strlen(prefix);
    if (*name == 0) return false;
    size_t len = strlen(name);
    const char *suffix = "/show";
    size_t slen = strlen(suffix);
    if (len > slen && strcmp(name + len - slen, suffix) == 0) {
        *is_show = true;
        len -= slen;
    }
    if (len + 1 > out_cap) return false;
    memcpy(out, name, len);
    out[len] = '\0';
    return out[0] != '\0';
}

// GET /api/images -> {"status":"ok","images":[{name,w,h}...],"count":N,"max":30}
static esp_err_t images_list_handler(httpd_req_t *req)
{
    if (pairing_http_check(req, PL_ROLE_USER) != ESP_OK) return ESP_FAIL;
    image_store_entry_t entries[IMAGE_STORE_MAX];
    int count = image_store_list(entries, IMAGE_STORE_MAX);

    /* 30 entries × ~44 B each + wrapper — 2 KB is deliberate headroom (the
     * 1 KB js_api list buffer is a known ceiling; don't repeat it here). */
    char resp[2048];
    int n = snprintf(resp, sizeof(resp), "{\"status\":\"ok\",\"images\":[");
    for (int i = 0; i < count; i++) {
        n += snprintf(resp + n, sizeof(resp) - n,
                      "%s{\"name\":\"%s\",\"w\":%d,\"h\":%d}",
                      i ? "," : "", entries[i].name, entries[i].w, entries[i].h);
        if ((size_t)n >= sizeof(resp) - 64) break;
    }
    n += snprintf(resp + n, sizeof(resp) - n, "],\"count\":%d,\"max\":%d}",
                  count, IMAGE_STORE_MAX);
    if (n < 0 || (size_t)n >= sizeof(resp)) return send_err_json(req, 500, "list too large");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

// POST /api/images — snapshot the current stored frame into a new slot.
static esp_err_t images_save_handler(httpd_req_t *req)
{
    if (pairing_http_check(req, PL_ROLE_CREATOR) != ESP_OK) return ESP_FAIL;
    char name[IMAGE_STORE_NAME_CAP];
    esp_err_t err = image_store_save_current(name, sizeof(name));
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

// POST /api/images/<name>/show — copy into the frame store, display, persist
// mode "frame". Identical end state to a client push (POST /api/frame + mode).
static esp_err_t images_show_handler(httpd_req_t *req)
{
    if (pairing_http_check(req, PL_ROLE_CREATOR) != ESP_OK) return ESP_FAIL;
    char name[16];
    bool is_show = false;
    if (!split_uri(req->uri, name, sizeof(name), &is_show) || !is_show) {
        return send_err_json(req, 404, "no such image");
    }
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
    /* POST wildcard serves only the /show sub-resource; split_uri rejects
     * everything else with 404. */
    httpd_uri_t show = {.uri = "/api/images/*", .method = HTTP_POST, .handler = images_show_handler};
    register_or_warn(server, &show);
    httpd_uri_t del = {.uri = "/api/images/*", .method = HTTP_DELETE, .handler = images_delete_handler};
    register_or_warn(server, &del);
    return ESP_OK;
}
