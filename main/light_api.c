#include "light_api.h"
#include "led_control.h"
#include "ws_server.h"
#include "config_store.h"
#include "js_player.h"
#include "js_storage.h"
#include "js_api.h"
#include "wormhole.h"
#include "frame_store.h"
#include "wifi.h"
#include "pairing.h"
#include "plaiiin_mqtt.h"   // mirror app/BLE changes to MQTT subscribers
#include "swarm_radio.h"    // PlanV3 V2.5 Task 1 — ESP-NOW coexistence spike
#include "swarm.h"          // PlanV3 V2.5 Task 2 — swarm_notify_state() hooks
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_mac.h"
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
    // Every color change (app/HTTP or BLE) settles here ~1s after the last
    // update, so mirror it to MQTT now. Debounced with the NVS write so a
    // color-picker drag doesn't flood the broker with intermediate values.
    mqtt_client_publish_state();
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

// --- on/off NVS persistence ------------------------------------------------
//
// The lamp restores its last on/off state at boot (see main.c). Power toggles
// are user-driven and rare, so unlike baseColor we write through immediately
// rather than debounce. The static guard skips redundant commits (repeated
// "off" presses, periodic HA refreshes) so flash isn't churned needlessly.
static int s_last_persisted_power = -1;
static void persist_power(bool on)
{
    int v = on ? 1 : 0;
    if (v == s_last_persisted_power) return;
    config_store_set_i32(CONFIG_KEY_POWER_ON, v);
    s_last_persisted_power = v;
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
///
/// Phase 35 — delegates to js_api_play() which dispatches hardcoded effects
/// vs PLBC scripts by name. We still keep the "already playing this script"
/// fast-path so a power-on mid-fade-out doesn't bin the running animation's
/// state (jumpy's head position, depthswirl's phase, etc.).
static esp_err_t start_current_js(void)
{
    char name[64] = {0};
    config_get_str_or(CONFIG_KEY_CURRENT_JS, name, sizeof(name), "");
    if (!name[0]) return ESP_ERR_NOT_FOUND;
    const char *cur = js_api_current_name();
    if (js_api_is_running() && cur && strcmp(cur, name) == 0) {
        return ESP_OK;
    }
    return js_api_play(name, JS_DEFAULT_FPS);
}

// --- Transport-agnostic helpers ---------------------------------------------
//
// These do the actual work and are called from both the HTTP handlers in this
// file and the BLE GATT layer in bt_service.c. Keeping them here means there's
// exactly one place that decides what "set color" or "switch to js mode" mean.

// Fires from led_control's fade task once a power-fade completes. On a
// fade-OUT we stop the JS player here — NOT synchronously in apply_power(false)
// — so the live animation keeps running and dimming naturally for the whole
// fade window instead of freezing on a single frame at t=0.
static void on_fade_complete(bool was_off)
{
    if (!was_off) return;
    char mode[16] = {0};
    get_persistent_mode(mode, sizeof(mode));
    if (strcmp(mode, "js") == 0) {
        /* Stops whichever runtime is live — JS player or hardcoded effect. */
        js_api_stop();
    }
}

void light_api_apply_power(bool on)
{
    // Remember the user's choice so a reboot/power-loss restores it (main.c).
    persist_power(on);

    char mode[16] = {0};
    get_persistent_mode(mode, sizeof(mode));
    // While a WS client is streaming, the panel is driven frame-by-frame over
    // the socket — the persisted JS effect must stay suspended. Without this
    // guard, powering on (clients send stream-mode THEN power-on) would
    // re-launch the JS player here and composite it on top of the stream.
    if (strcmp(mode, "js") == 0 && ws_server_get_mode() != LAMP_MODE_STREAM) {
        if (on) {
            led_control_power(true);
            // start_current_js is idempotent — safe to call when the player
            // is already running (e.g. user toggled on mid-fade-out, before
            // the fade-completion callback got a chance to stop it).
            (void)start_current_js();
        } else {
            // JS player keeps running through the fade-out. on_fade_complete()
            // will stop it once led_control's fade task finishes the ramp.
            led_control_power(false);
        }
    } else if (strcmp(mode, "frame") == 0 && ws_server_get_mode() != LAMP_MODE_STREAM) {
        led_control_power(on);
        if (on) (void)frame_store_display();
    } else {
        led_control_power(on);
    }
    mqtt_client_publish_state();
    swarm_notify_state();   // PlanV3 V2.5 — mirror to swarm (no-op if not an active member)
}

// Uniform api-mode paint. The persisting set_all is deliberate — this is the
// one path (besides the raw pixel-array POST) that defines the user's color,
// so last_color must follow it.
static void paint_solid(uint8_t r, uint8_t g, uint8_t b)
{
    int n = led_control_get_count();
    led_color_t *colors = calloc(n, sizeof(led_color_t));
    if (!colors) return;
    for (int i = 0; i < n; i++) {
        colors[i].r = r; colors[i].g = g; colors[i].b = b;
    }
    led_control_set_all(colors, n);
    free(colors);
}

// Repaint the base color after leaving js/stream mode. Stopping the player
// (or closing the stream socket) leaves the last animation frame frozen on
// the panel while /api/state reports mode "api" + baseColor — repainting
// makes the panel show what the API says.
static void repaint_base_color(void)
{
    if (!led_control_is_on()) return;
    uint8_t r = 0, g = 0, b = 0;
    js_player_get_base_color(&r, &g, &b);
    paint_solid(r, g, b);
}

void light_api_apply_color_solid(uint8_t r, uint8_t g, uint8_t b)
{
    char mode[16] = {0};
    get_persistent_mode(mode, sizeof(mode));
    bool content_mode = (strcmp(mode, "js") == 0) || (strcmp(mode, "frame") == 0);
    if (!content_mode) {
        paint_solid(r, g, b);
    }
    js_player_set_base_color(r, g, b);
    int32_t packed = ((int32_t)r << 16) | ((int32_t)g << 8) | (int32_t)b;
    schedule_basecolor_persist(packed);
    swarm_notify_state();   // PlanV3 V2.5 — mirror to swarm (no-op if not an active member)
}

// Brightness is a global LED-driver setting, not a per-mode one, so there's no
// js-mode special-casing — but it still needs one shared entry point so HTTP
// and BLE both persist-free apply it AND notify MQTT subscribers.
void light_api_apply_brightness(uint8_t value)
{
    led_control_set_brightness(value);
    mqtt_client_publish_state();
    swarm_notify_state();   // PlanV3 V2.5 — mirror to swarm (no-op if not an active member)
}

int light_api_apply_mode(const char *mode)
{
    if (!mode) return -1;
    if (strcmp(mode, "api") == 0) {
        ws_server_set_mode(LAMP_MODE_API);
        js_api_stop();   /* stops either runtime; also clears current_name */
        config_store_set_str(CONFIG_KEY_LAMP_MODE, "api");
        repaint_base_color();
        mqtt_client_publish_state();
        swarm_notify_state();   // PlanV3 V2.5 — mirror to swarm (no-op if not an active member)
        return 0;
    }
    if (strcmp(mode, "js") == 0) {
        ws_server_set_mode(LAMP_MODE_API);
        config_store_set_str(CONFIG_KEY_LAMP_MODE, "js");
        if (led_control_is_on()) (void)start_current_js();
        mqtt_client_publish_state();
        swarm_notify_state();   // PlanV3 V2.5 — mirror to swarm (no-op if not an active member)
        return 0;
    }
    if (strcmp(mode, "frame") == 0) {
        ws_server_set_mode(LAMP_MODE_API);
        js_api_stop();
        config_store_set_str(CONFIG_KEY_LAMP_MODE, "frame");
        if (led_control_is_on()) (void)frame_store_display();
        mqtt_client_publish_state();
        swarm_notify_state();   // PlanV3 V2.5 — mirror to swarm (no-op if not an active member;
                                 // the packet format has no "frame" wire value — see swarm.c)
        return 0;
    }
    if (strcmp(mode, "stream") == 0) {
        js_api_stop();
        ws_server_set_mode(LAMP_MODE_STREAM);
        mqtt_client_publish_state();
        swarm_notify_state();   // PlanV3 V2.5 — mirror to swarm (no-op if not an active member)
        return 0;
    }
    return -1;
}

void light_api_get_mode(char *out, size_t out_len)
{
    // Mirrors the GET /api/mode "mode" field: a live WS session reports
    // "stream"; otherwise the persisted intent ("api", "js" or "frame").
    if (ws_server_get_mode() == LAMP_MODE_STREAM) {
        snprintf(out, out_len, "stream");
        return;
    }
    get_persistent_mode(out, out_len);
}

// PlanV3 V2.5 — swarm.c's state-snapshot builder needs the raw persisted
// mode (never "stream"): the packet's mode field only encodes api/js, so a
// transient stream takeover shouldn't leak into what gets broadcast to the
// swarm. Thin wrapper so swarm.c doesn't need its own copy of the
// CONFIG_KEY_LAMP_MODE read.
void light_api_get_persistent_mode(char *out, size_t out_len)
{
    get_persistent_mode(out, out_len);
}

// Auto stream takeover, driven by the WS layer the moment real pixel frames
// arrive — so a client previewing over /ws never has to POST /api/mode
// "stream" first, and a running JS effect can't composite under the stream.
// Always stops the live runtime (even if mode was already STREAM) because a
// power-on can re-launch the player after an explicit setMode("stream").
void light_api_enter_stream(void)
{
    js_api_stop();                       /* suspend JS player / hardcoded effect */
    ws_server_set_mode(LAMP_MODE_STREAM);
}

// Restore the persisted mode once the last stream socket closes. Mirrors the
// "js"/"api" branches of apply_mode without re-persisting (stream was never
// persisted). No-op if an explicit /api/mode call already left stream mode.
void light_api_exit_stream(void)
{
    if (ws_server_get_mode() != LAMP_MODE_STREAM) return;
    ws_server_set_mode(LAMP_MODE_API);
    char mode[16] = {0};
    get_persistent_mode(mode, sizeof(mode));
    if (strcmp(mode, "js") == 0 && led_control_is_on()) {
        (void)start_current_js();
    } else if (strcmp(mode, "frame") == 0 && led_control_is_on()) {
        (void)frame_store_display();
    } else if (strcmp(mode, "api") == 0) {
        repaint_base_color();
    }
}

// POST /api/power  body: {"on":true} or {"on":false}
static esp_err_t power_handler(httpd_req_t *req)
{
    if (pairing_http_check(req, PL_ROLE_USER) != ESP_OK) return ESP_FAIL;
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
    if (pairing_http_check(req, PL_ROLE_USER) != ESP_OK) return ESP_FAIL;
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
    // on its very next frame. Frame mode is the same story: the displayed
    // frame is content, not the user's solid-color intent (e.g. an HA
    // set_color hitting this same endpoint must not wipe a drawn frame) —
    // baseColor still updates so a later switch back to api mode reflects it.
    char mode[16] = {0};
    get_persistent_mode(mode, sizeof(mode));
    bool js_mode = (strcmp(mode, "js") == 0);
    bool content_mode = js_mode || (strcmp(mode, "frame") == 0);
    if (!content_mode) {
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
        // PlanV3 V2.5 fix round 2, bug 1 — this handler predates
        // light_api_apply_color_solid and duplicates its paint/persist logic
        // inline instead of calling it, so it never carried that function's
        // swarm_notify_state() hook. Placed here (not gated on content_mode)
        // so it covers BOTH branches above: the plain array-paint case and
        // the js/frame content_mode case, since baseColor changed in every
        // idx>0 case regardless of whether led_control_set_all() ran.
        swarm_notify_state();
    }

    free(colors);
    free(buf);

    ESP_LOGI(TAG, "Set %d LED colors%s", idx,
             content_mode ? (js_mode ? " (baseColor only — js mode)" : " (baseColor only — frame mode)") : "");

    httpd_resp_set_type(req, "application/json");
    char resp[64];
    snprintf(resp, sizeof(resp), "{\"status\":\"ok\",\"count\":%d}", idx);
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

// POST /api/frame — save a drawn frame (creator+). Raw RGB body, length
// must equal the logical grid exactly. PlanV3 V2.4.
static esp_err_t frame_post_handler(httpd_req_t *req)
{
    if (pairing_http_check(req, PL_ROLE_CREATOR) != ESP_OK) return ESP_FAIL;
    int w = led_control_get_logical_w();
    int h = led_control_get_logical_h();
    size_t want = (size_t)(w * h * 3);
    if (req->content_len != want) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        char msg[96];
        snprintf(msg, sizeof(msg),
                 "{\"status\":\"error\",\"message\":\"expected %u bytes (%dx%dx3)\"}",
                 (unsigned)want, w, h);
        httpd_resp_sendstr(req, msg);
        return ESP_OK;
    }
    uint8_t *buf = malloc(want);
    if (!buf) { httpd_resp_send_500(req); return ESP_OK; }
    size_t got = 0;
    while (got < want) {
        int r = httpd_req_recv(req, (char *)buf + got, want - got);
        if (r <= 0) { free(buf); httpd_resp_send_500(req); return ESP_OK; }
        got += r;
    }
    esp_err_t err = frame_store_save(w, h, buf, want);
    free(buf);
    if (err != ESP_OK) { httpd_resp_send_500(req); return ESP_OK; }
    char mode[16] = {0};
    get_persistent_mode(mode, sizeof(mode));
    if (strcmp(mode, "frame") == 0 && led_control_is_on()
        && ws_server_get_mode() != LAMP_MODE_STREAM) {
        (void)frame_store_display();
    }
    httpd_resp_set_type(req, "application/json");
    char ok[64];
    snprintf(ok, sizeof(ok), "{\"status\":\"ok\",\"w\":%d,\"h\":%d}", w, h);
    httpd_resp_sendstr(req, ok);
    return ESP_OK;
}

// GET /api/frame — read the stored frame back for re-editing (user+).
static esp_err_t frame_get_handler(httpd_req_t *req)
{
    if (pairing_http_check(req, PL_ROLE_USER) != ESP_OK) return ESP_FAIL;
    int w = led_control_get_logical_w();
    int h = led_control_get_logical_h();
    uint8_t *buf = NULL; size_t len = 0;
    if (frame_store_load(w, h, &buf, &len) != ESP_OK) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"no frame stored\"}");
        return ESP_OK;
    }
    char wh[8];
    snprintf(wh, sizeof(wh), "%d", w);
    httpd_resp_set_hdr(req, "X-Frame-W", wh);
    char hh[8];
    snprintf(hh, sizeof(hh), "%d", h);
    httpd_resp_set_hdr(req, "X-Frame-H", hh);
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_send(req, (const char *)buf, len);
    free(buf);
    return ESP_OK;
}

