#include "bt_service.h"
#include "config_store.h"
#include "wifi.h"
#include "light_api.h"
#include "js_api.h"
#include "js_player.h"
#include "js_storage.h"
#include "led_control.h"
#include "ws_server.h"
#include "pairing.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "host/ble_att.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/ble.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "bt_service";

// Provided by NimBLE's store/config component (linked in when
// CONFIG_BT_NIMBLE_NVS_PERSIST=y). No public header exports it, so the ESP-IDF
// NimBLE examples extern-declare it exactly like this. Installs the NVS-backed
// keystore so Just Works bonds survive reboots — including the deliberate
// post-claim reboot — instead of living in RAM only.
void ble_store_config_init(void);

#define UPLOAD_MAX_BYTES (48 * 1024)

// --- UUIDs ------------------------------------------------------------------
//
// Random base 4D9B71C0-1F8E-4A1F-9B8C-3D2E1A0E5C0?. Last byte distinguishes
// service vs. each characteristic so a sniffer trace is human-readable.

#define DEF_UUID(name, last) \
    static const ble_uuid128_t name = BLE_UUID128_INIT( \
        0x00 + (last), 0x5C, 0x0E, 0x1A, 0x2E, 0x3D, 0x8C, 0x9B, \
        0x1F, 0x4A, 0x8E, 0x1F, 0xC0, 0x71, 0x9B, 0x4D)

DEF_UUID(svc_uuid,            0x00);
DEF_UUID(chr_device_info,     0x01);
DEF_UUID(chr_wifi_scan,       0x02);
DEF_UUID(chr_wifi_config,     0x03);
DEF_UUID(chr_wifi_status,     0x04);
DEF_UUID(chr_power,           0x05);
DEF_UUID(chr_color,           0x06);
DEF_UUID(chr_mode,            0x07);
DEF_UUID(chr_current_script,  0x08);
DEF_UUID(chr_play_next,       0x09);
DEF_UUID(chr_play_prev,       0x0A);
DEF_UUID(chr_upload_meta,     0x0B);
DEF_UUID(chr_upload_data,     0x0C);
DEF_UUID(chr_upload_status,   0x0D);
DEF_UUID(chr_pair_token,      0x0E);
DEF_UUID(chr_pair_claim,      0x0F);
DEF_UUID(chr_pair_unpair,     0x10);
DEF_UUID(chr_brightness,      0x11);
DEF_UUID(chr_script_params,   0x12);
DEF_UUID(chr_fps,             0x13);
DEF_UUID(chr_fetch_meta,      0x14);
DEF_UUID(chr_fetch_data,      0x15);
DEF_UUID(chr_script_delete,   0x16);
DEF_UUID(chr_script_stop,     0x17);

// --- State ------------------------------------------------------------------

static bool s_running = false;
static uint8_t s_own_addr_type = 0;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_h_wifi_scan = 0;
static uint16_t s_h_wifi_status = 0;
static uint16_t s_h_upload_status = 0;
static uint16_t s_h_current = 0;

// Active upload state. One concurrent upload — BLE bandwidth + RAM make
// concurrent transfers a non-goal.
typedef struct {
    bool   active;
    char   name[64];
    size_t total;
    size_t received;
    char  *buf;
    char   last_status[320];
} upload_state_t;
static upload_state_t s_up = {0};

// --- Utilities --------------------------------------------------------------

static int read_u8(struct os_mbuf *om, uint8_t *out)
{
    return ble_hs_mbuf_to_flat(om, out, 1, NULL);
}

static int copy_mbuf_str(struct os_mbuf *om, char *out, size_t max)
{
    uint16_t n = OS_MBUF_PKTLEN(om);
    if (n >= max) n = max - 1;
    int rc = ble_hs_mbuf_to_flat(om, out, n, NULL);
    if (rc != 0) return rc;
    out[n] = 0;
    return 0;
}

static int respond(struct os_mbuf *om, const void *data, size_t len)
{
    int rc = os_mbuf_append(om, data, len);
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int respond_str(struct os_mbuf *om, const char *s)
{
    return respond(om, s, strlen(s));
}

// Notify a single connected peer that a R/Notify characteristic changed.
// Safe to call before any subscriber attaches — the central will read
// the next time it polls. We set + notify.
static void notify_str(uint16_t handle, const char *s)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(s, strlen(s));
    if (!om) return;
    int rc = ble_gattc_notify_custom(s_conn_handle, handle, om);
    if (rc != 0) ESP_LOGW(TAG, "notify h=%u rc=%d", handle, rc);
}

// --- Pull simple JSON value (string between quotes after "key": ) -----------
//
// The Mac app sends very small JSON (≤ MTU), so we don't pull in cJSON just
// for this. Same scanner pattern as the HTTP handlers.
static bool extract_json_str(const char *src, const char *key, char *out, size_t out_len)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(src, needle);
    if (!p) return false;
    p = strchr(p, ':'); if (!p) return false;
    p = strchr(p, '"'); if (!p) return false;
    p++;
    const char *end = strchr(p, '"');
    if (!end) return false;
    size_t n = end - p;
    if (n >= out_len) n = out_len - 1;
    memcpy(out, p, n);
    out[n] = 0;
    return true;
}

