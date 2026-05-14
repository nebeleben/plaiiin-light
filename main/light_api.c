#include "light_api.h"
#include "led_control.h"
#include "ws_server.h"
#include "config_store.h"
#include "js_player.h"
#include "js_storage.h"
#include "js_api.h"
#include "pairing.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "light_api";

// --- baseColor NVS-write debounce ------------------------------------------
//
// The macOS color picker streams updates; persisting on every one would block
// the LED task on flash commits (visible as flicker). Instead we cache the
// latest packed color in a static, restart a one-shot timer on each update,
// and only commit when the user has been quiet for ~DEBOUNCE_MS.
#define BASECOLOR_DEBOUNCE_MS 1000
static esp_timer_handle_t s_basecolor_timer = NULL;
static int32_t s_pending_basecolor = -1;
static int32_t s_last_persisted_basecolor = -1;

static void basecolor_commit_cb(void *arg)
{
    (void)arg;
    int32_t v = s_pending_basecolor;
    if (v >= 0 && v != s_last_persisted_basecolor) {
        config_store_set_i32(CONFIG_KEY_BASE_COLOR, v);
        s_last_persisted_basecolor = v;
    }
}

static void schedule_basecolor_persist(int32_t packed)
{
    s_pending_basecolor = packed;
    if (!s_basecolor_timer) {
        const esp_timer_create_args_t args = {
            .callback = basecolor_commit_cb,
            .name = "basecolor_persist"
        };
        if (esp_timer_create(&args, &s_basecolor_timer) != ESP_OK) return;
    }
    esp_timer_stop(s_basecolor_timer);   // OK if not running
    esp_timer_start_once(s_basecolor_timer, BASECOLOR_DEBOUNCE_MS * 1000);
}

/// Read the persistent lamp mode ("api" or "js"). "stream" lives only on the
/// websocket and is not persisted; if the websocket is in stream mode that
/// takes precedence (returned to clients via /api/state).
static void get_persistent_mode(char *out, size_t max_len)
{
    config_get_str_or(CONFIG_KEY_LAMP_MODE, out, max_len, "api");
}

/// Start playback of the persisted current_js. Returns ESP_OK if anything
/// got loaded; ESP_ERR_NOT_FOUND if no script is selected or it's missing.
static esp_err_t start_current_js(void)
{
    char name[64] = {0};
    config_get_str_or(CONFIG_KEY_CURRENT_JS, name, sizeof(name), "");
    if (!name[0]) return ESP_ERR_NOT_FOUND;
    char *src = NULL;
    size_t len = 0;
    esp_err_t err = js_storage_read(name, &src, &len);
    if (err != ESP_OK) return err;
    err = js_player_start(src, JS_DEFAULT_FPS);
    free(src);
    if (err == ESP_OK) js_player_set_current_name(name);
    return err;
}

// --- Transport-agnostic helpers ---------------------------------------------
//
// These do the actual work and are called from both the HTTP handlers in this
// file and the BLE GATT layer in bt_service.c. Keeping them here means there's
// exactly one place that decides what "set color" or "switch to js mode" mean.

void light_api_apply_power(bool on)
{
    char mode[16] = {0};
    get_persistent_mode(mode, sizeof(mode));
    if (strcmp(mode, "js") == 0) {
        if (on) {
            led_control_power(true);
            (void)start_current_js();
        } else {
            js_player_stop();
            led_control_power(false);
        }
    } else {
        led_control_power(on);
    }
}

void light_api_apply_color_solid(uint8_t r, uint8_t g, uint8_t b)
{
    char mode[16] = {0};
    get_persistent_mode(mode, sizeof(mode));
    bool js_mode = (strcmp(mode, "js") == 0);
    if (!js_mode) {
        int n = led_control_get_count();
        led_color_t *colors = calloc(n, sizeof(led_color_t));
        if (colors) {
            for (int i = 0; i < n; i++) {
                colors[i].r = r; colors[i].g = g; colors[i].b = b;
            }
            led_control_set_all(colors, n);
            free(colors);
        }
    }
    js_player_set_base_color(r, g, b);
    int32_t packed = ((int32_t)r << 16) | ((int32_t)g << 8) | (int32_t)b;
    schedule_basecolor_persist(packed);
}

