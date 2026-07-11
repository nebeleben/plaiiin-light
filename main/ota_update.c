#include "ota_update.h"
#include "pairing.h"
#include "http_server.h"
#include "esp_ota_ops.h"
#include "esp_log.h"
#include "esp_app_format.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdint.h>
#include <string.h>

#ifndef LAMPOS_FORM_STR
#define LAMPOS_FORM_STR ""   /* set by main/CMakeLists.txt via -DLAMPOS_FORM_STR=... */
#endif

#define APP_DESC_MAGIC          0xABCD5432u
#define APP_DESC_PROJ_NAME_OFF  48
#define APP_DESC_PROJ_NAME_LEN  32
#define FORM_PROJECT_PREFIX     "plaiiinlight_os_"

static const char *TAG = "ota_update";

/* Extract the form name embedded in an incoming app binary by scanning the
 * first chunk for esp_app_desc's magic word (0xABCD5432) and reading the
 * project_name field 48 bytes after it. Project name is "plaiiinlight_os_<form>"
 * per Phase 35's per-form build. Returns true on success. */
static bool extract_form_from_buf(const uint8_t *buf, int n,
                                  char *out_form, size_t out_len)
{
    const size_t prefix_len = strlen(FORM_PROJECT_PREFIX);
    for (int i = 0; i + APP_DESC_PROJ_NAME_OFF + APP_DESC_PROJ_NAME_LEN <= n; i++) {
        uint32_t magic;
        memcpy(&magic, buf + i, sizeof(magic));
        if (magic != APP_DESC_MAGIC) continue;
        const char *pn = (const char *)(buf + i + APP_DESC_PROJ_NAME_OFF);
        size_t pn_len = strnlen(pn, APP_DESC_PROJ_NAME_LEN);
        if (pn_len > prefix_len && memcmp(pn, FORM_PROJECT_PREFIX, prefix_len) == 0) {
            int suffix_len = (int)(pn_len - prefix_len);
            snprintf(out_form, out_len, "%.*s", suffix_len, pn + prefix_len);
            return true;
        }
        return false;  /* found app_desc but it's not a per-form binary */
    }
    return false;
}

extern const uint8_t ota_html_start[] asm("_binary_ota_html_start");
extern const uint8_t ota_html_end[]   asm("_binary_ota_html_end");

static esp_err_t ota_page_handler(httpd_req_t *req)
{
    portal_send_html(req, ota_html_start, ota_html_end, NULL);
    return ESP_OK;
}

// GET /api/ota/info - current firmware info
static esp_err_t ota_info_handler(httpd_req_t *req)
{
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();

    char json[352];
    // "target" is CONFIG_IDF_TARGET ("esp32", "esp32c3", …) — the SoC this
    // binary was built for. It matches the release artifact's chip suffix, so
    // an OTA selector can pick the right-architecture binary: both esp32-strip
    // and c3-strip report form "strip", but differ here. Without it a wrong-
    // arch image is still rejected at esp_ota_end (chip_id mismatch), but the
    // device would just keep failing the update instead of being offered the
    // correct binary.
    snprintf(json, sizeof(json),
        "{\"version\":\"%s\",\"date\":\"%s\",\"time\":\"%s\","
        "\"partition\":\"%s\",\"idf\":\"%s\",\"form\":\"%s\",\"target\":\"%s\"}",
        app->version, app->date, app->time,
        running->label, app->idf_ver, LAMPOS_FORM_STR, CONFIG_IDF_TARGET);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    return ESP_OK;
}