static bool extract_json_int(const char *src, const char *key, int *out)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(src, needle);
    if (!p) return false;
    p = strchr(p, ':'); if (!p) return false;
    *out = atoi(p + 1);
    return true;
}

// --- WiFi scan (BLE-triggered) ---------------------------------------------

static void wifi_scan_task(void *arg)
{
    (void)arg;
    wifi_scan_config_t cfg = {0};
    esp_err_t err = esp_wifi_scan_start(&cfg, true);   // blocking
    if (err != ESP_OK) {
        notify_str(s_h_wifi_scan, "{\"status\":\"error\"}");
        vTaskDelete(NULL);
        return;
    }
    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    if (count > 16) count = 16;   // cap — keep payload BLE-friendly
    wifi_ap_record_t *records = calloc(count, sizeof(wifi_ap_record_t));
    if (!records) { vTaskDelete(NULL); return; }
    esp_wifi_scan_get_ap_records(&count, records);

    char buf[800];
    int off = 0;
    off += snprintf(buf + off, sizeof(buf) - off, "{\"networks\":[");
    for (int i = 0; i < count && off < (int)sizeof(buf) - 64; i++) {
        off += snprintf(buf + off, sizeof(buf) - off,
                        "%s{\"ssid\":\"%s\",\"rssi\":%d,\"sec\":%d}",
                        i ? "," : "", records[i].ssid, records[i].rssi,
                        (int)records[i].authmode);
    }
    off += snprintf(buf + off, sizeof(buf) - off, "]}");
    free(records);
    notify_str(s_h_wifi_scan, buf);
    vTaskDelete(NULL);
}

// --- Characteristic access callbacks ---------------------------------------

static int access_device_info(uint16_t conn, uint16_t attr,
                              struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
    char node[64], vendor[64], api_ver[32], lamp_form[32], lamp_type[32], fw[32];
    config_get_str_or(CONFIG_KEY_NODE_NAME, node, sizeof(node), CONFIG_PLAIIIN_NODE_NAME);
    config_get_str_or(CONFIG_KEY_VENDOR_NAME, vendor, sizeof(vendor), CONFIG_PLAIIIN_VENDOR_NAME);
    config_get_str_or(CONFIG_KEY_API_VERSION, api_ver, sizeof(api_ver), CONFIG_PLAIIIN_API_VERSION);
    config_get_str_or(CONFIG_KEY_LAMP_FORM, lamp_form, sizeof(lamp_form), CONFIG_PLAIIIN_FORM);
    config_get_str_or(CONFIG_KEY_LAMP_TYPE, lamp_type, sizeof(lamp_type), CONFIG_PLAIIIN_LAMP_TYPE);
    snprintf(fw, sizeof(fw), "%s", CONFIG_PLAIIIN_FIRMWARE_VERSION);

    char body[320];
    snprintf(body, sizeof(body),
             "{\"node\":\"%s\",\"vendor\":\"%s\",\"api\":\"%s\",\"fw\":\"%s\","
             "\"lampForm\":\"%s\",\"lampType\":\"%s\"}",
             node, vendor, api_ver, fw, lamp_form, lamp_type);
    return respond_str(ctxt->om, body);
}

static int access_wifi_scan(uint16_t conn, uint16_t attr,
                            struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        // Kick off scan on a small dedicated task (esp_wifi_scan_start is blocking).
        xTaskCreate(wifi_scan_task, "ble_wifi_scan", 4096, NULL, 5, NULL);
        return 0;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return respond_str(ctxt->om, "{\"status\":\"idle\"}");
    }
    return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
}

// Self-restarting tail of the wifi-config BLE write. Runs in its own task so
// the GATT access callback can return immediately and the "saved" notify
// gets out before the radio dies. Brief delay so the host has time to read
// the notify; without it the macOS sheet would just see the disconnect.
static void wifi_config_reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
}

static int access_wifi_config(uint16_t conn, uint16_t attr,
                              struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
    char buf[160];
    if (copy_mbuf_str(ctxt->om, buf, sizeof(buf)) != 0) return BLE_ATT_ERR_UNLIKELY;
    char ssid[64] = {0}, psk[64] = {0};
    if (!extract_json_str(buf, "ssid", ssid, sizeof(ssid))) return BLE_ATT_ERR_INVALID_PDU;
    extract_json_str(buf, "psk", psk, sizeof(psk));   // psk optional (open networks)
    config_store_set_str(CONFIG_KEY_WIFI_SSID, ssid);
    config_store_set_str(CONFIG_KEY_WIFI_PASS, psk);
    notify_str(s_h_wifi_status, "{\"state\":\"saved\",\"hint\":\"rebooting\"}");
    ESP_LOGI(TAG, "BLE wrote WiFi creds for ssid=%s — scheduling reboot", ssid);
    // Defer the actual restart so the GATT response + notify clear the air.
    xTaskCreate(wifi_config_reboot_task, "wifi_cfg_reboot", 2048, NULL, 5, NULL);
    return 0;
}