int light_api_apply_mode(const char *mode)
{
    if (!mode) return -1;
    if (strcmp(mode, "api") == 0) {
        ws_server_set_mode(LAMP_MODE_API);
        js_player_stop();
        js_player_set_current_name(NULL);
        config_store_set_str(CONFIG_KEY_LAMP_MODE, "api");
        return 0;
    }
    if (strcmp(mode, "js") == 0) {
        ws_server_set_mode(LAMP_MODE_API);
        config_store_set_str(CONFIG_KEY_LAMP_MODE, "js");
        if (led_control_is_on()) (void)start_current_js();
        return 0;
    }
    if (strcmp(mode, "stream") == 0) {
        js_player_stop();
        js_player_set_current_name(NULL);
        ws_server_set_mode(LAMP_MODE_STREAM);
        return 0;
    }
    return -1;
}

// POST /api/power  body: {"on":true} or {"on":false}
static esp_err_t power_handler(httpd_req_t *req)
{
    if (pairing_http_check(req) != ESP_OK) return ESP_FAIL;
    char buf[64] = {0};
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    bool on = (strstr(buf, "true") != NULL);
    light_api_apply_power(on);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, on ? "{\"status\":\"on\"}" : "{\"status\":\"off\"}");
    return ESP_OK;
}

/**
 * POST /api/color
 * Body: {"colors":[[255,0,0],[0,255,0],[0,0,255],...]}
 * Simple JSON parser — expects [[r,g,b],...] within "colors"
 */
static esp_err_t color_handler(httpd_req_t *req)
{
    if (pairing_http_check(req) != ESP_OK) return ESP_FAIL;
    int content_len = req->content_len;
    if (content_len <= 0 || content_len > 8192) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid content length");
        return ESP_FAIL;
    }

    char *buf = malloc(content_len + 1);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    int received = 0;
    while (received < content_len) {
        int ret = httpd_req_recv(req, buf + received, content_len - received);
        if (ret <= 0) {
            free(buf);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Receive error");
            return ESP_FAIL;
        }
        received += ret;
    }
    buf[content_len] = '\0';

    // Find the array start after "colors"
    char *p = strstr(buf, "colors");
    if (!p) { p = buf; }
    p = strchr(p, '[');
    if (!p) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing colors array");
        return ESP_FAIL;
    }
    p++; // skip outer [

    int led_count = led_control_get_count();
    led_color_t *colors = calloc(led_count, sizeof(led_color_t));
    if (!colors) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    int idx = 0;
    while (*p && idx < led_count) {
        // Find next [r,g,b]
        char *bracket = strchr(p, '[');
        if (!bracket) break;
        p = bracket + 1;

        int r = 0, g = 0, b = 0;
        r = (int)strtol(p, &p, 10);
        if (*p == ',') p++;
        g = (int)strtol(p, &p, 10);
        if (*p == ',') p++;
        b = (int)strtol(p, &p, 10);

        // Guardrail: clamp to valid range instead of silently wrapping.
        if (r < 0) r = 0; else if (r > 255) r = 255;
        if (g < 0) g = 0; else if (g > 255) g = 255;
        if (b < 0) b = 0; else if (b > 255) b = 255;
        colors[idx].r = (uint8_t)r;
        colors[idx].g = (uint8_t)g;
        colors[idx].b = (uint8_t)b;
        idx++;

        // Skip past ]
        char *end = strchr(p, ']');
        if (end) p = end + 1;
    }

    if (idx != led_count) {
        ESP_LOGW(TAG, "color: received %d pixels, expected %d", idx, led_count);
    }

    // In js mode the player owns the framebuffer — writing a solid color here
    // would race against the next render() output and look like a flicker.
    // We still update baseColor so the running script picks up the new tint
    // on its very next frame.
    char mode[16] = {0};
    get_persistent_mode(mode, sizeof(mode));
    bool js_mode = (strcmp(mode, "js") == 0);
    if (!js_mode) {
        led_control_set_all(colors, idx);
    }

    // Persist the first received color as the "base color" — that's what HA
    // and similar integrations send (a single uniform color), and JS scripts
    // get it back as the 4th render() arg. NVS commits are slow (~tens of ms)
    // and were causing visible flicker when the user scrubbed the macOS color
    // picker (which fires onChange on every cursor delta), so we debounce:
    // the timer is restarted on every color update and only fires when the
    // user has stopped moving for ~1 s.
    if (idx > 0) {
        uint8_t r = colors[0].r, g = colors[0].g, b = colors[0].b;
        js_player_set_base_color(r, g, b);
        int32_t packed = ((int32_t)r << 16) | ((int32_t)g << 8) | (int32_t)b;
        schedule_basecolor_persist(packed);
    }

    free(colors);
    free(buf);

    ESP_LOGI(TAG, "Set %d LED colors%s", idx, js_mode ? " (baseColor only — js mode)" : "");

    httpd_resp_set_type(req, "application/json");
    char resp[64];
    snprintf(resp, sizeof(resp), "{\"status\":\"ok\",\"count\":%d}", idx);
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

