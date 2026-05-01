#include "captive_portal.h"
#include "config_store.h"
#include "wifi.h"
#include "pairing.h"
#include "http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "captive_portal";

// Embedded pages
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");
extern const uint8_t network_html_start[] asm("_binary_network_html_start");
extern const uint8_t network_html_end[]   asm("_binary_network_html_end");

static const char *SAVED_RESPONSE =
    "<html><body style='background:#131620;color:#c8ccd8;font-family:sans-serif;text-align:center;padding-top:60px'>"
    "<h2 style='color:#7c8cf5'>Configuration Saved</h2>"
    "<p style='color:#6b7084'>Device will reboot in 2 seconds...</p></body></html>";

// --- Helpers ---

static void parse_field(const char *buf, const char *key, char *out, size_t out_len)
{
    out[0] = '\0';
    size_t klen = strlen(key);
    const char *p = buf;
    while ((p = strstr(p, key)) != NULL) {
        if (p != buf && *(p - 1) != '&') { p += klen; continue; }
        if (p[klen] != '=') { p += klen; continue; }
        p += klen + 1;
        const char *end = strchr(p, '&');
        size_t vlen = end ? (size_t)(end - p) : strlen(p);
        if (vlen >= out_len) vlen = out_len - 1;
        strncpy(out, p, vlen);
        out[vlen] = '\0';
        return;
    }
}

static void save_str_if_set(const char *buf, const char *form_key, const char *nvs_key)
{
    char val[64] = {0};
    parse_field(buf, form_key, val, sizeof(val));
    if (strlen(val) > 0) config_store_set_str(nvs_key, val);
}

static void save_i32_if_set(const char *buf, const char *form_key, const char *nvs_key)
{
    char val[16] = {0};
    parse_field(buf, form_key, val, sizeof(val));
    if (strlen(val) > 0) config_store_set_i32(nvs_key, (int32_t)atoi(val));
}

static char *recv_body(httpd_req_t *req)
{
    int len = req->content_len;
    if (len <= 0 || len > 1024) return NULL;
    char *buf = malloc(len + 1);
    if (!buf) return NULL;
    int received = 0;
    while (received < len) {
        int ret = httpd_req_recv(req, buf + received, len - received);
        if (ret <= 0) { free(buf); return NULL; }
        received += ret;
    }
    buf[len] = '\0';
    return buf;
}

// --- GET handlers ---

static esp_err_t config_page_get_handler(httpd_req_t *req)
{
    portal_send_html(req, index_html_start, index_html_end, NULL);
    return ESP_OK;
}

static esp_err_t network_page_get_handler(httpd_req_t *req)
{
    portal_send_html(req, network_html_start, network_html_end, NULL);
    return ESP_OK;
}

// --- POST /config: device + LED settings (reboot) ---

static esp_err_t config_save_handler(httpd_req_t *req)
{
    if (pairing_http_check(req) != ESP_OK) return ESP_FAIL;
    char *buf = recv_body(req);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad request");
        return ESP_FAIL;
    }

    save_str_if_set(buf, "node_name", CONFIG_KEY_NODE_NAME);
    save_str_if_set(buf, "vendor_name", CONFIG_KEY_VENDOR_NAME);
    save_i32_if_set(buf, "led_pin", CONFIG_KEY_LED_PIN);
    save_i32_if_set(buf, "led_clk_pin", CONFIG_KEY_LED_CLK_PIN);
    save_i32_if_set(buf, "led_count", CONFIG_KEY_LED_COUNT);
    save_str_if_set(buf, "led_type", CONFIG_KEY_LED_TYPE);
    save_str_if_set(buf, "lamp_type", CONFIG_KEY_LAMP_TYPE);
    save_str_if_set(buf, "lamp_form", CONFIG_KEY_LAMP_FORM);
    save_i32_if_set(buf, "btn_pwr_pin",  CONFIG_KEY_BTN_PWR_PIN);
    save_i32_if_set(buf, "btn_next_pin", CONFIG_KEY_BTN_NEXT_PIN);
    save_i32_if_set(buf, "btn_prev_pin", CONFIG_KEY_BTN_PREV_PIN);

    ESP_LOGI(TAG, "Config saved (device + LED + buttons)");
    free(buf);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, SAVED_RESPONSE);

    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    return ESP_OK;
}

// --- POST /network: WiFi settings (reboot) ---

static esp_err_t network_save_handler(httpd_req_t *req)
{
    if (pairing_http_check(req) != ESP_OK) return ESP_FAIL;
    char *buf = recv_body(req);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad request");
        return ESP_FAIL;
    }

    char ssid[64] = {0}, pass[64] = {0};
    parse_field(buf, "ssid", ssid, sizeof(ssid));
    parse_field(buf, "pass", pass, sizeof(pass));

    if (strlen(ssid) > 0) {
        config_store_set_str(CONFIG_KEY_WIFI_SSID, ssid);
        config_store_set_str(CONFIG_KEY_WIFI_PASS, pass);
        ESP_LOGI(TAG, "Network saved, SSID='%s'", ssid);
    }

    free(buf);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, SAVED_RESPONSE);

    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    return ESP_OK;
}

// --- Captive portal redirect (AP mode -> /network) ---

static esp_err_t captive_redirect_handler(httpd_req_t *req)
{
    if (wifi_get_mode() != PLAIIIN_WIFI_AP) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
        return ESP_OK;
    }
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/network");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// --- Registration ---

esp_err_t captive_portal_register(httpd_handle_t server)
{
    httpd_uri_t config_get = { .uri = "/config", .method = HTTP_GET, .handler = config_page_get_handler };
    httpd_register_uri_handler(server, &config_get);
    httpd_uri_t config_post = { .uri = "/config", .method = HTTP_POST, .handler = config_save_handler };
    httpd_register_uri_handler(server, &config_post);

    httpd_uri_t network_get = { .uri = "/network", .method = HTTP_GET, .handler = network_page_get_handler };
    httpd_register_uri_handler(server, &network_get);
    httpd_uri_t network_post = { .uri = "/network", .method = HTTP_POST, .handler = network_save_handler };
    httpd_register_uri_handler(server, &network_post);

    httpd_uri_t r1 = { .uri = "/generate_204", .method = HTTP_GET, .handler = captive_redirect_handler };
    httpd_register_uri_handler(server, &r1);
    httpd_uri_t r2 = { .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = captive_redirect_handler };
    httpd_register_uri_handler(server, &r2);

    return ESP_OK;
}