static int access_wifi_status(uint16_t conn, uint16_t attr,
                              struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
    bool conn_ok = wifi_is_connected();
    char body[96];
    snprintf(body, sizeof(body), "{\"state\":\"%s\"}", conn_ok ? "connected" : "idle");
    return respond_str(ctxt->om, body);
}

static int access_power(uint16_t conn, uint16_t attr,
                        struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint8_t v = 0;
        if (read_u8(ctxt->om, &v) != 0) return BLE_ATT_ERR_INVALID_PDU;
        light_api_apply_power(v != 0);
        return 0;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        uint8_t v = led_control_is_on() ? 1 : 0;
        return respond(ctxt->om, &v, 1);
    }
    return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
}

static int access_color(uint16_t conn, uint16_t attr,
                        struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint8_t rgb[3] = {0};
        uint16_t got = 0;
        if (ble_hs_mbuf_to_flat(ctxt->om, rgb, sizeof(rgb), &got) != 0 || got != 3) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        light_api_apply_color_solid(rgb[0], rgb[1], rgb[2]);
        return 0;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        uint8_t r, g, b;
        js_player_get_base_color(&r, &g, &b);
        uint8_t rgb[3] = {r, g, b};
        return respond(ctxt->om, rgb, 3);
    }
    return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
}

static int access_mode(uint16_t conn, uint16_t attr,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        char m[16] = {0};
        if (copy_mbuf_str(ctxt->om, m, sizeof(m)) != 0) return BLE_ATT_ERR_UNLIKELY;
        if (light_api_apply_mode(m) != 0) return BLE_ATT_ERR_INVALID_PDU;
        return 0;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        char persistent[16] = {0};
        config_get_str_or(CONFIG_KEY_LAMP_MODE, persistent, sizeof(persistent), "api");
        const char *eff = (ws_server_get_mode() == LAMP_MODE_STREAM) ? "stream" : persistent;
        return respond_str(ctxt->om, eff);
    }
    return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
}

// Rolling rendered-FPS of the JS player, mirroring the HTTP /api/state `fps`.
// Read-only string ("%.1f") — 0.0 when nothing is rendering frames.
static int access_fps(uint16_t conn, uint16_t attr,
                      struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
    char buf[16];
    snprintf(buf, sizeof(buf), "%.1f", js_api_get_fps());
    return respond_str(ctxt->om, buf);
}

static int access_brightness(uint16_t conn, uint16_t attr,
                             struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint8_t v = 0;
        if (read_u8(ctxt->om, &v) != 0) return BLE_ATT_ERR_INVALID_PDU;
        led_control_set_brightness(v);
        return 0;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        uint8_t v = led_control_get_brightness();
        return respond(ctxt->om, &v, 1);
    }
    return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
}

// Tunable params for the *currently playing* effect — the BLE analogue of
// /api/js/<current>/params. READ returns the schema JSON ({"items":[...]});
// WRITE applies a partial patch ({"name":value,...}). Buffers are heap, not
// stack: the schema can be ~2 KB and the NimBLE host task stack is only 4 KB.
static int access_script_params(uint16_t conn, uint16_t attr,
                                struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    const char *name = js_api_current_name();
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        if (!name || !name[0]) return respond_str(ctxt->om, "{\"items\":[]}");
        char *buf = malloc(2048);
        if (!buf) return BLE_ATT_ERR_INSUFFICIENT_RES;
        int n = js_api_params_json(name, buf, 2048);
        int rc = (n > 0) ? respond(ctxt->om, buf, (size_t)n)
                         : respond_str(ctxt->om, "{\"items\":[]}");
        free(buf);
        return rc;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        if (!name || !name[0]) return 0;  // nothing playing — no-op
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        char *body = malloc((size_t)len + 1);
        if (!body) return BLE_ATT_ERR_INSUFFICIENT_RES;
        uint16_t got = 0;
        if (ble_hs_mbuf_to_flat(ctxt->om, body, len, &got) != 0) {
            free(body);
            return BLE_ATT_ERR_UNLIKELY;
        }
        body[got] = 0;
        js_api_apply_params(name, body, got);
        free(body);
        return 0;
    }
    return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
}

static int access_current(uint16_t conn, uint16_t attr,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        // Write a script name → load + play it.
        char name[64] = {0};
        if (copy_mbuf_str(ctxt->om, name, sizeof(name)) != 0) return BLE_ATT_ERR_UNLIKELY;
        if (js_api_play(name, JS_DEFAULT_FPS) != ESP_OK) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        notify_str(s_h_current, name);
        return 0;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        /* Phase 35 — js_api_current_name reports whichever runtime is live. */
        const char *cur = js_api_current_name();
        return respond_str(ctxt->om, cur ? cur : "");
    }
    return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
}

static int access_play_step(uint16_t conn, uint16_t attr,
                            struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
    int direction = (int)(intptr_t)arg;
    char chosen[64] = {0};
    esp_err_t err = (direction > 0)
                    ? js_api_play_next(chosen, sizeof(chosen))
                    : js_api_play_prev(chosen, sizeof(chosen));
    if (err != ESP_OK) return BLE_ATT_ERR_UNLIKELY;
    notify_str(s_h_current, chosen);
    return 0;
}