// GET /api/brightness -> {"brightness":255}
static esp_err_t brightness_get_handler(httpd_req_t *req)
{
    if (pairing_http_check(req, PL_ROLE_USER) != ESP_OK) return ESP_FAIL;
    char resp[32];
    snprintf(resp, sizeof(resp), "{\"brightness\":%d}", led_control_get_brightness());
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

// POST /api/brightness  body: {"brightness":128}
static esp_err_t brightness_set_handler(httpd_req_t *req)
{
    if (pairing_http_check(req, PL_ROLE_USER) != ESP_OK) return ESP_FAIL;
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

    light_api_apply_brightness((uint8_t)val);

    char resp[32];
    snprintf(resp, sizeof(resp), "{\"brightness\":%d}", val);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

// GET /api/fade -> {"onMs":N,"offMs":N}
static esp_err_t fade_get_handler(httpd_req_t *req)
{
    if (pairing_http_check(req, PL_ROLE_USER) != ESP_OK) return ESP_FAIL;
    char resp[48];
    snprintf(resp, sizeof(resp),
             "{\"onMs\":%u,\"offMs\":%u}",
             (unsigned)led_control_get_fade_on_ms(),
             (unsigned)led_control_get_fade_off_ms());
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

// GET /api/ap_js -> {"name":"ap-wormhole"}
// POST /api/ap_js {"name":"..."}  — set the script played while the lamp
// is in AP / onboarding mode. Empty name => clear (falls back to the
// built-in blue pulse). Persisted to NVS; takes effect on next boot.
// USER may read; CREATOR may write — same pattern as /api/fade. Pairing
// is enforced when the device is paired; in AP mode itself the device is
// unpaired so both verbs are open.
static esp_err_t ap_js_get_handler(httpd_req_t *req)
{
    if (pairing_http_check(req, PL_ROLE_USER) != ESP_OK) return ESP_FAIL;
    char name[64] = {0};
    config_get_str_or(CONFIG_KEY_AP_JS, name, sizeof(name), "");
    char resp[128];
    snprintf(resp, sizeof(resp), "{\"name\":\"%s\"}", name);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

// Permissive script-name validator. Same rule we apply to JS storage:
// non-empty, alphanumeric + dash/underscore/dot, <= 63 chars. Empty IS
// allowed here (it means "disable, use fallback").
static bool ap_js_name_valid(const char *s)
{
    size_t n = strlen(s);
    if (n == 0) return true;
    if (n > 63) return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        if (!ok) return false;
    }
    return true;
}

static esp_err_t ap_js_set_handler(httpd_req_t *req)
{
    if (pairing_http_check(req, PL_ROLE_CREATOR) != ESP_OK) return ESP_FAIL;
    char buf[160] = {0};
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    // Tiny hand-parse: locate "name" : "...".
    char name[64] = {0};
    char *p = strstr(buf, "\"name\"");
    if (p) p = strchr(p, ':');
    if (p) p = strchr(p, '"');
    if (p) {
        p++;
        char *end = strchr(p, '"');
        if (end) {
            size_t n = (size_t)(end - p);
            if (n >= sizeof(name)) n = sizeof(name) - 1;
            memcpy(name, p, n);
            name[n] = '\0';
        }
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing name");
        return ESP_FAIL;
    }
    if (!ap_js_name_valid(name)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid name (allowed: a-z A-Z 0-9 - _ .)");
        return ESP_FAIL;
    }
    config_store_set_str(CONFIG_KEY_AP_JS, name);
    return ap_js_get_handler(req);
}

// GET /api/fade/debug -> live fade-engine snapshot. Used during Phase 33
// bring-up to observe what the fade machinery is actually doing without
// access to serial logs. Safe to leave in (admin-gated, small response).
static esp_err_t fade_debug_handler(httpd_req_t *req)
{
    if (pairing_http_check(req, PL_ROLE_ADMIN) != ESP_OK) return ESP_FAIL;
    led_fade_debug_t d;
    led_control_fade_debug(&d);
    char resp[256];
    snprintf(resp, sizeof(resp),
             "{\"scaleQ16\":%u,\"dir\":%d,\"durationMs\":%u,\"elapsedMs\":%lu,"
             "\"powerOn\":%s,\"paintingActive\":%s,"
             "\"sinceExternalPaintMs\":%lu,\"armCount\":%lu,\"externalPaintCount\":%lu}",
             (unsigned)d.scale_q16, (int)d.dir, (unsigned)d.duration_ms,
             (unsigned long)d.elapsed_ms,
             d.power_on ? "true" : "false",
             d.painting_active ? "true" : "false",
             (unsigned long)d.since_external_paint_ms,
             (unsigned long)d.arm_count,
             (unsigned long)d.external_paint_count);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

// POST /api/fade {"onMs":N,"offMs":N} — either field optional; 0 = instant snap,
// upper bound 5000 ms (firmware clamp). Creator role to match brightness.
static esp_err_t fade_set_handler(httpd_req_t *req)
{
    if (pairing_http_check(req, PL_ROLE_CREATOR) != ESP_OK) return ESP_FAIL;
    char buf[96] = {0};
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    uint16_t on_ms  = led_control_get_fade_on_ms();
    uint16_t off_ms = led_control_get_fade_off_ms();
    char *p;
    if ((p = strstr(buf, "onMs")) != NULL && (p = strchr(p, ':')) != NULL) {
        int v = atoi(p + 1);
        if (v < 0) v = 0; else if (v > 5000) v = 5000;
        on_ms = (uint16_t)v;
    }
    if ((p = strstr(buf, "offMs")) != NULL && (p = strchr(p, ':')) != NULL) {
        int v = atoi(p + 1);
        if (v < 0) v = 0; else if (v > 5000) v = 5000;
        off_ms = (uint16_t)v;
    }
    led_control_set_fade_durations(on_ms, off_ms);
    return fade_get_handler(req);
}

// GET /api/limits -> {"maxBrightness":N,"maxCurrentMa":N}
static esp_err_t limits_get_handler(httpd_req_t *req)
{
    if (pairing_http_check(req, PL_ROLE_USER) != ESP_OK) return ESP_FAIL;
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
    if (pairing_http_check(req, PL_ROLE_ADMIN) != ESP_OK) return ESP_FAIL;
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
    if (pairing_http_check(req, PL_ROLE_USER) != ESP_OK) return ESP_FAIL;
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
    if (pairing_http_check(req, PL_ROLE_ADMIN) != ESP_OK) return ESP_FAIL;
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
    if (pairing_http_check(req, PL_ROLE_USER) != ESP_OK) return ESP_FAIL;
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
    if (pairing_http_check(req, PL_ROLE_ADMIN) != ESP_OK) return ESP_FAIL;
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
    if (pairing_http_check(req, PL_ROLE_USER) != ESP_OK) return ESP_FAIL;
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
    const char *current = js_api_current_name();
    float fps = js_api_get_fps();
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
    if (pairing_http_check(req, PL_ROLE_USER) != ESP_OK) return ESP_FAIL;
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
    if (pairing_http_check(req, PL_ROLE_USER) != ESP_OK) return ESP_FAIL;
    char persistent[16] = {0};
    get_persistent_mode(persistent, sizeof(persistent));
    const char *effective = (ws_server_get_mode() == LAMP_MODE_STREAM) ? "stream" : persistent;
    const char *current = js_api_current_name();
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
    if (pairing_http_check(req, PL_ROLE_USER) != ESP_OK) return ESP_FAIL;
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

// --- Phase 29 — wormhole render mode -----------------------------------------
//
// Three endpoints, all 404 for non-wormhole lamps. JSON style matches
// /api/grid and /api/orientation. See docs/wormhole-api.md.

// Build the shared GET /api/wormhole body into `out`. The physical/creative
// arrays carry one object per ring (wormhole_rings()). Always NUL-terminates.
static void wormhole_build_json(char *out, size_t max_len)
{
    int rings = wormhole_rings();
    int render_px = wormhole_render_pixels();
    const char *mode = (wormhole_mode() == WORMHOLE_MODE_MIRROR) ? "mirror" : "strip";

    size_t o = 0;
    o += snprintf(out + o, max_len - o,
                  "{\"mode\":\"%s\",\"rings\":%d,\"mirrorAllowed\":%s,"
                  "\"renderPixels\":%d,\"streamPixels\":%d,\"physical\":[",
                  mode, rings, wormhole_mirror_allowed() ? "true" : "false",
                  render_px, render_px);
    for (int r = 0; r < rings && o + 1 < max_len; r++) {
        int face = 0, dir = 0, off = 0;
        wormhole_get_phys(r, &face, &dir, &off);
        o += snprintf(out + o, max_len - o,
                      "%s{\"face\":%d,\"direction\":%d,\"offset\":%d}",
                      r ? "," : "", face, dir, off);
    }
    o += snprintf(out + o, max_len - o, "],\"creative\":[");
    for (int r = 0; r < rings && o + 1 < max_len; r++) {
        bool rev = false; int coff = 0; float br = 1.0f;
        wormhole_get_creative(r, &rev, &coff, &br);
        o += snprintf(out + o, max_len - o,
                      "%s{\"reverse\":%s,\"offset\":%d,\"brightness\":%.3f}",
                      r ? "," : "", rev ? "true" : "false", coff, br);
    }
    snprintf(out + o, max_len - o, "]}");
}

// Send the GET /api/wormhole body — shared by GET and the two POST handlers.
static esp_err_t wormhole_send_state(httpd_req_t *req)
{
    char *json = malloc(2048);
    if (!json) return ESP_FAIL;
    wormhole_build_json(json, 2048);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

// GET /api/wormhole — role: user. 404 for non-wormhole lamps.
static esp_err_t wormhole_get_handler(httpd_req_t *req)
{
    if (pairing_http_check(req, PL_ROLE_USER) != ESP_OK) return ESP_FAIL;
    if (!wormhole_is_wormhole()) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not a wormhole lamp");
        return ESP_FAIL;
    }
    return wormhole_send_state(req);
}

// POST /api/wormhole — role: admin. Body: any subset of {mode,rings,physical}.
// A mode/rings change re-inits the player and bumps the WS stream generation.
static esp_err_t wormhole_post_handler(httpd_req_t *req)
{
    if (pairing_http_check(req, PL_ROLE_ADMIN) != ESP_OK) return ESP_FAIL;
    if (!wormhole_is_wormhole()) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not a wormhole lamp");
        return ESP_FAIL;
    }
    int content_len = req->content_len;
    if (content_len <= 0 || content_len > 2048) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_FAIL;
    }
    char *buf = malloc(content_len + 1);
    if (!buf) return ESP_FAIL;
    int received = 0;
    while (received < content_len) {
        int ret = httpd_req_recv(req, buf + received, content_len - received);
        if (ret <= 0) { free(buf); return ESP_FAIL; }
        received += ret;
    }
    buf[content_len] = '\0';

    bool topology_changed = false;
    char *p;

    // mode — "strip" | "mirror". Mirror gated on the geometry check.
    if ((p = strstr(buf, "\"mode\"")) != NULL && (p = strchr(p, ':')) != NULL
        && (p = strchr(p, '"')) != NULL) {
        p++;
        const char *end = strchr(p, '"');
        char mode[16] = {0};
        if (end && (size_t)(end - p) < sizeof(mode)) {
            memcpy(mode, p, end - p);
            if (strcmp(mode, "mirror") != 0 && strcmp(mode, "strip") != 0) {
                free(buf);
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "mode must be strip|mirror");
                return ESP_FAIL;
            }
            if (strcmp(mode, "mirror") == 0 && !wormhole_mirror_allowed()) {
                free(buf);
                httpd_resp_set_type(req, "application/json");
                httpd_resp_set_status(req, "409 Conflict");
                httpd_resp_sendstr(req, "{\"error\":\"mirror not allowed for this geometry\"}");
                return ESP_FAIL;
            }
            char cur[16] = {0};
            config_get_str_or(CONFIG_KEY_WH_MODE, cur, sizeof(cur), "strip");
            if (strcmp(cur, mode) != 0) topology_changed = true;
            config_store_set_str(CONFIG_KEY_WH_MODE, mode);
        }
    }

    // rings — i32 >= 1.
    if ((p = strstr(buf, "\"rings\"")) != NULL && (p = strchr(p, ':')) != NULL) {
        int rings = atoi(p + 1);
        if (rings < 1) {
            free(buf);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "rings must be >= 1");
            return ESP_FAIL;
        }
        int32_t cur = 0;
        config_store_get_i32(CONFIG_KEY_WH_RINGS, &cur);
        if ((int)cur != rings) topology_changed = true;
        config_store_set_i32(CONFIG_KEY_WH_RINGS, rings);
    }

    // physical — full JSON array, stored verbatim under wh_phys.
    if ((p = strstr(buf, "\"physical\"")) != NULL) {
        const char *start = strchr(p, '[');
        if (start) {
            int depth = 0;
            const char *end = start;
            for (; *end; end++) {
                if (*end == '[') depth++;
                else if (*end == ']') { depth--; if (depth == 0) { end++; break; } }
            }
            if (depth == 0 && end > start) {
                size_t alen = (size_t)(end - start);
                char *arr = malloc(alen + 1);
                if (arr) {
                    memcpy(arr, start, alen);
                    arr[alen] = '\0';
                    config_store_set_str(CONFIG_KEY_WH_PHYS, arr);
                    free(arr);
                }
            }
        }
    }

    free(buf);

    // Re-read config so wormhole_*() reflect the new values, and bump the
    // WS stream generation (an active stream then closes with 4002).
    wormhole_reload();

    // A mode/rings change alters the render geometry — re-init the player by
    // restarting the active script cleanly. A brief flicker is acceptable.
    // Phase 35 — works for hardcoded effects too via js_api_play dispatch.
    if (topology_changed && js_api_is_running()) {
        const char *name = js_api_current_name();
        if (name && name[0]) {
            char nm[64] = {0};
            snprintf(nm, sizeof(nm), "%s", name);
            js_api_stop();
            // Phase 41 — autoswitch=false: the user just set this mode by hand
            // (or changed rings); re-applying the effect's declared @mode here
            // would immediately revert their choice, making the toggle a no-op.
            js_api_play_ex(nm, JS_DEFAULT_FPS, false);
        }
    }

    return wormhole_send_state(req);
}

// POST /api/wormhole/creative — role: creator. Body: {"creative":[…]} (full
// array) or {"ring":N,"reverse":…,"offset":…,"brightness":…} (single patch).
// Stored regardless of mode; no player re-init, no stream close.
static esp_err_t wormhole_creative_handler(httpd_req_t *req)
{
    if (pairing_http_check(req, PL_ROLE_CREATOR) != ESP_OK) return ESP_FAIL;
    if (!wormhole_is_wormhole()) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not a wormhole lamp");
        return ESP_FAIL;
    }
    int content_len = req->content_len;
    if (content_len <= 0 || content_len > 2048) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_FAIL;
    }
    char *buf = malloc(content_len + 1);
    if (!buf) return ESP_FAIL;
    int received = 0;
    while (received < content_len) {
        int ret = httpd_req_recv(req, buf + received, content_len - received);
        if (ret <= 0) { free(buf); return ESP_FAIL; }
        received += ret;
    }
    buf[content_len] = '\0';

    char *p;
    if ((p = strstr(buf, "\"creative\"")) != NULL) {
        // Full-array form — store wh_creative verbatim.
        const char *start = strchr(p, '[');
        if (start) {
            int depth = 0;
            const char *end = start;
            for (; *end; end++) {
                if (*end == '[') depth++;
                else if (*end == ']') { depth--; if (depth == 0) { end++; break; } }
            }
            if (depth == 0 && end > start) {
                size_t alen = (size_t)(end - start);
                char *arr = malloc(alen + 1);
                if (arr) {
                    memcpy(arr, start, alen);
                    arr[alen] = '\0';
                    config_store_set_str(CONFIG_KEY_WH_CREATIVE, arr);
                    free(arr);
                }
            }
        }
    } else if ((p = strstr(buf, "\"ring\"")) != NULL) {
        // Single-ring patch — merge into the stored array, rebuild it whole.
        char *rp = strchr(p, ':');
        int ring = rp ? atoi(rp + 1) : -1;
        int rings = wormhole_rings();
        if (ring >= 0 && ring < rings) {
            // The patch only mentions some fields; missing ones keep the
            // ring's current value (read back via the accessor).
            bool rev = false; int coff = 0; float br = 1.0f;
            wormhole_get_creative(ring, &rev, &coff, &br);
            char *q;
            if ((q = strstr(buf, "\"reverse\"")) != NULL && (q = strchr(q, ':')) != NULL) {
                q++;
                while (*q == ' ') q++;
                if (!strncmp(q, "true", 4)) rev = true;
                else if (!strncmp(q, "false", 5)) rev = false;
                else rev = atoi(q) != 0;
            }
            if ((q = strstr(buf, "\"offset\"")) != NULL && (q = strchr(q, ':')) != NULL) {
                coff = ((atoi(q + 1) % 24) + 24) % 24;
            }
            if ((q = strstr(buf, "\"brightness\"")) != NULL && (q = strchr(q, ':')) != NULL) {
                br = (float)atof(q + 1);
                if (br < 0.0f) br = 0.0f;
                if (br > 1.0f) br = 1.0f;
            }
            // Rebuild the full creative array — every ring read via the
            // accessor, the patched ring overridden.
            char *arr = malloc(2048);
            if (arr) {
                size_t o = 0;
                o += snprintf(arr + o, 2048 - o, "[");
                for (int r = 0; r < rings; r++) {
                    bool rr = false; int ro = 0; float rb = 1.0f;
                    if (r == ring) { rr = rev; ro = coff; rb = br; }
                    else wormhole_get_creative(r, &rr, &ro, &rb);
                    o += snprintf(arr + o, 2048 - o,
                                  "%s{\"reverse\":%s,\"offset\":%d,\"brightness\":%.3f}",
                                  r ? "," : "", rr ? "true" : "false", ro, rb);
                }
                snprintf(arr + o, 2048 - o, "]");
                config_store_set_str(CONFIG_KEY_WH_CREATIVE, arr);
                free(arr);
            }
        }
    }

    free(buf);
    // Creative knobs take effect on the next frame — just re-read config.
    // No player re-init, no stream close (the render geometry is unchanged):
    // wormhole_reload_creative() deliberately does not bump the stream gen.
    wormhole_reload_creative();
    return wormhole_send_state(req);
}

