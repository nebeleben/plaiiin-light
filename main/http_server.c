#include "http_server.h"
#include "captive_portal.h"
#include "light_api.h"
#include "ws_server.h"
#include "ota_update.h"
#include "plaiiin_mqtt.h"
#include "config_store.h"
#include "led_control.h"
#include "js_storage.h"
#include "pairing.h"
#include "form_prompt.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "http_server";

// Forward declarations for the embedded /api docs page so the content-negotiating
// /api handler below can fall through to the HTML page. The full block of
// extern declarations for the other portal pages lives further down.
extern const uint8_t api_html_start[] asm("_binary_api_html_start");
extern const uint8_t api_html_end[]   asm("_binary_api_html_end");

// GET /api - browsers (Accept: text/html) get the docs page; everything else
// gets the JSON device-info payload below.
static esp_err_t api_html_handler(httpd_req_t *req)
{
    portal_send_html(req, api_html_start, api_html_end, NULL);
    return ESP_OK;
}

// GET /api - device info (reads NVS with Kconfig fallback)
static esp_err_t api_info_handler(httpd_req_t *req)
{
    // Content-negotiate: a browser hitting /api as a page wants HTML; CLI/JS
    // clients sending Accept: application/json (or no Accept) get JSON.
    char accept[128] = {0};
    if (httpd_req_get_hdr_value_str(req, "Accept", accept, sizeof(accept) - 1) == ESP_OK) {
        if (strstr(accept, "text/html") && !strstr(accept, "application/json")) {
            return api_html_handler(req);
        }
    }

    char node_name[64], vendor[64], api_ver[32];
    char lamp_type[32], lamp_form[32];

    config_get_str_or(CONFIG_KEY_NODE_NAME, node_name, sizeof(node_name), CONFIG_PLAIIIN_NODE_NAME);
    config_get_str_or(CONFIG_KEY_VENDOR_NAME, vendor, sizeof(vendor), CONFIG_PLAIIIN_VENDOR_NAME);
    config_get_str_or(CONFIG_KEY_API_VERSION, api_ver, sizeof(api_ver), CONFIG_PLAIIIN_API_VERSION);

    config_get_str_or(CONFIG_KEY_LAMP_TYPE, lamp_type, sizeof(lamp_type), CONFIG_PLAIIIN_LAMP_TYPE);
    config_get_str_or(CONFIG_KEY_LAMP_FORM, lamp_form, sizeof(lamp_form), CONFIG_PLAIIIN_FORM);

    int32_t led_pin = config_get_i32_or(CONFIG_KEY_LED_PIN, CONFIG_PLAIIIN_LED_PIN);
    int32_t led_clk_pin = config_get_i32_or(CONFIG_KEY_LED_CLK_PIN, CONFIG_PLAIIIN_LED_CLK_PIN);
    int rotation       = led_control_get_rotation();
    int origin         = led_control_get_origin();
    bool serpentine    = led_control_get_serpentine();
    int serp_axis      = led_control_get_serp_axis();
    int led_count = led_control_get_count();

    char led_type_str[16] = {0};
    config_get_str_or(CONFIG_KEY_LED_TYPE, led_type_str, sizeof(led_type_str), CONFIG_PLAIIIN_LED_TYPE);

    int phys_w = led_control_get_physical_w();
    int phys_h = led_control_get_physical_h();
    int logical_w = led_control_get_logical_w();
    int logical_h = led_control_get_logical_h();
    int px_group_w = led_control_get_pixel_group_w();
    int px_group_h = led_control_get_pixel_group_h();
    int32_t btn_pwr  = config_get_i32_or(CONFIG_KEY_BTN_PWR_PIN,  CONFIG_PLAIIIN_BTN_PWR_PIN);
    int32_t btn_next = config_get_i32_or(CONFIG_KEY_BTN_NEXT_PIN, CONFIG_PLAIIIN_BTN_NEXT_PIN);
    int32_t btn_prev = config_get_i32_or(CONFIG_KEY_BTN_PREV_PIN, CONFIG_PLAIIIN_BTN_PREV_PIN);

    char json[900];
    snprintf(json, sizeof(json),
        "{\"vendor\":\"%s\",\"apiVersion\":\"%s\",\"firmwareVersion\":\"%s\","
        "\"nodeName\":\"%s\","
        "\"ledPin\":%ld,\"ledClkPin\":%ld,\"ledCount\":%d,\"ledType\":\"%s\","
        "\"lampType\":\"%s\",\"lampForm\":\"%s\","
        "\"physicalW\":%d,\"physicalH\":%d,"
        "\"logicalW\":%d,\"logicalH\":%d,"
        "\"pixelGroupW\":%d,\"pixelGroupH\":%d,"
        "\"rotation\":%d,\"origin\":%d,\"serpentine\":%s,\"serpentineAxis\":%d,"
        "\"buttonPwrPin\":%ld,\"buttonNextPin\":%ld,\"buttonPrevPin\":%ld}",
        vendor, api_ver, CONFIG_PLAIIIN_FIRMWARE_VERSION,
        node_name, (long)led_pin, (long)led_clk_pin, led_count,
        led_type_str, lamp_type, lamp_form,
        phys_w, phys_h, logical_w, logical_h, px_group_w, px_group_h,
        rotation, origin, serpentine ? "true" : "false", serp_axis,
        (long)btn_pwr, (long)btn_next, (long)btn_prev);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    return ESP_OK;
}

