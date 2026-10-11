// Scenes: a named JSON document that captures the lamp's user-facing state and
// can be recalled with one call. Every field is optional; an absent field
// means "leave as is", so partial scenes (just a mood colour, say) work too.
//
//   { "on": true, "mode": "js", "script": "blaze", "params": {"speed": 2},
//     "image": "img-003", "color": [255,140,40], "brightness": 120 }
//
// Why on the lamp: a scene stored here works for every client, for an AI
// agent, for Home Assistant and for the swarm, and it follows the lamp.
//
// Activation order is the point — the caller shouldn't have to know it:
//   script (-> js mode) / image (-> frame mode) / explicit mode   content first
//   params                                                        onto the running script
//   brightness                                                    before power, so the fade-in lands right
//   power                                                         content is ready; power-on's resume is idempotent
//   color                                                         last: a solid paint needs power on; in js/frame it just tints
// Each step goes through the same light_api/js_api helpers the HTTP handlers
// use, so persistence, MQTT and swarm mirroring all happen exactly as if the
// user had clicked through it.
//
// Routes (additive; see docs/protocol.md "Scenes"):
//   GET    /api/scenes                 user     {"scenes":[{"name","scene"}]}
//   GET    /api/scenes/<name>          user     the document
//   PUT    /api/scenes/<name>          creator  save an explicit document
//   DELETE /api/scenes/<name>          creator
//   POST   /api/scenes/<name>/capture  creator  snapshot the CURRENT state under <name>
//   POST   /api/scenes/<name>/activate user     apply it
#include "scene_api.h"
#include "scene_store.h"
#include "light_api.h"
#include "js_api.h"
#include "js_player.h"
#include "led_control.h"
#include "image_store.h"
#include "frame_store.h"
#include "config_store.h"
#include "pairing.h"
#include "cJSON.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "scene_api";

static esp_err_t send_err_json(httpd_req_t *req, int status, const char *msg)
{
    char code[8];
    snprintf(code, sizeof(code), "%d", status);
    httpd_resp_set_status(req, status == 400 ? "400 Bad Request" :
                               status == 404 ? "404 Not Found" :
                               status == 409 ? "409 Conflict" :
                               status == 507 ? "507 Insufficient Storage" :
                               "500 Internal Server Error");
    httpd_resp_set_type(req, "application/json");
    char body[128];
    snprintf(body, sizeof(body), "{\"error\":\"%s\",\"code\":%s}", msg, code);
    httpd_resp_sendstr(req, body);
    return ESP_FAIL;
}

static esp_err_t send_json(httpd_req_t *req, cJSON *obj)
{
    char *out = cJSON_PrintUnformatted(obj);
    if (!out) return send_err_json(req, 500, "OOM");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out);
    free(out);
    return ESP_OK;
}