// --- Chunked script upload --------------------------------------------------
//
// Protocol (one transfer at a time):
//   1. Mac writes JSON {"name":"X","total":N} to upload_meta. Resets state,
//      allocates buffer.
//   2. Mac writes raw bytes to upload_data, in MTU-sized chunks. Each write
//      appends to the buffer in order. (Order is guaranteed by ATT.)
//   3. When received==total, server validates + commits to SPIFFS, then
//      notifies upload_status with {"state":"done"|"error",...}.
//   At any time the Mac can read upload_status to see {"state":"in_progress",
//   "received":R,"total":T}.

static void upload_reset(void)
{
    if (s_up.buf) free(s_up.buf);
    memset(&s_up, 0, sizeof(s_up));
}

static void upload_finalise(void)
{
    char err[160] = {0};
    int ok = js_api_write_script(s_up.name, s_up.buf, s_up.received, err, sizeof(err));
    if (ok) {
        snprintf(s_up.last_status, sizeof(s_up.last_status),
                 "{\"state\":\"done\",\"name\":\"%s\"}", s_up.name);
    } else {
        snprintf(s_up.last_status, sizeof(s_up.last_status),
                 "{\"state\":\"error\",\"name\":\"%s\",\"error\":\"%s\"}",
                 s_up.name, err);
    }
    notify_str(s_h_upload_status, s_up.last_status);
    upload_reset();
}

static int access_upload_meta(uint16_t conn, uint16_t attr,
                              struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
    char buf[160] = {0};
    if (copy_mbuf_str(ctxt->om, buf, sizeof(buf)) != 0) return BLE_ATT_ERR_UNLIKELY;
    char name[64] = {0};
    int total = 0;
    if (!extract_json_str(buf, "name", name, sizeof(name))) return BLE_ATT_ERR_INVALID_PDU;
    if (!extract_json_int(buf, "total", &total) || total <= 0 || total > UPLOAD_MAX_BYTES) {
        return BLE_ATT_ERR_INVALID_PDU;
    }
    upload_reset();
    s_up.buf = malloc(total);
    if (!s_up.buf) return BLE_ATT_ERR_INSUFFICIENT_RES;
    s_up.active = true;
    s_up.total = (size_t)total;
    s_up.received = 0;
    snprintf(s_up.name, sizeof(s_up.name), "%s", name);
    snprintf(s_up.last_status, sizeof(s_up.last_status),
             "{\"state\":\"ready\",\"name\":\"%s\",\"total\":%d}", name, total);
    notify_str(s_h_upload_status, s_up.last_status);
    return 0;
}

static int access_upload_data(uint16_t conn, uint16_t attr,
                              struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
    if (!s_up.active) return BLE_ATT_ERR_UNLIKELY;
    uint16_t n = OS_MBUF_PKTLEN(ctxt->om);
    if (s_up.received + n > s_up.total) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    int rc = ble_hs_mbuf_to_flat(ctxt->om, s_up.buf + s_up.received, n, NULL);
    if (rc != 0) return BLE_ATT_ERR_UNLIKELY;
    s_up.received += n;
    if (s_up.received == s_up.total) {
        upload_finalise();
    } else {
        snprintf(s_up.last_status, sizeof(s_up.last_status),
                 "{\"state\":\"in_progress\",\"received\":%u,\"total\":%u}",
                 (unsigned)s_up.received, (unsigned)s_up.total);
    }
    return 0;
}

static int access_upload_status(uint16_t conn, uint16_t attr,
                                struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
    if (s_up.last_status[0]) return respond_str(ctxt->om, s_up.last_status);
    return respond_str(ctxt->om, "{\"state\":\"idle\"}");
}

// --- Chunked fetch: script list + script download --------------------------
//
// The mirror image of the upload path. Large READ payloads — the merged script
// list and a script's full source text — blow past the ~1 KB GATT long-read
// ceiling, so we stream them out in MTU-sized slices.
//   1. Peer WRITES {"op":"list"} or {"op":"read","name":"X"} to fetch_meta.
//      The server builds the entire payload into a heap buffer and remembers
//      its length (or records an error).
//   2. Peer READS fetch_meta to learn {"state":"ready","total":N} (or
//      {"state":"error",...}).
//   3. Peer READS fetch_data repeatedly; each read returns the next slice and
//      advances the cursor, until the peer has accumulated `total` bytes.
// Each slice is kept strictly below ATT_MTU-1 so CoreBluetooth (and any other
// GATT stack) never issues an automatic READ_BLOB follow-up — a blob read would
// re-enter this callback and double-advance the cursor, corrupting the stream.
// One transfer at a time, like upload.
typedef struct {
    bool   active;
    char  *buf;
    size_t total;
    size_t offset;
    char   err[96];   // non-empty → the arming write failed; READ reports it
} fetch_state_t;
static fetch_state_t s_fetch = {0};

static void fetch_reset(void)
{
    if (s_fetch.buf) free(s_fetch.buf);
    memset(&s_fetch, 0, sizeof(s_fetch));
}