// GET /api/storage -> {"total":N,"used":N,"free":N,"files":N}
static esp_err_t storage_info_handler(httpd_req_t *req)
{
    if (pairing_http_check(req) != ESP_OK) return ESP_FAIL;
    size_t total = 0, used = 0, files = 0;
    esp_err_t err = js_storage_info(&total, &used, &files);
    if (err != ESP_OK) {
        // SPIFFS not mounted (older flash without storage partition).
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"total\":0,\"used\":0,\"free\":0,\"files\":0}");
        return ESP_OK;
    }
    size_t free_b = (used > total) ? 0 : (total - used);
    char json[128];
    snprintf(json, sizeof(json),
        "{\"total\":%u,\"used\":%u,\"free\":%u,\"files\":%u}",
        (unsigned)total, (unsigned)used, (unsigned)free_b, (unsigned)files);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    return ESP_OK;
}

// Phase 26 — per-lamp physical-form descriptor injected into AI compose
// prompts. GET returns {form, default, override, hasOverride, effective};
// PUT replaces the editable override (empty body clears it); DELETE clears it.
// All three sit behind pairing_http_check, consistent with /api/mqtt etc.

// Append `in` to the JSON string already in `out`, escaping it as a JSON
// string body. `out` must already be NUL-terminated; stays within out_len.
static void json_escape_append(const char *in, char *out, size_t out_len)
{
    size_t o = strlen(out);
    for (const char *p = in; *p && o + 7 < out_len; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
        else if (c == '\r') { out[o++] = '\\'; out[o++] = 'r'; }
        else if (c == '\t') { out[o++] = '\\'; out[o++] = 't'; }
        else if (c < 0x20)  { o += snprintf(out + o, out_len - o, "\\u%04x", c); }
        else out[o++] = (char)c;
    }
    out[o] = '\0';
}