// "/api/scenes/<name>[/<action>]" -> name, action ("" when none). false if malformed.
static bool split_uri(const char *uri, char *name, size_t ncap, char *action, size_t acap)
{
    const char *base = "/api/scenes/";
    size_t bl = strlen(base);
    if (strncmp(uri, base, bl) != 0) return false;
    const char *p = uri + bl;
    size_t i = 0;
    while (p[i] && p[i] != '/' && p[i] != '?') i++;
    if (i == 0 || i >= ncap) return false;
    memcpy(name, p, i); name[i] = 0;
    action[0] = 0;
    if (p[i] == '/') {
        const char *a = p + i + 1;
        size_t j = 0;
        while (a[j] && a[j] != '/' && a[j] != '?' && j < acap - 1) { action[j] = a[j]; j++; }
        action[j] = 0;
    }
    return scene_store_name_valid(name);
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

// ---------- validation ------------------------------------------------------

// Accepts the document if every KNOWN field has the right shape; unknown
// fields are tolerated (forward compatibility). Returns NULL or an error text.
static const char *validate_scene(const cJSON *doc)
{
    if (!cJSON_IsObject(doc)) return "scene must be a JSON object";
    const cJSON *v;
    if ((v = cJSON_GetObjectItem(doc, "on")) && !cJSON_IsBool(v)) return "on must be a boolean";
    if ((v = cJSON_GetObjectItem(doc, "mode"))) {
        if (!cJSON_IsString(v)) return "mode must be a string";
        const char *m = v->valuestring;
        if (strcmp(m, "api") && strcmp(m, "js") && strcmp(m, "frame")) return "mode must be api, js or frame";
    }
    if ((v = cJSON_GetObjectItem(doc, "script")) && !cJSON_IsString(v)) return "script must be a string";
    if ((v = cJSON_GetObjectItem(doc, "image")) && !cJSON_IsString(v)) return "image must be a string";
    if ((v = cJSON_GetObjectItem(doc, "params")) && !cJSON_IsObject(v)) return "params must be an object";
    if ((v = cJSON_GetObjectItem(doc, "brightness"))) {
        if (!cJSON_IsNumber(v) || v->valuedouble < 0 || v->valuedouble > 255) return "brightness must be 0..255";
    }
    if ((v = cJSON_GetObjectItem(doc, "color"))) {
        if (!cJSON_IsArray(v) || cJSON_GetArraySize(v) != 3) return "color must be [r,g,b]";
        cJSON *c; cJSON_ArrayForEach(c, v) { if (!cJSON_IsNumber(c)) return "color must be [r,g,b]"; }
    }
    return NULL;
}

// ---------- capture ---------------------------------------------------------

static cJSON *capture_scene(void)
{
    cJSON *doc = cJSON_CreateObject();
    cJSON_AddBoolToObject(doc, "on", led_control_is_on());

    char mode[16] = {0};
    light_api_get_persistent_mode(mode, sizeof(mode));
    cJSON_AddStringToObject(doc, "mode", mode);

    if (strcmp(mode, "js") == 0) {
        // What's actually running covers both hardcoded effects and scripts;
        // fall back to the persisted selection when the player is idle (off).
        char script[64] = {0};
        const char *running = js_api_is_running() ? js_api_current_name() : NULL;
        if (running && *running) snprintf(script, sizeof(script), "%s", running);
        else config_get_str_or(CONFIG_KEY_CURRENT_JS, script, sizeof(script), "");
        if (script[0]) {
            cJSON_AddStringToObject(doc, "script", script);
            // Current param values -> {"name": value, ...}
            char *pj = malloc(2048);
            if (pj && js_api_params_json(script, pj, 2048) > 0) {
                cJSON *pr = cJSON_Parse(pj);
                cJSON *items = pr ? cJSON_GetObjectItem(pr, "items") : NULL;
                if (items && cJSON_GetArraySize(items) > 0) {
                    cJSON *params = cJSON_AddObjectToObject(doc, "params");
                    cJSON *it;
                    cJSON_ArrayForEach(it, items) {
                        cJSON *n = cJSON_GetObjectItem(it, "name");
                        cJSON *val = cJSON_GetObjectItem(it, "value");
                        if (cJSON_IsString(n) && cJSON_IsNumber(val))
                            cJSON_AddNumberToObject(params, n->valuestring, val->valuedouble);
                    }
                }
                cJSON_Delete(pr);
            }
            free(pj);
        }
    } else if (strcmp(mode, "frame") == 0) {
        char img[IMAGE_STORE_NAME_CAP + 4] = {0};
        config_get_str_or(CONFIG_KEY_CUR_IMAGE, img, sizeof(img), "");
        if (img[0]) cJSON_AddStringToObject(doc, "image", img);
    }

    uint8_t r, g, b;
    js_player_get_base_color(&r, &g, &b);
    cJSON *col = cJSON_AddArrayToObject(doc, "color");
    cJSON_AddItemToArray(col, cJSON_CreateNumber(r));
    cJSON_AddItemToArray(col, cJSON_CreateNumber(g));
    cJSON_AddItemToArray(col, cJSON_CreateNumber(b));
    cJSON_AddNumberToObject(doc, "brightness", led_control_get_brightness());
    return doc;
}

// ---------- activate --------------------------------------------------------

// Shows a saved image exactly like POST /api/images/<name>/show.
static int show_image(const char *name, const char **why)
{
    int w = 0, h = 0; uint8_t *buf = NULL; size_t len = 0;
    if (image_store_load(name, &w, &h, &buf, &len) != ESP_OK) { *why = "no such image"; return 404; }
    if (w != led_control_get_logical_w() || h != led_control_get_logical_h()) {
        free(buf); *why = "image geometry mismatch"; return 400;
    }
    esp_err_t err = frame_store_save(w, h, buf, len);
    free(buf);
    if (err != ESP_OK) { *why = "frame write failed"; return 500; }
    if (light_api_apply_mode("frame") != 0) { *why = "mode apply failed"; return 500; }
    config_store_set_str(CONFIG_KEY_CUR_IMAGE, name);
    return 0;
}

// Returns 0 on success, else an HTTP status with *why set.
static int apply_scene(const cJSON *doc, const char **why)
{
    const cJSON *script = cJSON_GetObjectItem(doc, "script");
    const cJSON *image  = cJSON_GetObjectItem(doc, "image");
    const cJSON *mode   = cJSON_GetObjectItem(doc, "mode");
    const cJSON *params = cJSON_GetObjectItem(doc, "params");
    const cJSON *bright = cJSON_GetObjectItem(doc, "brightness");
    const cJSON *on     = cJSON_GetObjectItem(doc, "on");
    const cJSON *color  = cJSON_GetObjectItem(doc, "color");

    // 1. Content. A script implies js mode and an image implies frame mode;
    //    an explicit mode only matters when neither is given (e.g. api + color).
    if (cJSON_IsString(script) && script->valuestring[0]) {
        esp_err_t err = js_api_play(script->valuestring, 0 /* default fps */);
        if (err == ESP_ERR_NOT_FOUND) { *why = "no such script"; return 404; }
        if (err != ESP_OK)            { *why = "play failed";    return 500; }
        if (light_api_apply_mode("js") != 0) { *why = "mode apply failed"; return 500; }
        // 2. Params onto the now-running script (partial object is fine).
        if (cJSON_IsObject(params) && cJSON_GetArraySize(params) > 0) {
            char *pj = cJSON_PrintUnformatted(params);
            if (pj) { (void)js_api_apply_params(script->valuestring, pj, strlen(pj)); free(pj); }
        }
    } else if (cJSON_IsString(image) && image->valuestring[0]) {
        int st = show_image(image->valuestring, why);
        if (st) return st;
    } else if (cJSON_IsString(mode)) {
        if (light_api_apply_mode(mode->valuestring) != 0) { *why = "bad mode"; return 400; }
    }

    // 3. Brightness before power so the fade-in lands at the target level.
    if (cJSON_IsNumber(bright)) {
        double v = bright->valuedouble;
        if (v < 0) v = 0; else if (v > 255) v = 255;
        light_api_apply_brightness((uint8_t)v);
    }

    // 4. Power. Content is ready; power-on's resume is idempotent.
    if (cJSON_IsBool(on)) light_api_apply_power(cJSON_IsTrue(on));

    // 5. Colour last: a solid paint needs power on; in js/frame it sets the
    //    base colour the content tints from.
    if (cJSON_IsArray(color) && cJSON_GetArraySize(color) == 3) {
        int c[3] = {0, 0, 0};
        for (int i = 0; i < 3; i++) {
            cJSON *n = cJSON_GetArrayItem(color, i);
            double v = cJSON_IsNumber(n) ? n->valuedouble : 0;
            c[i] = v < 0 ? 0 : v > 255 ? 255 : (int)v;
        }
        light_api_apply_color_solid((uint8_t)c[0], (uint8_t)c[1], (uint8_t)c[2]);
    }
    return 0;
}

// ---------- handlers --------------------------------------------------------

static void list_cb(const char *name, void *arg)
{
    cJSON *arr = (cJSON *)arg;
    char *json = NULL;
    if (scene_store_read(name, &json, NULL) != ESP_OK) return;
    cJSON *doc = cJSON_Parse(json);
    free(json);
    if (!doc) return;   // corrupt file: skip rather than fail the list
    cJSON *entry = cJSON_CreateObject();
    cJSON_AddStringToObject(entry, "name", name);
    cJSON_AddItemToObject(entry, "scene", doc);
    cJSON_AddItemToArray(arr, entry);
}

// GET /api/scenes
static esp_err_t scenes_list_handler(httpd_req_t *req)
{
    if (pairing_http_check(req, PL_ROLE_USER) != ESP_OK) return ESP_FAIL;
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "scenes");
    scene_store_list(list_cb, arr);
    esp_err_t r = send_json(req, root);
    cJSON_Delete(root);
    return r;
}

