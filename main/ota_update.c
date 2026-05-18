#include "ota_update.h"
#include "pairing.h"
#include "http_server.h"
#include "esp_ota_ops.h"
#include "esp_log.h"
#include "esp_app_format.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "ota_update";

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

    char json[256];
    snprintf(json, sizeof(json),
        "{\"version\":\"%s\",\"date\":\"%s\",\"time\":\"%s\","
        "\"partition\":\"%s\",\"idf\":\"%s\"}",
        app->version, app->date, app->time,
        running->label, app->idf_ver);

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