// GET /api/brightness -> {"brightness":255}
static esp_err_t brightness_get_handler(httpd_req_t *req)
{
    if (pairing_http_check(req) != ESP_OK) return ESP_FAIL;
    char resp[32];
    snprintf(resp, sizeof(resp), "{\"brightness\":%d}", led_control_get_brightness());
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

// POST /api/brightness  body: {"brightness":128}
static esp_err_t brightness_set_handler(httpd_req_t *req)
{
    if (pairing_http_check(req) != ESP_OK) return ESP_FAIL;
    char buf[64] = {0};
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }

    // Parse brightness value
    char *p = strstr(buf, "brightness");
    if (p) p = strchr(p, ':');
    if (p) p++;
    int val = p ? atoi(p) : -1;
    if (val < 0) val = 0;
    if (val > 255) val = 255;

    led_control_set_brightness((uint8_t)val);

    char resp[32];
    snprintf(resp, sizeof(resp), "{\"brightness\":%d}", val);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

// GET /api/limits -> {"maxBrightness":N,"maxCurrentMa":N}
static esp_err_t limits_get_handler(httpd_req_t *req)
{
    if (pairing_http_check(req) != ESP_OK) return ESP_FAIL;
    char resp[96];
    snprintf(resp, sizeof(resp),
             "{\"maxBrightness\":%u,\"maxCurrentMa\":%lu}",
             led_control_get_max_brightness(),
             (unsigned long)led_control_get_max_current_ma());
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

// POST /api/limits {"maxBrightness":N,"maxCurrentMa":N}
static esp_err_t limits_set_handler(httpd_req_t *req)
{
    if (pairing_http_check(req) != ESP_OK) return ESP_FAIL;
    char buf[128] = {0};
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    char *p;
    if ((p = strstr(buf, "maxBrightness")) != NULL) {
        p = strchr(p, ':');
        if (p) {
            int v = atoi(p + 1);
            if (v < 0) v = 0; else if (v > 255) v = 255;
            led_control_set_max_brightness((uint8_t)v);
        }
    }
    if ((p = strstr(buf, "maxCurrentMa")) != NULL) {
        p = strchr(p, ':');
        if (p) {
            long v = atol(p + 1);
            if (v < 0) v = 0;
            led_control_set_max_current_ma((uint32_t)v);
        }
    }
    return limits_get_handler(req);
}

// GET /api/grid -> {"pixelGroupW":N,"pixelGroupH":N,"physicalW":N,"physicalH":N,"logicalW":N,"logicalH":N}
static esp_err_t grid_get_handler(httpd_req_t *req)
{
    if (pairing_http_check(req) != ESP_OK) return ESP_FAIL;
    char resp[192];
    snprintf(resp, sizeof(resp),
             "{\"pixelGroupW\":%d,\"pixelGroupH\":%d,"
             "\"physicalW\":%d,\"physicalH\":%d,"
             "\"logicalW\":%d,\"logicalH\":%d}",
             led_control_get_pixel_group_w(),
             led_control_get_pixel_group_h(),
             led_control_get_physical_w(),
             led_control_get_physical_h(),
             led_control_get_logical_w(),
             led_control_get_logical_h());
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

// POST /api/grid {"pixelGroupW":N,"pixelGroupH":N}
static esp_err_t grid_set_handler(httpd_req_t *req)
{
    if (pairing_http_check(req) != ESP_OK) return ESP_FAIL;
    char buf[128] = {0};
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    char *p;
    int gw = led_control_get_pixel_group_w();
    int gh = led_control_get_pixel_group_h();
    bool changed = false;
    if ((p = strstr(buf, "pixelGroupW")) != NULL) {
        p = strchr(p, ':');
        if (p) { gw = atoi(p + 1); if (gw < 1) gw = 1; changed = true; }
    }
    if ((p = strstr(buf, "pixelGroupH")) != NULL) {
        p = strchr(p, ':');
        if (p) { gh = atoi(p + 1); if (gh < 1) gh = 1; changed = true; }
    }
    if (changed) led_control_set_pixel_group(gw, gh);
    return grid_get_handler(req);
}

// GET /api/orientation -> {"rotation":N,"origin":N,"serpentine":bool,"serpentineAxis":N}
static esp_err_t orientation_get_handler(httpd_req_t *req)
{
    if (pairing_http_check(req) != ESP_OK) return ESP_FAIL;
    char resp[128];
    snprintf(resp, sizeof(resp),
             "{\"rotation\":%d,\"origin\":%d,\"serpentine\":%s,\"serpentineAxis\":%d}",
             led_control_get_rotation(),
             led_control_get_origin(),
             led_control_get_serpentine() ? "true" : "false",
             led_control_get_serp_axis());
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

// POST /api/orientation  body: any subset of {rotation,origin,serpentine,serpentineAxis}.
// rotation: 0|90|180|270  origin: 0..3 (TL/TR/BL/BR)  serpentineAxis: 0|1
static esp_err_t orientation_set_handler(httpd_req_t *req)
{
    if (pairing_http_check(req) != ESP_OK) return ESP_FAIL;
    char buf[160] = {0};
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    int rotation  = led_control_get_rotation();
    int origin    = led_control_get_origin();
    bool serp     = led_control_get_serpentine();
    int serp_axis = led_control_get_serp_axis();
    char *p;
    if ((p = strstr(buf, "rotation")) != NULL && (p = strchr(p, ':')) != NULL) {
        int r = atoi(p + 1);
        if (r != 0 && r != 90 && r != 180 && r != 270) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "rotation must be 0/90/180/270");
            return ESP_FAIL;
        }
        rotation = r;
    }
    if ((p = strstr(buf, "origin")) != NULL && (p = strchr(p, ':')) != NULL) {
        int o = atoi(p + 1);
        if (o < 0 || o > 3) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "origin must be 0..3");
            return ESP_FAIL;
        }
        origin = o;
    }
    // Match "serpentine" first; "serpentineAxis" is matched separately to avoid
    // "serpentine" picking up the value of "serpentineAxis".
    if ((p = strstr(buf, "serpentineAxis")) != NULL && (p = strchr(p, ':')) != NULL) {
        int a = atoi(p + 1);
        if (a != 0 && a != 1) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "serpentineAxis must be 0 or 1");
            return ESP_FAIL;
        }
        serp_axis = a;
    }
    if ((p = strstr(buf, "\"serpentine\"")) != NULL && (p = strchr(p, ':')) != NULL) {
        // Accept true/false or 0/1.
        while (*++p == ' ') ;
        if (!strncmp(p, "true", 4))       serp = true;
        else if (!strncmp(p, "false", 5)) serp = false;
        else                              serp = atoi(p) != 0;
    }
    led_control_set_orientation(rotation, origin, serp, serp_axis);
    return orientation_get_handler(req);
}