static int access_fetch_meta(uint16_t conn, uint16_t attr,
                             struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        char buf[160] = {0};
        if (copy_mbuf_str(ctxt->om, buf, sizeof(buf)) != 0) return BLE_ATT_ERR_UNLIKELY;
        char op[16] = {0};
        if (!extract_json_str(buf, "op", op, sizeof(op))) return BLE_ATT_ERR_INVALID_PDU;
        fetch_reset();
        if (strcmp(op, "list") == 0) {
            char *out = malloc(1400);
            if (!out) return BLE_ATT_ERR_INSUFFICIENT_RES;
            int n = js_api_list_json(out, 1400);
            if (n <= 0) {
                free(out);
                snprintf(s_fetch.err, sizeof(s_fetch.err), "list failed");
                s_fetch.active = true;
                return 0;
            }
            s_fetch.buf = out;
            s_fetch.total = (size_t)n;
            s_fetch.active = true;
            return 0;
        }
        if (strcmp(op, "read") == 0) {
            char name[64] = {0};
            if (!extract_json_str(buf, "name", name, sizeof(name))) return BLE_ATT_ERR_INVALID_PDU;
            char *source = NULL; size_t len = 0;
            esp_err_t err = js_storage_read(name, &source, &len);
            if (err != ESP_OK) {
                snprintf(s_fetch.err, sizeof(s_fetch.err), "%s",
                         err == ESP_ERR_NOT_FOUND ? "not found" : "read failed");
                s_fetch.active = true;
                return 0;
            }
            s_fetch.buf = source;   // ownership transfers to the fetch session
            s_fetch.total = len;
            s_fetch.active = true;
            return 0;
        }
        return BLE_ATT_ERR_INVALID_PDU;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        char st[128];
        if (!s_fetch.active) {
            snprintf(st, sizeof(st), "{\"state\":\"idle\"}");
        } else if (s_fetch.err[0]) {
            snprintf(st, sizeof(st), "{\"state\":\"error\",\"error\":\"%s\"}", s_fetch.err);
        } else {
            snprintf(st, sizeof(st), "{\"state\":\"ready\",\"total\":%u}",
                     (unsigned)s_fetch.total);
        }
        return respond_str(ctxt->om, st);
    }
    return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
}

static int access_fetch_data(uint16_t conn, uint16_t attr,
                             struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
    // No live session (or it errored / already drained) → empty read.
    if (!s_fetch.active || s_fetch.err[0] || !s_fetch.buf) return 0;
    size_t remaining = s_fetch.total - s_fetch.offset;
    if (remaining == 0) return 0;
    // Slice strictly below ATT_MTU-1 so the peer's stack never auto-blobs.
    uint16_t mtu = ble_att_mtu(conn);
    size_t chunk = (mtu > 23) ? (size_t)mtu - 3 : 20;
    if (chunk > 512) chunk = 512;
    size_t n = remaining < chunk ? remaining : chunk;
    int rc = respond(ctxt->om, s_fetch.buf + s_fetch.offset, n);
    if (rc != 0) return rc;
    s_fetch.offset += n;
    if (s_fetch.offset >= s_fetch.total) fetch_reset();   // fully drained
    return 0;
}

static int access_script_delete(uint16_t conn, uint16_t attr,
                                struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
    char name[64] = {0};
    if (copy_mbuf_str(ctxt->om, name, sizeof(name)) != 0) return BLE_ATT_ERR_UNLIKELY;
    esp_err_t err = js_api_delete_script(name);
    if (err == ESP_ERR_INVALID_STATE) return BLE_ATT_ERR_WRITE_NOT_PERMITTED;  // hardcoded
    if (err != ESP_OK) return BLE_ATT_ERR_UNLIKELY;                            // not found / fail
    return 0;
}

static int access_script_stop(uint16_t conn, uint16_t attr,
                              struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
    js_api_stop();
    notify_str(s_h_current, "");   // tell subscribers nothing is playing now
    return 0;
}

// Bonded peers can read pair_token to learn the shared HTTP secret without
// the user having to copy it out of a separate UI. Available only in paired
// mode AND only over an encrypted link (BLE_GATT_CHR_F_READ_ENC flag below).
static int access_pair_token(uint16_t conn, uint16_t attr,
                             struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
    char tok[64] = {0};
    if (pairing_get_token(tok, sizeof(tok)) != ESP_OK) return respond_str(ctxt->om, "");
    return respond_str(ctxt->om, tok);
}

// Claim admin over BLE alone. Writing to this characteristic while unpaired
// mints a fresh admin token (pairing_pair) and switches the lamp to paired
// mode — letting a user own a lamp that has never seen WiFi. The peer then
// reads chr_pair_token to fetch the key. First-claim-wins: once paired, BLE
// claims are rejected (the bonded owner re-reads the token instead). The
// characteristic is WRITE_ENC, so the claim happens over a Just-Works-bonded
// link, not in the clear.
// Self-restarting tail of a successful BLE admin-claim. The reboot rebuilds
// the GATT table in paired mode (control chars become bonded-link-only) and
// re-runs wifi_init (which now skips the provisioning AP). The delay is longer
// than the wifi-config one: the claiming peer reads pair_token immediately
// AFTER its claim write, so the link must stay alive for that round-trip
// before the radio dies.
static void pair_claim_reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