// GET /api/form-prompt
static esp_err_t form_prompt_get_handler(httpd_req_t *req)
{
    if (pairing_http_check(req) != ESP_OK) return ESP_FAIL;

    char lamp_form[32];
    config_get_str_or(CONFIG_KEY_LAMP_FORM, lamp_form, sizeof(lamp_form), CONFIG_PLAIIIN_FORM);

    char def[1024] = {0}, ovr[1024] = {0};
    form_prompt_build_default(def, sizeof(def));
    bool has_override =
        (config_store_get_str(CONFIG_KEY_FORM_PROMPT, ovr, sizeof(ovr)) == ESP_OK && ovr[0]);

    size_t cap = 6144;
    char *json = malloc(cap);
    if (!json) return ESP_FAIL;
    json[0] = '\0';
    strcat(json, "{\"form\":\"");
    json_escape_append(lamp_form, json, cap);
    strcat(json, "\",\"default\":\"");
    json_escape_append(def, json, cap);
    strcat(json, "\",\"override\":\"");
    if (has_override) json_escape_append(ovr, json, cap);
    strcat(json, has_override ? "\",\"hasOverride\":true,\"effective\":\""
                              : "\",\"hasOverride\":false,\"effective\":\"");
    json_escape_append(has_override ? ovr : def, json, cap);
    strcat(json, "\"}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

// PUT /api/form-prompt — body is the raw override text (empty body clears it).
static esp_err_t form_prompt_put_handler(httpd_req_t *req)
{
    if (pairing_http_check(req) != ESP_OK) return ESP_FAIL;
    int content_len = req->content_len;
    if (content_len < 0 || content_len > 2048) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad request");
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

    esp_err_t err;
    if (content_len == 0) {
        const char *keys[] = { CONFIG_KEY_FORM_PROMPT };
        err = config_store_erase_keys(keys, 1);
    } else {
        err = config_store_set_str(CONFIG_KEY_FORM_PROMPT, buf);
    }
    free(buf);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

// DELETE /api/form-prompt — clears the override, reverting to the default.
static esp_err_t form_prompt_delete_handler(httpd_req_t *req)
{
    if (pairing_http_check(req) != ESP_OK) return ESP_FAIL;
    const char *keys[] = { CONFIG_KEY_FORM_PROMPT };
    config_store_erase_keys(keys, 1);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

// Embedded static files
extern const uint8_t style_css_start[] asm("_binary_style_css_start");
extern const uint8_t style_css_end[]   asm("_binary_style_css_end");
extern const uint8_t shade_runtime_js_start[] asm("_binary_shade_runtime_js_start");
extern const uint8_t shade_runtime_js_end[]   asm("_binary_shade_runtime_js_end");
extern const uint8_t control_html_start[] asm("_binary_control_html_start");
extern const uint8_t control_html_end[]   asm("_binary_control_html_end");
extern const uint8_t test_html_start[] asm("_binary_test_html_start");
extern const uint8_t test_html_end[]   asm("_binary_test_html_end");
extern const uint8_t compose_html_start[] asm("_binary_compose_html_start");
extern const uint8_t compose_html_end[]   asm("_binary_compose_html_end");
extern const uint8_t mqtt_html_start[] asm("_binary_mqtt_html_start");
extern const uint8_t mqtt_html_end[]   asm("_binary_mqtt_html_end");
extern const uint8_t js_html_start[] asm("_binary_js_html_start");
extern const uint8_t js_html_end[]   asm("_binary_js_html_end");
// api_html_{start,end} forward-declared near the top of the file.

// Inject up to N <meta> tags before the </head> of an embedded portal page.
// Used to seed the page with the pairing token (when the request was
// authenticated) and the AI key (compose.html only). Anything missing falls
// back to no injection — the page still loads and shows its own "no key" UI.
//
// `extra_meta` is NUL-terminated HTML to splice in immediately before
// </head>. Pass NULL/"" to skip injection entirely.
static void send_html_with_meta(httpd_req_t *req, const uint8_t *start, const uint8_t *end,
                                const char *extra_meta)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    if (!extra_meta || !*extra_meta) {
        httpd_resp_send(req, (const char *)start, end - start);
        return;
    }
    // Find </head> in the static page. Static pages all carry one near the
    // top, so memmem stays cheap.
    size_t total = end - start;
    const char *needle = "</head>";
    size_t nlen = strlen(needle);
    const uint8_t *hit = NULL;
    for (size_t i = 0; i + nlen <= total; i++) {
        if (memcmp(start + i, needle, nlen) == 0) { hit = start + i; break; }
    }
    if (!hit) {
        // No </head> — page is too minimal for meta injection. Just send raw.
        httpd_resp_send(req, (const char *)start, total);
        return;
    }
    httpd_resp_send_chunk(req, (const char *)start, hit - start);
    httpd_resp_send_chunk(req, extra_meta, strlen(extra_meta));
    httpd_resp_send_chunk(req, (const char *)hit, end - hit);
    httpd_resp_send_chunk(req, NULL, 0);   // terminate chunked response
}

// Build the per-page <meta> block. Always includes plk-paired so the page
// JS knows whether to require Authorization on its fetches. Includes
// plk-token only when the *request itself* was authenticated (so an
// un-paired browser visiting a paired device can't just load /compose to
// scrape the token). compose.html (page_id="compose") additionally gets
// plk-aikey when present in NVS — replaces the network-exposed /api/ai/key.
static void build_meta_block(httpd_req_t *req, const char *page_id,
                             char *out, size_t out_len)
{
    bool paired = pairing_is_paired();
    bool authed = false;
    if (paired) {
        // pairing_http_check sends a 401 on failure; here we just want a
        // boolean. Recheck the header ourselves.
        char hdr[128] = {0};
        if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr) - 1) == ESP_OK
            && strncmp(hdr, "Bearer ", 7) == 0
            && pairing_check(hdr + 7)) {
            authed = true;
        }
    }
    int off = 0;
    off += snprintf(out + off, out_len - off,
                    "<meta name=\"plk-paired\" content=\"%s\">",
                    paired ? "true" : "false");
    if (paired && authed) {
        char tok[64] = {0};
        if (pairing_get_token(tok, sizeof(tok)) == ESP_OK && tok[0]) {
            off += snprintf(out + off, out_len - off,
                            "<meta name=\"plk-token\" content=\"%s\">", tok);
        }
    }
    if (page_id && strcmp(page_id, "compose") == 0) {
        char key[256] = {0};
        if (config_store_get_str(CONFIG_KEY_AI_API_KEY, key, sizeof(key)) == ESP_OK && key[0]) {
            off += snprintf(out + off, out_len - off,
                            "<meta name=\"plk-aikey\" content=\"%s\">", key);
        }
    }
    // Shared inline bootstrapper. Reads ?t= or <meta plk-token>, persists in
    // localStorage, and wraps fetch() to add Authorization on /api/ calls.
    // ~700 bytes; small enough to inline on every page (and keeps everything
    // self-contained — no extra HTTP round-trip for an external auth.js).
    static const char auth_js[] =
        "<script>(function(){"
        "var m=function(n){var e=document.querySelector('meta[name=\"'+n+'\"]');return e?e.content:'';};"
        "var paired=m('plk-paired')==='true';"
        "var u=new URL(location.href);var qt=u.searchParams.get('t');"
        "if(qt){localStorage.setItem('plk_token',qt);u.searchParams.delete('t');history.replaceState({},'',u.toString());}"
        "var mt=m('plk-token');if(mt)localStorage.setItem('plk_token',mt);"
        "var of=window.fetch;window.fetch=function(i,it){it=it||{};"
        "var url=typeof i==='string'?i:(i&&i.url);"
        "var isApi=paired&&url&&url.indexOf('/api/')===0;"
        "if(isApi){"
        "var t=localStorage.getItem('plk_token');"
        "if(t){if(it.headers instanceof Headers){it.headers.set('Authorization','Bearer '+t);}"
        "else{it.headers=Object.assign({},it.headers||{},{'Authorization':'Bearer '+t});}}}"
        // 401 from a paired /api/* call → the stored token was stale (the
        // user re-paired in the macOS app, factory-reset the lamp, etc.).
        // Drop it from localStorage and re-render the banner so the user
        // gets the bootstrap CTA immediately rather than seeing buttons
        // silently do nothing.
        "var pr=of(i,it);"
        "if(isApi){pr.then(function(r){"
        "if(r&&r.status===401){localStorage.removeItem('plk_token');plkBanner();}"
        "return r;}).catch(function(){});}"
        "return pr;};"
        "window.plkWSPath=function(p){var t=localStorage.getItem('plk_token');"
        "return paired&&t?p+(p.indexOf('?')<0?'?':'&')+'token='+encodeURIComponent(t):p;};"
        "window.plkAIKey=function(){return m('plk-aikey');};"
        // Friendlier failure mode when the device is paired but this browser
        // has no token: prepend a banner with a one-liner explaining how to
        // bootstrap. Without it the user just sees on/off do nothing because
        // every /api/* call silently 401s.
        "function plkBanner(){"
        "if(!paired||localStorage.getItem('plk_token'))return;"
        "if(document.getElementById('plk-banner'))return;"
        "var b=document.createElement('div');b.id='plk-banner';"
        "b.style.cssText='position:sticky;top:0;z-index:9999;padding:10px 14px;background:#b85c00;color:#fff;font:14px/1.4 -apple-system,system-ui,sans-serif;text-align:center;box-shadow:0 1px 4px rgba(0,0,0,.3);';"
        "b.innerHTML='\\u26a0 This lamp is paired. Open <b>Show pair-browser QR</b> in the macOS app and scan, or paste the URL into this browser, to grant access.';"
        "document.body.insertBefore(b,document.body.firstChild);}"
        "if(document.readyState==='loading')document.addEventListener('DOMContentLoaded',plkBanner);else plkBanner();"
        "})();</script>";
    off += snprintf(out + off, out_len - off, "%s", auth_js);
}