// POST /api/ota - receive firmware binary and flash
static esp_err_t ota_upload_handler(httpd_req_t *req)
{
    if (pairing_http_check(req, PL_ROLE_ADMIN) != ESP_OK) return ESP_FAIL;
    ESP_LOGI(TAG, "OTA update started, content length: %d", req->content_len);

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        ESP_LOGE(TAG, "No OTA partition found");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "{\"error\":\"No OTA partition\"}");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Writing to partition '%s' at offset 0x%lx",
             update_partition->label, (long)update_partition->address);

    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "{\"error\":\"OTA begin failed\"}");
        return ESP_FAIL;
    }

    char buf[1024];
    int total_read = 0;
    int content_len = req->content_len;

    /* Phase 35 — read enough of the head to find esp_app_desc and reject a
     * binary built for a different form before we commit any of it to flash.
     * Loop a few recv() calls if the first chunk is short (some clients send
     * small initial packets). 1 KB always covers app_desc, which sits just
     * past the ESP image header / first segment header. */
    int head_have = 0;
    while (head_have < (int)sizeof(buf)) {
        int got = httpd_req_recv(req, buf + head_have, sizeof(buf) - head_have);
        if (got <= 0) {
            if (got == HTTPD_SOCK_ERR_TIMEOUT) continue;
            ESP_LOGE(TAG, "Receive error during head read (%d bytes so far)", head_have);
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "{\"error\":\"Receive failed\"}");
            return ESP_FAIL;
        }
        head_have += got;
        if (head_have >= 512) break;   /* enough to find app_desc */
        if (total_read + head_have >= content_len) break;
    }

    char incoming_form[32] = {0};
    if (!extract_form_from_buf((uint8_t *)buf, head_have, incoming_form, sizeof(incoming_form))) {
        esp_ota_abort(ota_handle);
        ESP_LOGE(TAG, "OTA rejected: couldn't find form-bearing app_desc in head");
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"unrecognised binary (no per-form app_desc)\"}");
        return ESP_FAIL;
    }
    if (strcmp(incoming_form, LAMPOS_FORM_STR) != 0) {
        esp_ota_abort(ota_handle);
        ESP_LOGE(TAG, "OTA rejected: incoming form '%s' != device form '%s'",
                 incoming_form, LAMPOS_FORM_STR);
        char msg[192];
        snprintf(msg, sizeof(msg),
                 "{\"error\":\"form mismatch\",\"deviceForm\":\"%s\",\"incomingForm\":\"%s\"}",
                 LAMPOS_FORM_STR, incoming_form);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, msg);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "OTA form check ok: '%s'", incoming_form);

    /* Form check passed — write the buffered head, then stream the rest. */
    err = esp_ota_write(ota_handle, buf, head_have);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write (head) failed: %s", esp_err_to_name(err));
        esp_ota_abort(ota_handle);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "{\"error\":\"Write failed\"}");
        return ESP_FAIL;
    }
    total_read = head_have;

    while (total_read < content_len) {
        int read_len = httpd_req_recv(req, buf, sizeof(buf));
        if (read_len <= 0) {
            if (read_len == HTTPD_SOCK_ERR_TIMEOUT) continue;
            ESP_LOGE(TAG, "Receive error at %d/%d", total_read, content_len);
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "{\"error\":\"Receive failed\"}");
            return ESP_FAIL;
        }

        err = esp_ota_write(ota_handle, buf, read_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "{\"error\":\"Write failed\"}");
            return ESP_FAIL;
        }

        total_read += read_len;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "{\"error\":\"Validation failed\"}");
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "{\"error\":\"Set boot partition failed\"}");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA update successful! %d bytes written. Rebooting...", total_read);

    httpd_resp_set_type(req, "application/json");
    char resp[128];
    snprintf(resp, sizeof(resp),
        "{\"status\":\"ok\",\"bytes\":%d,\"partition\":\"%s\"}",
        total_read, update_partition->label);
    httpd_resp_sendstr(req, resp);

    // Reboot after response is sent
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

// --- BLE OTA session --------------------------------------------------------
//
// Mirrors ota_upload_handler but fed by chunked GATT writes. We buffer the
// head of the image until we have enough to read its embedded app_desc, run
// the same per-form check, then commit the head and stream the rest straight
// into esp_ota_write. State is a single static session — concurrency isn't a
// goal over BLE.

typedef struct {
    bool              active;
    esp_ota_handle_t  handle;
    const esp_partition_t *partition;
    size_t            total;
    size_t            received;     // bytes committed to flash via esp_ota_write
    uint8_t           head[1024];
    int               head_have;
    bool              form_checked; // true once the head was validated + written
} ota_ble_session_t;

static ota_ble_session_t s_ota = {0};

size_t ota_ble_received(void) { return s_ota.active ? s_ota.received : 0; }

void ota_ble_abort(void)
{
    if (s_ota.active) {
        esp_ota_abort(s_ota.handle);
    }
    memset(&s_ota, 0, sizeof(s_ota));
}

esp_err_t ota_ble_begin(size_t total_len)
{
    ota_ble_abort();
    if (total_len == 0) return ESP_ERR_INVALID_ARG;

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        ESP_LOGE(TAG, "BLE OTA: no update partition");
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &s_ota.handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BLE OTA: esp_ota_begin failed: %s", esp_err_to_name(err));
        return err;
    }
    s_ota.active       = true;
    s_ota.partition    = part;
    s_ota.total        = total_len;
    s_ota.received     = 0;
    s_ota.head_have    = 0;
    s_ota.form_checked = false;
    ESP_LOGI(TAG, "BLE OTA: begin %u bytes -> '%s'",
             (unsigned)total_len, part->label);
    return ESP_OK;
}