static int access_pair_claim(uint16_t conn, uint16_t attr,
                             struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
    if (pairing_is_paired()) {
        ESP_LOGW(TAG, "BLE claim rejected — lamp already owned");
        return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    }
    char token[64] = {0};
    esp_err_t err = pairing_pair(token, sizeof(token));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BLE claim: pairing_pair err=%s", esp_err_to_name(err));
        return BLE_ATT_ERR_UNLIKELY;
    }
    ESP_LOGI(TAG, "BLE claim: lamp paired over Bluetooth — scheduling reboot");
    // Drop the AP now to cover the pre-reboot window; the reboot then applies
    // the full paired GATT surface and wifi_init's AP-skip cleanly.
    wifi_provisioning_ap_stop();
    xTaskCreate(pair_claim_reboot_task, "pair_reboot", 2048, NULL, 5, NULL);
    return 0;
}

// Release ownership over BLE. The bonded owner writes here to unpair so the app
// can actually let go of a BLE-only lamp on "Remove" — otherwise the lamp stays
// claimed with its only admin token discarded, and re-claim is rejected forever
// (BLE_ATT_ERR_WRITE_NOT_PERMITTED). WRITE_ENC means only a Just-Works-bonded
// peer reaches it. The reboot rebuilds the GATT table in unpaired mode (control
// chars drop back to plaintext) and re-runs wifi_init, which brings the
// provisioning AP back so the now-ownerless lamp can be re-adopted over WiFi too.
static void pair_unpair_reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static int access_pair_unpair(uint16_t conn, uint16_t attr,
                              struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
    if (!pairing_is_paired()) {
        // Nothing to release — report success so a best-effort "unpair on
        // remove" against an already-unpaired lamp doesn't surface an error.
        ESP_LOGI(TAG, "BLE unpair: already unpaired — no-op");
        return 0;
    }
    esp_err_t err = pairing_unpair();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BLE unpair: pairing_unpair err=%s", esp_err_to_name(err));
        return BLE_ATT_ERR_UNLIKELY;
    }
    ESP_LOGI(TAG, "BLE unpair: ownership released — scheduling reboot");
    xTaskCreate(pair_unpair_reboot_task, "pair_unpair_reboot", 2048, NULL, 5, NULL);
    return 0;
}

// --- Service definition -----------------------------------------------------

// Built at start time so we can stamp on the WRITE_ENC / READ_ENC bits when
// the device is in paired mode (forces NimBLE to require a bonded link before
// allowing the operation). The pair_token + pair_claim characteristics are
// always present but are themselves ENC-gated, so they leak nothing to a
// passive scanner while still allowing a first-time BLE claim.
static struct ble_gatt_chr_def s_chrs[25];
static struct ble_gatt_svc_def s_svcs[2];