static void send_html(httpd_req_t *req, const uint8_t *start, const uint8_t *end)
{
    // Inline auth.js shim alone is ~880 bytes; with plk-token + plk-aikey
    // metas we need real headroom or snprintf will silently truncate
    // mid-<script>, leaving the page with an unterminated tag that swallows
    // everything until the next </script> in the body. Pages that didn't
    // depend on inline body JS (e.g. /mqtt) still rendered; ones that did
    // (compose, control, stream, scripts) showed a "black page".
    char meta[2048] = {0};
    build_meta_block(req, NULL, meta, sizeof(meta));
    send_html_with_meta(req, start, end, meta);
}

static void send_html_for(httpd_req_t *req, const uint8_t *start, const uint8_t *end,
                          const char *page_id)
{
    // Inline auth.js shim alone is ~880 bytes; with plk-token + plk-aikey
    // metas we need real headroom or snprintf will silently truncate
    // mid-<script>, leaving the page with an unterminated tag that swallows
    // everything until the next </script> in the body. Pages that didn't
    // depend on inline body JS (e.g. /mqtt) still rendered; ones that did
    // (compose, control, stream, scripts) showed a "black page".
    char meta[2048] = {0};
    build_meta_block(req, page_id, meta, sizeof(meta));
    send_html_with_meta(req, start, end, meta);
}