// GET /api/state ->
//   {"on":bool,"color":[r,g,b],"mode":"api"|"js"|"stream",
//    "brightness":int,"current":"name"|null,"fps":float}
//
// "mode" reflects the *effective* mode: stream when WS is active, otherwise
// the persisted lamp mode. "current" is the name of the currently-loaded JS
// script (or null). "fps" (Phase 22) is the rolling-5s rendered-FPS of the
// JS player — 0 when no script is producing frames.
static esp_err_t state_get_handler(httpd_req_t *req)
{
    if (pairing_http_check(req) != ESP_OK) return ESP_FAIL;
    // `color` was historically the *last painted* color. In js / stream modes
    // that flips every frame, so client-side color pickers couldn't sync
    // sensibly — the picker chased the latest frame's first pixel. Return
    // baseColor instead: the user's *intent* (last value sent through
    // /api/color), which stays stable across modes. In api mode baseColor ==
    // last painted, so existing api-mode clients see no behavioral change.
    uint8_t br = 0, bg = 0, bb = 0;
    js_player_get_base_color(&br, &bg, &bb);
    bool on = led_control_is_on();
    char persistent[16] = {0};
    get_persistent_mode(persistent, sizeof(persistent));
    const char *mode = (ws_server_get_mode() == LAMP_MODE_STREAM) ? "stream" : persistent;
    uint8_t brightness = led_control_get_brightness();
    const char *current = js_player_current_name();
    float fps = js_player_get_fps();
    // One-decimal formatting keeps the payload compact and matches what UIs
    // want to display anyway. snprintf with %.1f rounds for us.

    char resp[256];
    if (current) {
        snprintf(resp, sizeof(resp),
                 "{\"on\":%s,\"color\":[%u,%u,%u],\"mode\":\"%s\",\"brightness\":%u,\"current\":\"%s\",\"fps\":%.1f}",
                 on ? "true" : "false", br, bg, bb, mode, brightness, current, fps);
    } else {
        snprintf(resp, sizeof(resp),
                 "{\"on\":%s,\"color\":[%u,%u,%u],\"mode\":\"%s\",\"brightness\":%u,\"current\":null,\"fps\":%.1f}",
                 on ? "true" : "false", br, bg, bb, mode, brightness, fps);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

// GET /api/base_color -> {"color":[r,g,b]}
//
// The user-set color (4th arg to render() in js mode; same as the LED color
// in api mode). Distinct from the painted-frame color, which is what state.color
// returned before 1.8.6 and is no longer surfaced — the painted color flips
// per frame in js / stream mode and was never useful for clients.
static esp_err_t base_color_get_handler(httpd_req_t *req)
{
    if (pairing_http_check(req) != ESP_OK) return ESP_FAIL;
    uint8_t r = 0, g = 0, b = 0;
    js_player_get_base_color(&r, &g, &b);
    char resp[40];
    snprintf(resp, sizeof(resp), "{\"color\":[%u,%u,%u]}", r, g, b);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

// GET /api/mode -> {"mode":"api"|"js"|"stream","persistent":"api"|"js","current":"name"|null}
// PUT /api/mode  body: {"mode":"api"|"js"}
static esp_err_t mode_get_handler(httpd_req_t *req)
{
    if (pairing_http_check(req) != ESP_OK) return ESP_FAIL;
    char persistent[16] = {0};
    get_persistent_mode(persistent, sizeof(persistent));
    const char *effective = (ws_server_get_mode() == LAMP_MODE_STREAM) ? "stream" : persistent;
    const char *current = js_player_current_name();
    char resp[160];
    if (current) {
        snprintf(resp, sizeof(resp),
                 "{\"mode\":\"%s\",\"persistent\":\"%s\",\"current\":\"%s\"}",
                 effective, persistent, current);
    } else {
        snprintf(resp, sizeof(resp),
                 "{\"mode\":\"%s\",\"persistent\":\"%s\",\"current\":null}",
                 effective, persistent);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static esp_err_t mode_set_handler(httpd_req_t *req)
{
    if (pairing_http_check(req) != ESP_OK) return ESP_FAIL;
    char buf[96] = {0};
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body");
        return ESP_FAIL;
    }
    // Tiny scanner — accepts {"mode":"api"} or {"mode":"js"}. "stream" is
    // intentionally NOT settable here: it's a websocket-driven volatile state.
    const char *p = strstr(buf, "\"mode\"");
    if (!p) p = strstr(buf, "mode");
    if (!p) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "mode missing"); return ESP_FAIL; }
    p = strchr(p, ':');
    if (!p) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "mode missing"); return ESP_FAIL; }
    p = strchr(p, '"');
    if (!p) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "mode missing"); return ESP_FAIL; }
    p++;
    const char *end = strchr(p, '"');
    if (!end) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "mode missing"); return ESP_FAIL; }
    char mode[16] = {0};
    size_t n = end - p;
    if (n >= sizeof(mode)) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "mode too long"); return ESP_FAIL; }
    memcpy(mode, p, n);

    if (light_api_apply_mode(mode) != 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "mode must be 'api', 'js', or 'stream'");
        return ESP_FAIL;
    }
    return mode_get_handler(req);
}