// Validate + flush the buffered head once we have enough of it. Returns ESP_OK
// after the head is written, ESP_ERR_INVALID_VERSION on a form/binary mismatch.
static esp_err_t ota_ble_commit_head(void)
{
    char incoming_form[32] = {0};
    if (!extract_form_from_buf(s_ota.head, s_ota.head_have,
                               incoming_form, sizeof(incoming_form))) {
        ESP_LOGE(TAG, "BLE OTA rejected: no per-form app_desc in head");
        return ESP_ERR_INVALID_VERSION;
    }
    if (strcmp(incoming_form, LAMPOS_FORM_STR) != 0) {
        ESP_LOGE(TAG, "BLE OTA rejected: form '%s' != device '%s'",
                 incoming_form, LAMPOS_FORM_STR);
        return ESP_ERR_INVALID_VERSION;
    }
    esp_err_t err = esp_ota_write(s_ota.handle, s_ota.head, s_ota.head_have);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BLE OTA: esp_ota_write(head) failed: %s", esp_err_to_name(err));
        return err;
    }
    s_ota.received     = s_ota.head_have;
    s_ota.form_checked = true;
    ESP_LOGI(TAG, "BLE OTA: form '%s' ok, head %d bytes committed",
             incoming_form, s_ota.head_have);
    return ESP_OK;
}

esp_err_t ota_ble_write(const uint8_t *data, size_t len)
{
    if (!s_ota.active) return ESP_ERR_INVALID_STATE;
    if (len == 0) return ESP_OK;
    if (s_ota.received + (s_ota.form_checked ? 0 : s_ota.head_have) + len > s_ota.total) {
        ESP_LOGE(TAG, "BLE OTA: overrun (%u + %u > %u)",
                 (unsigned)s_ota.received, (unsigned)len, (unsigned)s_ota.total);
        ota_ble_abort();
        return ESP_ERR_INVALID_SIZE;
    }

    // Pre-form-check: accumulate into the head buffer until we have enough to
    // read app_desc (512 B covers it) or the whole (tiny) image has arrived.
    if (!s_ota.form_checked) {
        size_t space = sizeof(s_ota.head) - s_ota.head_have;
        size_t take  = len < space ? len : space;
        memcpy(s_ota.head + s_ota.head_have, data, take);
        s_ota.head_have += take;
        bool have_enough = s_ota.head_have >= 512 ||
                           (size_t)s_ota.head_have >= s_ota.total;
        if (!have_enough) return ESP_OK;   // wait for more head bytes

        esp_err_t err = ota_ble_commit_head();
        if (err != ESP_OK) { ota_ble_abort(); return err; }

        // Any bytes from this write that didn't fit in the head buffer get
        // written straight through now.
        if (take < len) {
            err = esp_ota_write(s_ota.handle, data + take, len - take);
            if (err != ESP_OK) { ota_ble_abort(); return err; }
            s_ota.received += (len - take);
        }
        return ESP_OK;
    }

    esp_err_t err = esp_ota_write(s_ota.handle, data, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BLE OTA: esp_ota_write failed: %s", esp_err_to_name(err));
        ota_ble_abort();
        return err;
    }
    s_ota.received += len;
    return ESP_OK;
}

esp_err_t ota_ble_end(void)
{
    if (!s_ota.active) return ESP_ERR_INVALID_STATE;
    if (!s_ota.form_checked) { ota_ble_abort(); return ESP_ERR_INVALID_STATE; }

    esp_err_t err = esp_ota_end(s_ota.handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BLE OTA: esp_ota_end failed: %s", esp_err_to_name(err));
        memset(&s_ota, 0, sizeof(s_ota));
        return err;
    }
    err = esp_ota_set_boot_partition(s_ota.partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BLE OTA: set_boot_partition failed: %s", esp_err_to_name(err));
        memset(&s_ota, 0, sizeof(s_ota));
        return err;
    }
    ESP_LOGI(TAG, "BLE OTA: complete, %u bytes -> '%s'",
             (unsigned)s_ota.received, s_ota.partition->label);
    memset(&s_ota, 0, sizeof(s_ota));
    return ESP_OK;
}

esp_err_t ota_update_register(httpd_handle_t server)
{
    httpd_uri_t ota_page = {
        .uri = "/ota",
        .method = HTTP_GET,
        .handler = ota_page_handler
    };
    httpd_register_uri_handler(server, &ota_page);

    httpd_uri_t ota_info = {
        .uri = "/api/ota/info",
        .method = HTTP_GET,
        .handler = ota_info_handler
    };
    httpd_register_uri_handler(server, &ota_info);

    httpd_uri_t ota_upload = {
        .uri = "/api/ota",
        .method = HTTP_POST,
        .handler = ota_upload_handler
    };
    httpd_register_uri_handler(server, &ota_upload);

    return ESP_OK;
}