// Public hook for page handlers in other compilation units (captive_portal,
// ota_update) — delegates to send_html_for so the meta + auth.js injection
// (Phase 9 banner + bearer-token plumbing) only lives in one place.
void portal_send_html(httpd_req_t *req, const uint8_t *start, const uint8_t *end,
                      const char *page_id)
{
    send_html_for(req, start, end, page_id);
}

static esp_err_t style_css_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/css");
    httpd_resp_send(req, (const char *)style_css_start,
                    style_css_end - style_css_start);
    return ESP_OK;
}

/* Shared shade() preview emulator — loaded by /compose and /js. */
static esp_err_t shade_runtime_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/javascript");
    // EMBED_TXTFILES appends a NUL terminator; `_end` points past it. Drop
    // that byte — a trailing '\0' in a <script> source trips the JS parser
    // ("SyntaxError: Invalid character"), unlike HTML/CSS which ignore it.
    size_t len = (size_t)(shade_runtime_js_end - shade_runtime_js_start);
    if (len > 0 && shade_runtime_js_start[len - 1] == 0) len--;
    httpd_resp_send(req, (const char *)shade_runtime_js_start, len);
    return ESP_OK;
}

static esp_err_t control_page_handler(httpd_req_t *req) { send_html(req, control_html_start, control_html_end); return ESP_OK; }
static esp_err_t test_page_handler(httpd_req_t *req) { send_html(req, test_html_start, test_html_end); return ESP_OK; }
static esp_err_t compose_page_handler(httpd_req_t *req) { send_html_for(req, compose_html_start, compose_html_end, "compose"); return ESP_OK; }
static esp_err_t mqtt_page_handler(httpd_req_t *req) { send_html(req, mqtt_html_start, mqtt_html_end); return ESP_OK; }
static esp_err_t js_page_handler(httpd_req_t *req) { send_html(req, js_html_start, js_html_end); return ESP_OK; }

// GET /api/mqtt - MQTT config
static esp_err_t mqtt_api_get_handler(httpd_req_t *req)
{
    if (pairing_http_check(req) != ESP_OK) return ESP_FAIL;
    char host[128] = {0};
    char node[64] = {0};
    config_get_str_or(CONFIG_KEY_MQTT_HOST, host, sizeof(host), "");
    config_get_str_or(CONFIG_KEY_NODE_NAME, node, sizeof(node), CONFIG_PLAIIIN_NODE_NAME);
    int32_t active = config_get_i32_or(CONFIG_KEY_MQTT_ACTIVE, 0);
    int32_t port = config_get_i32_or(CONFIG_KEY_MQTT_PORT, 1883);

    char json[384];
    snprintf(json, sizeof(json),
        "{\"active\":%s,\"host\":\"%s\",\"port\":%ld,\"nodeName\":\"%s\"}",
        active ? "true" : "false", host, (long)port, node);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    return ESP_OK;
}

// POST /mqtt - save MQTT config
static esp_err_t mqtt_save_handler(httpd_req_t *req)
{
    if (pairing_http_check(req) != ESP_OK) return ESP_FAIL;
    int content_len = req->content_len;
    if (content_len <= 0 || content_len > 512) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad request");
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

    // Parse mqtt_active
    char *p = strstr(buf, "mqtt_active=");
    if (p) {
        p += 12;
        int32_t val = (*p == '1') ? 1 : 0;
        config_store_set_i32(CONFIG_KEY_MQTT_ACTIVE, val);
    }

    // Parse mqtt_host
    p = strstr(buf, "mqtt_host=");
    if (p) {
        p += 10;
        char host[128] = {0};
        char *end = strchr(p, '&');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len > sizeof(host) - 1) len = sizeof(host) - 1;
        memcpy(host, p, len);
        if (strlen(host) > 0) config_store_set_str(CONFIG_KEY_MQTT_HOST, host);
    }

    // Parse mqtt_port
    p = strstr(buf, "mqtt_port=");
    if (p) {
        p += 10;
        int32_t port = (int32_t)atoi(p);
        if (port > 0) config_store_set_i32(CONFIG_KEY_MQTT_PORT, port);
    }

    free(buf);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_sendstr(req,
        "<html><body style='background:#131620;color:#c8ccd8;font-family:sans-serif;text-align:center;padding-top:60px'>"
        "<h2 style='color:#7c8cf5'>MQTT Settings Saved</h2>"
        "<p style='color:#6b7084'>Device will reboot in 2 seconds...</p></body></html>");

    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    return ESP_OK;
}

