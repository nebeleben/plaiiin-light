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

    char json[320];
    snprintf(json, sizeof(json),
        "{\"version\":\"%s\",\"date\":\"%s\",\"time\":\"%s\","
        "\"partition\":\"%s\",\"idf\":\"%s\",\"form\":\"%s\"}",
        app->version, app->date, app->time,
        running->label, app->idf_ver, LAMPOS_FORM_STR);

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