static void build_service_def(bool paired)
{
    // Common flag stamp: writable chars get WRITE_ENC, readable secrets
    // (current_script, pair_token) get READ_ENC. Public reads (device_info,
    // wifi_status state) stay unencrypted so the Mac can browse before bonding.
    uint16_t W  = BLE_GATT_CHR_F_WRITE  | (paired ? BLE_GATT_CHR_F_WRITE_ENC : 0);
    uint16_t R  = BLE_GATT_CHR_F_READ;
    uint16_t RE = BLE_GATT_CHR_F_READ   | (paired ? BLE_GATT_CHR_F_READ_ENC  : 0);
    uint16_t N  = BLE_GATT_CHR_F_NOTIFY;

    int i = 0;
    s_chrs[i++] = (struct ble_gatt_chr_def){ .uuid = &chr_device_info.u,    .access_cb = access_device_info,    .flags = R };
    s_chrs[i++] = (struct ble_gatt_chr_def){ .uuid = &chr_wifi_scan.u,      .access_cb = access_wifi_scan,      .flags = R | W | N, .val_handle = &s_h_wifi_scan };
    s_chrs[i++] = (struct ble_gatt_chr_def){ .uuid = &chr_wifi_config.u,    .access_cb = access_wifi_config,    .flags = W };
    s_chrs[i++] = (struct ble_gatt_chr_def){ .uuid = &chr_wifi_status.u,    .access_cb = access_wifi_status,    .flags = R | N, .val_handle = &s_h_wifi_status };
    s_chrs[i++] = (struct ble_gatt_chr_def){ .uuid = &chr_power.u,          .access_cb = access_power,          .flags = R | W };
    s_chrs[i++] = (struct ble_gatt_chr_def){ .uuid = &chr_color.u,          .access_cb = access_color,          .flags = R | W };
    s_chrs[i++] = (struct ble_gatt_chr_def){ .uuid = &chr_mode.u,           .access_cb = access_mode,           .flags = R | W };
    s_chrs[i++] = (struct ble_gatt_chr_def){ .uuid = &chr_brightness.u,     .access_cb = access_brightness,     .flags = R | W };
    s_chrs[i++] = (struct ble_gatt_chr_def){ .uuid = &chr_current_script.u, .access_cb = access_current,        .flags = RE | W | N, .val_handle = &s_h_current };
    s_chrs[i++] = (struct ble_gatt_chr_def){ .uuid = &chr_script_params.u,  .access_cb = access_script_params,  .flags = R | W };
    s_chrs[i++] = (struct ble_gatt_chr_def){ .uuid = &chr_fps.u,            .access_cb = access_fps,            .flags = R };
    s_chrs[i++] = (struct ble_gatt_chr_def){ .uuid = &chr_play_next.u,      .access_cb = access_play_step,      .arg = (void *)(intptr_t)+1, .flags = W };
    s_chrs[i++] = (struct ble_gatt_chr_def){ .uuid = &chr_play_prev.u,      .access_cb = access_play_step,      .arg = (void *)(intptr_t)-1, .flags = W };
    s_chrs[i++] = (struct ble_gatt_chr_def){ .uuid = &chr_upload_meta.u,    .access_cb = access_upload_meta,    .flags = W };
    s_chrs[i++] = (struct ble_gatt_chr_def){ .uuid = &chr_upload_data.u,    .access_cb = access_upload_data,    .flags = W };
    s_chrs[i++] = (struct ble_gatt_chr_def){ .uuid = &chr_upload_status.u,  .access_cb = access_upload_status,  .flags = R | N, .val_handle = &s_h_upload_status };
    s_chrs[i++] = (struct ble_gatt_chr_def){ .uuid = &chr_fetch_meta.u,     .access_cb = access_fetch_meta,     .flags = R | W };
    s_chrs[i++] = (struct ble_gatt_chr_def){ .uuid = &chr_fetch_data.u,     .access_cb = access_fetch_data,     .flags = R };
    s_chrs[i++] = (struct ble_gatt_chr_def){ .uuid = &chr_script_delete.u,  .access_cb = access_script_delete,  .flags = W };
    s_chrs[i++] = (struct ble_gatt_chr_def){ .uuid = &chr_script_stop.u,    .access_cb = access_script_stop,    .flags = W };
    // Pair token + claim are always present and always require an encrypted
    // (bonded) link — independent of `paired`. Claim lets a user take
    // ownership over BLE alone; token hands the minted admin key back to the
    // bonded owner. Exposing them while unpaired is safe: the claim is
    // first-wins, and both are ENC-gated so a passive sniffer learns nothing.
    s_chrs[i++] = (struct ble_gatt_chr_def){ .uuid = &chr_pair_token.u, .access_cb = access_pair_token, .flags = BLE_GATT_CHR_F_READ  | BLE_GATT_CHR_F_READ_ENC };
    s_chrs[i++] = (struct ble_gatt_chr_def){ .uuid = &chr_pair_claim.u, .access_cb = access_pair_claim, .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC };
    s_chrs[i++] = (struct ble_gatt_chr_def){ .uuid = &chr_pair_unpair.u, .access_cb = access_pair_unpair, .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC };
    s_chrs[i] = (struct ble_gatt_chr_def){ 0 };

    s_svcs[0] = (struct ble_gatt_svc_def){
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &svc_uuid.u,
        .characteristics = s_chrs,
    };
    s_svcs[1] = (struct ble_gatt_svc_def){ 0 };
}

// --- Advertising + GAP ------------------------------------------------------

static int gap_event(struct ble_gap_event *event, void *arg);

static void advertise(void)
{
    // BLE advert payload budget is 31 bytes. Flags(3) + UUID128(18) +
    // tx_pwr(3) = 24 bytes already; the device name (10–32 bytes) pushes us
    // over and NimBLE silently drops the whole packet — neither macOS nor
    // Android ever saw the lamp. Split: adv carries discoverability + the
    // service UUID (so ScanFilter on the Android side still matches), name
    // moves into the scan response which has its own 31-byte budget.
    struct ble_hs_adv_fields adv_fields = {0};
    adv_fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv_fields.tx_pwr_lvl_is_present = 1;
    adv_fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    adv_fields.uuids128 = (ble_uuid128_t *)&svc_uuid;
    adv_fields.num_uuids128 = 1;
    adv_fields.uuids128_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&adv_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields rc=%d", rc);
        return;
    }

    char node[32];
    config_get_str_or(CONFIG_KEY_NODE_NAME, node, sizeof(node), CONFIG_PLAIIIN_NODE_NAME);
    struct ble_hs_adv_fields rsp_fields = {0};
    rsp_fields.name = (uint8_t *)node;
    rsp_fields.name_len = strlen(node);
    rsp_fields.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGW(TAG, "adv_rsp_set_fields rc=%d (name truncated in scans)", rc);
    }

    struct ble_gap_adv_params params = {0};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0) ESP_LOGE(TAG, "adv_start rc=%d", rc);
    else        ESP_LOGI(TAG, "BLE advertising as %s", node);
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_LINK_ESTAB:
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "BLE connected handle=%d", s_conn_handle);
        } else {
            advertise();
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE disconnected reason=%d", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        upload_reset();
        advertise();
        return 0;
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "BLE MTU=%d", event->mtu.value);
        return 0;
    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "BLE encryption change status=%d", event->enc_change.status);
        // Central GATT stacks cache a *bonded* device's attribute table and only
        // refresh it on a Service Changed indication, so a lamp that grew its
        // table since the bond was created (e.g. an OTA that added the script
        // fetch/delete/stop chars) would stay invisible until a re-pair. Indicate
        // the whole range so the peer re-discovers.
        //
        // BUT only do this for an already-owned lamp reconnecting. During
        // first-time onboarding the peer is *freshly* pairing (no stale cache to
        // bust), and firing Service Changed mid-handshake makes it re-discover in
        // the middle of the bonded pair_claim write/pair_token read — which
        // stalls the claim and loops the link. At this ENC_CHANGE the claim write
        // hasn't run yet, so an onboarding lamp is still unpaired here; gating on
        // paired skips onboarding entirely. The post-claim reboot reconnects as
        // paired and gets the indication then.
        if (event->enc_change.status == 0 && pairing_is_paired()) {
            ble_svc_gatt_changed(0x0001, 0xffff);
        }
        return 0;
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        // The peer (e.g. iOS) still holds a bond we no longer have. Our bonds
        // are RAM-only, so any reboot — including the deliberate one after an
        // admin claim — and any re-flash/NVS re-burn drops them, while the
        // phone keeps its LTK. Without this case NimBLE denies the re-pair, the
        // link never encrypts, and the WRITE_ENC pair-claim comes back as
        // "Encryption is insufficient" (and iOS holds the connection open, so
        // we stop advertising until rebooted). Delete our stale record and tell
        // the host to pair again from scratch — the user never has to "forget"
        // the lamp in their Bluetooth settings.
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0)
            ble_store_util_delete_peer(&desc.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        return 0;
    }
    return 0;
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc) ESP_LOGE(TAG, "ensure_addr rc=%d", rc);
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc) { ESP_LOGE(TAG, "infer_auto rc=%d", rc); return; }
    advertise();
}