httpd_handle_t http_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = CONFIG_PLAIIIN_HTTP_PORT;
    // Sized with headroom: every page + REST endpoint costs one slot, so this
    // grows whenever we add an /api/* route. Hitting the cap causes silent
    // failures in late registrations (e.g. /api/stop returning 404).
    config.max_uri_handlers = 64;
    config.uri_match_fn = httpd_uri_match_wildcard;
    // JS validation runs mjs_exec on the request thread — needs more stack than default 4 KB.
    config.stack_size = 16 * 1024;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
        return NULL;
    }

    httpd_uri_t style = {
        .uri = "/style.css",
        .method = HTTP_GET,
        .handler = style_css_handler
    };
    httpd_register_uri_handler(server, &style);

    httpd_uri_t shade_runtime = {
        .uri = "/shade-runtime.js",
        .method = HTTP_GET,
        .handler = shade_runtime_handler
    };
    httpd_register_uri_handler(server, &shade_runtime);

    httpd_uri_t api_info = {
        .uri = "/api",
        .method = HTTP_GET,
        .handler = api_info_handler
    };
    httpd_register_uri_handler(server, &api_info);

    httpd_uri_t storage_info = {
        .uri = "/api/storage",
        .method = HTTP_GET,
        .handler = storage_info_handler
    };
    httpd_register_uri_handler(server, &storage_info);

    httpd_uri_t control_page = {
        .uri = "/control",
        .method = HTTP_GET,
        .handler = control_page_handler
    };
    httpd_register_uri_handler(server, &control_page);

    httpd_uri_t test_page = {
        .uri = "/stream",
        .method = HTTP_GET,
        .handler = test_page_handler
    };
    httpd_register_uri_handler(server, &test_page);

    httpd_uri_t compose_page = {
        .uri = "/compose",
        .method = HTTP_GET,
        .handler = compose_page_handler
    };
    httpd_register_uri_handler(server, &compose_page);

    httpd_uri_t mqtt_page = { .uri = "/mqtt", .method = HTTP_GET, .handler = mqtt_page_handler };
    httpd_register_uri_handler(server, &mqtt_page);
    httpd_uri_t js_page = { .uri = "/js", .method = HTTP_GET, .handler = js_page_handler };
    httpd_register_uri_handler(server, &js_page);
    httpd_uri_t mqtt_api = { .uri = "/api/mqtt", .method = HTTP_GET, .handler = mqtt_api_get_handler };
    httpd_register_uri_handler(server, &mqtt_api);
    httpd_uri_t mqtt_save = { .uri = "/mqtt", .method = HTTP_POST, .handler = mqtt_save_handler };
    httpd_register_uri_handler(server, &mqtt_save);

    httpd_uri_t form_prompt_get = { .uri = "/api/form-prompt", .method = HTTP_GET, .handler = form_prompt_get_handler };
    httpd_register_uri_handler(server, &form_prompt_get);
    httpd_uri_t form_prompt_put = { .uri = "/api/form-prompt", .method = HTTP_PUT, .handler = form_prompt_put_handler };
    httpd_register_uri_handler(server, &form_prompt_put);
    httpd_uri_t form_prompt_del = { .uri = "/api/form-prompt", .method = HTTP_DELETE, .handler = form_prompt_delete_handler };
    httpd_register_uri_handler(server, &form_prompt_del);

    captive_portal_register(server);
    light_api_register(server);
    ws_server_register(server);
    ota_update_register(server);

    ESP_LOGI(TAG, "HTTP server started on port %d", CONFIG_PLAIIIN_HTTP_PORT);
    return server;
}

void http_server_stop(httpd_handle_t server)
{
    if (server) {
        httpd_stop(server);
    }
}