// GET /api/scenes/<name>
static esp_err_t scene_get_handler(httpd_req_t *req)
{
    if (pairing_http_check(req, PL_ROLE_USER) != ESP_OK) return ESP_FAIL;
    char name[SCENE_MAX_NAME + 1], action[16];
    if (!split_uri(req->uri, name, sizeof(name), action, sizeof(action)) || action[0])
        return send_err_json(req, 404, "no such scene");
    char *json = NULL;
    if (scene_store_read(name, &json, NULL) != ESP_OK) return send_err_json(req, 404, "no such scene");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

// PUT /api/scenes/<name>  body: the scene document
static esp_err_t scene_put_handler(httpd_req_t *req)
{
    if (pairing_http_check(req, PL_ROLE_CREATOR) != ESP_OK) return ESP_FAIL;
    char name[SCENE_MAX_NAME + 1], action[16];
    if (!split_uri(req->uri, name, sizeof(name), action, sizeof(action)) || action[0])
        return send_err_json(req, 400, "bad scene name (1-32 of A-Z a-z 0-9 _ -)");
    char *body = recv_body(req, SCENE_MAX_BYTES);
    if (!body) return send_err_json(req, 400, "body missing or too large");
    cJSON *doc = cJSON_Parse(body);
    free(body);
    if (!doc) return send_err_json(req, 400, "invalid JSON");
    const char *bad = validate_scene(doc);
    if (bad) { cJSON_Delete(doc); return send_err_json(req, 400, bad); }
    char *norm = cJSON_PrintUnformatted(doc);   // store normalised
    cJSON_Delete(doc);
    if (!norm) return send_err_json(req, 500, "OOM");
    esp_err_t err = scene_store_write(name, norm, strlen(norm));
    free(norm);
    if (err == ESP_ERR_NO_MEM) return send_err_json(req, 507, "scene limit reached");
    if (err != ESP_OK)         return send_err_json(req, 500, "write failed");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

// DELETE /api/scenes/<name>
static esp_err_t scene_delete_handler(httpd_req_t *req)
{
    if (pairing_http_check(req, PL_ROLE_CREATOR) != ESP_OK) return ESP_FAIL;
    char name[SCENE_MAX_NAME + 1], action[16];
    if (!split_uri(req->uri, name, sizeof(name), action, sizeof(action)) || action[0])
        return send_err_json(req, 404, "no such scene");
    if (scene_store_delete(name) != ESP_OK) return send_err_json(req, 404, "no such scene");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

// POST /api/scenes/<name>/capture | /activate
static esp_err_t scene_post_handler(httpd_req_t *req)
{
    char name[SCENE_MAX_NAME + 1], action[16];
    if (!split_uri(req->uri, name, sizeof(name), action, sizeof(action)))
        return send_err_json(req, 404, "no such scene");

    if (strcmp(action, "capture") == 0) {
        if (pairing_http_check(req, PL_ROLE_CREATOR) != ESP_OK) return ESP_FAIL;
        cJSON *doc = capture_scene();
        char *json = cJSON_PrintUnformatted(doc);
        if (!json) { cJSON_Delete(doc); return send_err_json(req, 500, "OOM"); }
        esp_err_t err = scene_store_write(name, json, strlen(json));
        free(json);
        if (err == ESP_ERR_NO_MEM) { cJSON_Delete(doc); return send_err_json(req, 507, "scene limit reached"); }
        if (err != ESP_OK)         { cJSON_Delete(doc); return send_err_json(req, 500, "write failed"); }
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "status", "ok");
        cJSON_AddStringToObject(root, "name", name);
        cJSON_AddItemToObject(root, "scene", doc);
        esp_err_t r = send_json(req, root);
        cJSON_Delete(root);
        ESP_LOGI(TAG, "captured scene '%s'", name);
        return r;
    }

    if (strcmp(action, "activate") == 0) {
        if (pairing_http_check(req, PL_ROLE_USER) != ESP_OK) return ESP_FAIL;
        char *json = NULL;
        if (scene_store_read(name, &json, NULL) != ESP_OK) return send_err_json(req, 404, "no such scene");
        cJSON *doc = cJSON_Parse(json);
        free(json);
        if (!doc) return send_err_json(req, 500, "scene is corrupt");
        const char *why = "failed";
        int st = apply_scene(doc, &why);
        cJSON_Delete(doc);
        if (st) return send_err_json(req, st, why);
        ESP_LOGI(TAG, "activated scene '%s'", name);
        char body[80];
        snprintf(body, sizeof(body), "{\"status\":\"ok\",\"scene\":\"%s\"}", name);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, body);
        return ESP_OK;
    }
    return send_err_json(req, 404, "unknown action (capture|activate)");
}

esp_err_t scene_api_register(httpd_handle_t server)
{
    httpd_uri_t list = { .uri = "/api/scenes",   .method = HTTP_GET,    .handler = scenes_list_handler };
    httpd_uri_t get  = { .uri = "/api/scenes/*", .method = HTTP_GET,    .handler = scene_get_handler };
    httpd_uri_t put  = { .uri = "/api/scenes/*", .method = HTTP_PUT,    .handler = scene_put_handler };
    httpd_uri_t del  = { .uri = "/api/scenes/*", .method = HTTP_DELETE, .handler = scene_delete_handler };
    httpd_uri_t post = { .uri = "/api/scenes/*", .method = HTTP_POST,   .handler = scene_post_handler };
    httpd_register_uri_handler(server, &list);
    httpd_register_uri_handler(server, &get);
    httpd_register_uri_handler(server, &put);
    httpd_register_uri_handler(server, &del);
    httpd_register_uri_handler(server, &post);
    return ESP_OK;
}