// PlanV3 V2.5 Task 1 — swarm_radio coexistence spike. Admin-gated debug
// surface for the ESP-NOW GO/NO-GO bench test; kept afterward as ongoing
// diagnostics. POST /api/swarm/ping broadcasts a 32-byte plaintext test
// packet: "PLSW-SPIKE" (10B) + this lamp's STA MAC (6B) + a monotonic
// sequence number (4B, native-endian) + zero padding to 32B.
static esp_err_t swarm_ping_handler(httpd_req_t *req)
{
    if (pairing_http_check(req, PL_ROLE_ADMIN) != ESP_OK) return ESP_FAIL;

    static uint32_t s_ping_seq = 0;
    uint8_t pkt[32] = {0};
    memcpy(pkt, "PLSW-SPIKE", 10);
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    memcpy(pkt + 10, mac, sizeof(mac));
    uint32_t seq = ++s_ping_seq;
    memcpy(pkt + 16, &seq, sizeof(seq));

    esp_err_t err = swarm_radio_send(pkt, sizeof(pkt));
    char resp[96];
    snprintf(resp, sizeof(resp), "{\"sent\":%s,\"seq\":%lu}",
             err == ESP_OK ? "true" : "false", (unsigned long)seq);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

// GET /api/swarm/stats -> {"tx":n,"rx":n,"txFail":n,"lastFrom":"aa:bb:cc:dd:ee:ff",
//                           "dropAuth":n,"dropReplay":n,"relayed":n,"applied":n,
//                           "stackFree":n}
// PlanV3 V2.5 Task 3 — folds swarm.c's protocol-layer counters (Task 2) in
// alongside swarm_radio's link-layer counters (Task 1). `stackFree` is the
// swarm worker task's FreeRTOS stack high-water mark in bytes (0 if the
// worker isn't running) — instrumentation for the 4 KB stack js_api_play
// runs on inside that task.
static esp_err_t swarm_stats_handler(httpd_req_t *req)
{
    if (pairing_http_check(req, PL_ROLE_ADMIN) != ESP_OK) return ESP_FAIL;

    uint32_t tx = 0, rx = 0, tx_fail = 0;
    swarm_radio_stats(&tx, &rx, &tx_fail);
    uint8_t mac[6] = {0};
    swarm_radio_last_from(mac);
    uint32_t drop_auth = 0, drop_replay = 0, relayed = 0, applied = 0;
    swarm_debug_stats(&drop_auth, &drop_replay, &relayed, &applied);
    uint32_t stack_free = swarm_worker_stack_free();
    char resp[288];
    snprintf(resp, sizeof(resp),
             "{\"tx\":%lu,\"rx\":%lu,\"txFail\":%lu,"
             "\"lastFrom\":\"%02x:%02x:%02x:%02x:%02x:%02x\","
             "\"dropAuth\":%lu,\"dropReplay\":%lu,\"relayed\":%lu,\"applied\":%lu,"
             "\"stackFree\":%lu}",
             (unsigned long)tx, (unsigned long)rx, (unsigned long)tx_fail,
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
             (unsigned long)drop_auth, (unsigned long)drop_replay,
             (unsigned long)relayed, (unsigned long)applied,
             (unsigned long)stack_free);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

// PlanV3 V2.5 Task 3 — /api/swarm provisioning. Admin-gated like the Task 1
// ping/stats debug surface. The 64-hex swarm key is only ever read off the
// wire and handed to swarm_join(), which persists it to NVS; it is never
// echoed back in a response or written to a log line.

// GET /api/swarm -> {"member":bool,"id":"<16hex or empty>","enabled":bool,"channel":N}
static esp_err_t swarm_get_handler(httpd_req_t *req)
{
    if (pairing_http_check(req, PL_ROLE_ADMIN) != ESP_OK) return ESP_FAIL;

    char id[17] = {0};
    swarm_get_id(id, sizeof(id));
    char resp[128];
    snprintf(resp, sizeof(resp),
             "{\"member\":%s,\"id\":\"%s\",\"enabled\":%s,\"channel\":%d}",
             swarm_is_member() ? "true" : "false", id,
             swarm_is_enabled() ? "true" : "false", swarm_get_channel());
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

// POST /api/swarm  body: {"id":"<16hex>","key":"<64hex>","channel":N} -> join.
// channel is optional (0 = unset/current, matching swarm_join). 400
// {"status":"error","message":"invalid id/key/channel"} on malformed input;
// swarm_join validates hex format + channel range and implies enable, so
// there is no separate enable step here on success.
static esp_err_t swarm_post_handler(httpd_req_t *req)
{
    if (pairing_http_check(req, PL_ROLE_ADMIN) != ESP_OK) return ESP_FAIL;

    int content_len = req->content_len;
    if (content_len <= 0 || content_len > 512) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"invalid id/key/channel\"}");
        return ESP_OK;
    }
    char *buf = malloc(content_len + 1);
    if (!buf) { httpd_resp_send_500(req); return ESP_OK; }
    int received = 0;
    while (received < content_len) {
        int ret = httpd_req_recv(req, buf + received, content_len - received);
        if (ret <= 0) { free(buf); httpd_resp_send_500(req); return ESP_OK; }
        received += ret;
    }
    buf[content_len] = '\0';

    char id[32] = {0};
    char key[80] = {0};
    int channel = 0;
    bool have_id = false, have_key = false;

    char *p;
    if ((p = strstr(buf, "\"id\"")) != NULL && (p = strchr(p, ':')) != NULL
        && (p = strchr(p, '"')) != NULL) {
        p++;
        const char *end = strchr(p, '"');
        if (end && (size_t)(end - p) < sizeof(id)) {
            memcpy(id, p, end - p);
            have_id = true;
        }
    }
    if ((p = strstr(buf, "\"key\"")) != NULL && (p = strchr(p, ':')) != NULL
        && (p = strchr(p, '"')) != NULL) {
        p++;
        const char *end = strchr(p, '"');
        if (end && (size_t)(end - p) < sizeof(key)) {
            memcpy(key, p, end - p);
            have_key = true;
        }
    }
    if ((p = strstr(buf, "\"channel\"")) != NULL && (p = strchr(p, ':')) != NULL) {
        channel = atoi(p + 1);
    }
    // Key material lives only in `buf` (freed now) and the stack `key`
    // buffer handed to swarm_join below — never logged, never in `resp`.
    free(buf);

    // Membership gate (mirrors the apps' transport == "wifi" eligibility rule):
    // an ESP-NOW member needs a concrete radio channel, which a lamp normally
    // captures from the AP it is associated with. Joining an AP-less lamp —
    // e.g. through its own onboarding softAP, the only HTTP path a BLE-only
    // lamp has — with channel 0 would create a member that can never find
    // the swarm. Refuse unless the caller pins the channel explicitly (the
    // hook a future BLE-aware client would use).
    if (have_id && have_key && channel == 0 && !wifi_is_connected()) {
        free(buf);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"lamp is not on WiFi — joining a swarm needs a WiFi connection (or an explicit channel)\"}");
        return ESP_OK;
    }
    if (!have_id || !have_key || swarm_join(id, key, channel) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"invalid id/key/channel\"}");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

// DELETE /api/swarm -> leave: wipes the identity keys (sw_id/sw_key/sw_on)
// and disables. Always succeeds, even when already not a member.
static esp_err_t swarm_delete_handler(httpd_req_t *req)
{
    if (pairing_http_check(req, PL_ROLE_ADMIN) != ESP_OK) return ESP_FAIL;

    swarm_leave();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

// POST /api/swarm/enable  body: {"enabled":true|false} -> toggle without
// losing membership. Enabling when not a member is rejected (400):
// swarm_set_enabled() returns ESP_ERR_INVALID_STATE for that case; disabling
// a non-member is always a harmless no-op (200).
static esp_err_t swarm_enable_handler(httpd_req_t *req)
{
    if (pairing_http_check(req, PL_ROLE_ADMIN) != ESP_OK) return ESP_FAIL;

    char buf[64] = {0};
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"invalid enabled\"}");
        return ESP_OK;
    }
    bool enabled = (strstr(buf, "true") != NULL);

    if (swarm_set_enabled(enabled) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"not a swarm member\"}");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

esp_err_t light_api_register(httpd_handle_t server)
{
    // Hook into the LED fade lifecycle so we can stop the JS player AFTER a
    // fade-out finishes (rather than synchronously on the off command).
    led_control_set_fade_complete_cb(on_fade_complete);

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

    // PlanV3 V2.4 — draw-to-lamp. POST saves a drawn frame (creator+); GET
    // reads it back for re-editing (user+).
    httpd_uri_t frame_post = {
        .uri = "/api/frame",
        .method = HTTP_POST,
        .handler = frame_post_handler
    };
    httpd_register_uri_handler(server, &frame_post);

    httpd_uri_t frame_get = {
        .uri = "/api/frame",
        .method = HTTP_GET,
        .handler = frame_get_handler
    };
    httpd_register_uri_handler(server, &frame_get);

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

    httpd_uri_t fade_get = {
        .uri = "/api/fade",
        .method = HTTP_GET,
        .handler = fade_get_handler
    };
    httpd_register_uri_handler(server, &fade_get);

    httpd_uri_t fade_set = {
        .uri = "/api/fade",
        .method = HTTP_POST,
        .handler = fade_set_handler
    };
    httpd_register_uri_handler(server, &fade_set);

    httpd_uri_t fade_debug = {
        .uri = "/api/fade/debug",
        .method = HTTP_GET,
        .handler = fade_debug_handler
    };
    httpd_register_uri_handler(server, &fade_debug);

    httpd_uri_t ap_js_get = {
        .uri = "/api/ap_js",
        .method = HTTP_GET,
        .handler = ap_js_get_handler
    };
    httpd_register_uri_handler(server, &ap_js_get);

    httpd_uri_t ap_js_set = {
        .uri = "/api/ap_js",
        .method = HTTP_POST,
        .handler = ap_js_set_handler
    };
    httpd_register_uri_handler(server, &ap_js_set);

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

    // Phase 29 — wormhole render mode. GET is user, POST is admin, the
    // creative patch is creator. All three 404 for non-wormhole lamps.
    httpd_uri_t wormhole_get = {
        .uri = "/api/wormhole", .method = HTTP_GET, .handler = wormhole_get_handler
    };
    httpd_register_uri_handler(server, &wormhole_get);
    httpd_uri_t wormhole_post = {
        .uri = "/api/wormhole", .method = HTTP_POST, .handler = wormhole_post_handler
    };
    httpd_register_uri_handler(server, &wormhole_post);
    httpd_uri_t wormhole_creative = {
        .uri = "/api/wormhole/creative", .method = HTTP_POST, .handler = wormhole_creative_handler
    };
    httpd_register_uri_handler(server, &wormhole_creative);

    // PlanV3 V2.5 Task 1 — swarm_radio coexistence spike debug surface.
    httpd_uri_t swarm_ping = {
        .uri = "/api/swarm/ping", .method = HTTP_POST, .handler = swarm_ping_handler
    };
    httpd_register_uri_handler(server, &swarm_ping);
    httpd_uri_t swarm_stats = {
        .uri = "/api/swarm/stats", .method = HTTP_GET, .handler = swarm_stats_handler
    };
    httpd_register_uri_handler(server, &swarm_stats);

    // PlanV3 V2.5 Task 3 — /api/swarm provisioning (join/leave/enable). GET,
    // POST and DELETE on the same "/api/swarm" URI are three distinct
    // registrations, plus the enable sub-route — 4 new handler slots total.
    httpd_uri_t swarm_get = {
        .uri = "/api/swarm", .method = HTTP_GET, .handler = swarm_get_handler
    };
    httpd_register_uri_handler(server, &swarm_get);
    httpd_uri_t swarm_post = {
        .uri = "/api/swarm", .method = HTTP_POST, .handler = swarm_post_handler
    };
    httpd_register_uri_handler(server, &swarm_post);
    httpd_uri_t swarm_delete = {
        .uri = "/api/swarm", .method = HTTP_DELETE, .handler = swarm_delete_handler
    };
    httpd_register_uri_handler(server, &swarm_delete);
    httpd_uri_t swarm_enable = {
        .uri = "/api/swarm/enable", .method = HTTP_POST, .handler = swarm_enable_handler
    };
    httpd_register_uri_handler(server, &swarm_enable);

    return ESP_OK;
}

static esp_err_t bt_get_handler(httpd_req_t *req)
{
    if (pairing_http_check(req, PL_ROLE_USER) != ESP_OK) return ESP_FAIL;
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
    if (pairing_http_check(req, PL_ROLE_ADMIN) != ESP_OK) return ESP_FAIL;
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