// GET /api/bt -> {"policy":"auto"|"always"|"never","running":bool}
// PUT /api/bt body: {"policy":"auto"} — change takes effect on next boot.
static esp_err_t bt_get_handler(httpd_req_t *req);
static esp_err_t bt_set_handler(httpd_req_t *req);

esp_err_t light_api_register(httpd_handle_t server)
{
    httpd_uri_t power = {
        .uri = "/api/power",
        .method = HTTP_POST,
        .handler = power_handler
    };
    httpd_register_uri_handler(server, &power);

    httpd_uri_t color = {
        .uri = "/api/color",
        .method = HTTP_POST,
        .handler = color_handler
    };
    httpd_register_uri_handler(server, &color);

    httpd_uri_t brightness_get = {
        .uri = "/api/brightness",
        .method = HTTP_GET,
        .handler = brightness_get_handler
    };
    httpd_register_uri_handler(server, &brightness_get);

    httpd_uri_t brightness_set = {
        .uri = "/api/brightness",
        .method = HTTP_POST,
        .handler = brightness_set_handler
    };
    httpd_register_uri_handler(server, &brightness_set);

    httpd_uri_t state_get = {
        .uri = "/api/state",
        .method = HTTP_GET,
        .handler = state_get_handler
    };
    httpd_register_uri_handler(server, &state_get);

    httpd_uri_t base_color_get = {
        .uri = "/api/base_color",
        .method = HTTP_GET,
        .handler = base_color_get_handler
    };
    httpd_register_uri_handler(server, &base_color_get);

    httpd_uri_t limits_get = {
        .uri = "/api/limits",
        .method = HTTP_GET,
        .handler = limits_get_handler
    };
    httpd_register_uri_handler(server, &limits_get);

    httpd_uri_t limits_set = {
        .uri = "/api/limits",
        .method = HTTP_POST,
        .handler = limits_set_handler
    };
    httpd_register_uri_handler(server, &limits_set);

    httpd_uri_t grid_get = {
        .uri = "/api/grid",
        .method = HTTP_GET,
        .handler = grid_get_handler
    };
    httpd_register_uri_handler(server, &grid_get);

    httpd_uri_t grid_set = {
        .uri = "/api/grid",
        .method = HTTP_POST,
        .handler = grid_set_handler
    };
    httpd_register_uri_handler(server, &grid_set);

    httpd_uri_t orientation_get = {
        .uri = "/api/orientation",
        .method = HTTP_GET,
        .handler = orientation_get_handler
    };
    httpd_register_uri_handler(server, &orientation_get);

    httpd_uri_t orientation_set = {
        .uri = "/api/orientation",
        .method = HTTP_POST,
        .handler = orientation_set_handler
    };
    httpd_register_uri_handler(server, &orientation_set);

    httpd_uri_t mode_get = {
        .uri = "/api/mode",
        .method = HTTP_GET,
        .handler = mode_get_handler
    };
    httpd_register_uri_handler(server, &mode_get);

    httpd_uri_t mode_set_put = {
        .uri = "/api/mode",
        .method = HTTP_PUT,
        .handler = mode_set_handler
    };
    httpd_register_uri_handler(server, &mode_set_put);

    // Keep POST for backwards compatibility with the older clients that
    // pre-date the PUT split.
    httpd_uri_t mode_set_post = {
        .uri = "/api/mode",
        .method = HTTP_POST,
        .handler = mode_set_handler
    };
    httpd_register_uri_handler(server, &mode_set_post);

    httpd_uri_t bt_get = {.uri = "/api/bt", .method = HTTP_GET, .handler = bt_get_handler};
    httpd_register_uri_handler(server, &bt_get);
    httpd_uri_t bt_put = {.uri = "/api/bt", .method = HTTP_PUT, .handler = bt_set_handler};
    httpd_register_uri_handler(server, &bt_put);

    return ESP_OK;
}