static void on_reset(int reason) { ESP_LOGW(TAG, "BLE reset reason=%d", reason); }

static void host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t bt_service_start(void)
{
    if (s_running) return ESP_OK;

    char policy[16] = {0};
    config_get_str_or(CONFIG_KEY_BT_ENABLED, policy, sizeof(policy), "auto");
    if (strcmp(policy, "never") == 0) {
        ESP_LOGI(TAG, "BT disabled by policy=never");
        return ESP_OK;
    }

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init err=%s", esp_err_to_name(err));
        return err;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    // Persist Just Works bonds to NVS so the link stays bonded across reboots
    // (the RAM store would drop every bond, forcing a re-pair after the
    // post-claim reboot). Pairs with the BLE_GAP_EVENT_REPEAT_PAIRING handler:
    // persistence is the happy path, repeat-pairing recovers a wiped/reflashed
    // lamp whose phone still holds a stale key.
    ble_store_config_init();

    // Phase 9 — Just Works bonding. NoInputNoOutput cap means the host
    // (macOS/Android) will NOT prompt for a passkey; it auto-bonds. Bonding is
    // enabled unconditionally (not only when paired) so the encrypted pair
    // claim/token characteristics can be reached even by a brand-new, unpaired
    // lamp: a peer that writes the ENC claim char triggers Just Works bonding,
    // then claims admin over the now-encrypted link. Future upgrade to LE
    // Secure Connections + passkey can flip these caps without surface changes.
    bool paired = pairing_is_paired();
    ble_hs_cfg.sm_io_cap = 3;          // BLE_HS_IO_NO_INPUT_OUTPUT
    ble_hs_cfg.sm_bonding = 1;         // always — enables the ENC pair-claim path
    ble_hs_cfg.sm_mitm = 0;            // no MITM protection (Just Works)
    ble_hs_cfg.sm_sc = 1;              // LE Secure Connections — cheap, modern
    ble_hs_cfg.sm_our_key_dist = 1;    // ENC_KEY
    ble_hs_cfg.sm_their_key_dist = 1;  // ENC_KEY

    ble_svc_gap_init();
    ble_svc_gatt_init();

    build_service_def(paired);
    int rc = ble_gatts_count_cfg(s_svcs);
    if (rc != 0) { ESP_LOGE(TAG, "count_cfg rc=%d", rc); return ESP_FAIL; }
    rc = ble_gatts_add_svcs(s_svcs);
    if (rc != 0) { ESP_LOGE(TAG, "add_svcs rc=%d", rc); return ESP_FAIL; }

    char node[32];
    config_get_str_or(CONFIG_KEY_NODE_NAME, node, sizeof(node), CONFIG_PLAIIIN_NODE_NAME);
    ble_svc_gap_device_name_set(node);

    nimble_port_freertos_init(host_task);
    s_running = true;
    ESP_LOGI(TAG, "BT service started (policy=%s)", policy);
    return ESP_OK;
}

void bt_service_notify_wifi_connected(void)
{
    if (!s_running) return;
    char policy[16] = {0};
    config_get_str_or(CONFIG_KEY_BT_ENABLED, policy, sizeof(policy), "auto");
    if (strcmp(policy, "auto") != 0) return;
    // Tear down to free RAM. NimBLE doesn't have a clean port_deinit on all
    // IDF versions; on those that don't, stop advertising and let it idle.
    ESP_LOGI(TAG, "WiFi up — stopping BLE advertising (auto policy)");
    ble_gap_adv_stop();
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}

bool bt_service_is_running(void)
{
    return s_running;
}