static esp_err_t bt_get_handler(httpd_req_t *req)
{
    if (pairing_http_check(req) != ESP_OK) return ESP_FAIL;
    char policy[16] = {0};
    config_get_str_or(CONFIG_KEY_BT_ENABLED, policy, sizeof(policy), "auto");
    extern bool bt_service_is_running(void);
    char resp[96];
    snprintf(resp, sizeof(resp), "{\"policy\":\"%s\",\"running\":%s}",
             policy, bt_service_is_running() ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static esp_err_t bt_set_handler(httpd_req_t *req)
{
    if (pairing_http_check(req) != ESP_OK) return ESP_FAIL;
    char buf[96] = {0};
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body");
        return ESP_FAIL;
    }
    const char *p = strstr(buf, "\"policy\"");
    if (!p) p = strstr(buf, "policy");
    if (!p) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "policy missing"); return ESP_FAIL; }
    p = strchr(p, ':'); if (!p) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "policy missing"); return ESP_FAIL; }
    p = strchr(p, '"'); if (!p) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "policy missing"); return ESP_FAIL; }
    p++;
    const char *end = strchr(p, '"'); if (!end) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "policy missing"); return ESP_FAIL; }
    char policy[16] = {0};
    size_t n = end - p; if (n >= sizeof(policy)) n = sizeof(policy) - 1;
    memcpy(policy, p, n);
    if (strcmp(policy, "auto") != 0 && strcmp(policy, "always") != 0 && strcmp(policy, "never") != 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "policy must be auto/always/never");
        return ESP_FAIL;
    }
    config_store_set_str(CONFIG_KEY_BT_ENABLED, policy);
    return bt_get_handler(req);
}
