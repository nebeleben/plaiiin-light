#include "http_server.h"
#include "captive_portal.h"
#include "light_api.h"
#include "ws_server.h"
#include "ota_update.h"
#include "plaiiin_mqtt.h"
#include "config_store.h"
#include "led_control.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "http_server";

// GET /api - device info (reads NVS with Kconfig fallback)
static esp_err_t api_info_handler(httpd_req_t *req)
{
    char node_name[64], vendor[64], api_ver[32];
    char lamp_type[32], lamp_form[32];

    config_get_str_or(CONFIG_KEY_NODE_NAME, node_name, sizeof(node_name), CONFIG_PLAIIIN_NODE_NAME);
    config_get_str_or(CONFIG_KEY_VENDOR_NAME, vendor, sizeof(vendor), CONFIG_PLAIIIN_VENDOR_NAME);
    config_get_str_or(CONFIG_KEY_API_VERSION, api_ver, sizeof(api_ver), CONFIG_PLAIIIN_API_VERSION);

    config_get_str_or(CONFIG_KEY_LAMP_TYPE, lamp_type, sizeof(lamp_type), CONFIG_PLAIIIN_LAMP_TYPE);
    config_get_str_or(CONFIG_KEY_LAMP_FORM, lamp_form, sizeof(lamp_form), CONFIG_PLAIIIN_FORM);

    int32_t led_pin = config_get_i32_or(CONFIG_KEY_LED_PIN, CONFIG_PLAIIIN_LED_PIN);
    int32_t led_clk_pin = config_get_i32_or(CONFIG_KEY_LED_CLK_PIN, CONFIG_PLAIIIN_LED_CLK_PIN);
    int32_t rotation = config_get_i32_or(CONFIG_KEY_ROTATION, 0);
    if (rotation != 0 && rotation != 90 && rotation != 180 && rotation != 270) rotation = 0;
    int led_count = led_control_get_count();

    char led_type_str[16] = {0};
    config_get_str_or(CONFIG_KEY_LED_TYPE, led_type_str, sizeof(led_type_str), CONFIG_PLAIIIN_LED_TYPE);

    int phys_w = led_control_get_physical_w();
    int phys_h = led_control_get_physical_h();
    int logical_w = led_control_get_logical_w();
    int logical_h = led_control_get_logical_h();
    int px_group_w = led_control_get_pixel_group_w();
    int px_group_h = led_control_get_pixel_group_h();

    char json[800];
    snprintf(json, sizeof(json),
        "{\"vendor\":\"%s\",\"apiVersion\":\"%s\",\"firmwareVersion\":\"%s\","
        "\"nodeName\":\"%s\","
        "\"ledPin\":%ld,\"ledClkPin\":%ld,\"ledCount\":%d,\"ledType\":\"%s\","
        "\"lampType\":\"%s\",\"lampForm\":\"%s\","
        "\"physicalW\":%d,\"physicalH\":%d,"
        "\"logicalW\":%d,\"logicalH\":%d,"
        "\"pixelGroupW\":%d,\"pixelGroupH\":%d,"
        "\"rotation\":%ld}",
        vendor, api_ver, CONFIG_PLAIIIN_FIRMWARE_VERSION,
        node_name, (long)led_pin, (long)led_clk_pin, led_count,
        led_type_str, lamp_type, lamp_form,
        phys_w, phys_h, logical_w, logical_h, px_group_w, px_group_h,
        (long)rotation);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    return ESP_OK;
}

// Embedded static files
extern const uint8_t style_css_start[] asm("_binary_style_css_start");
extern const uint8_t style_css_end[]   asm("_binary_style_css_end");
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

static void send_html(httpd_req_t *req, const uint8_t *start, const uint8_t *end)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_send(req, (const char *)start, end - start);
}

static esp_err_t style_css_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/css");
    httpd_resp_send(req, (const char *)style_css_start,
                    style_css_end - style_css_start);
    return ESP_OK;
}

static esp_err_t control_page_handler(httpd_req_t *req) { send_html(req, control_html_start, control_html_end); return ESP_OK; }
static esp_err_t test_page_handler(httpd_req_t *req) { send_html(req, test_html_start, test_html_end); return ESP_OK; }
static esp_err_t compose_page_handler(httpd_req_t *req) { send_html(req, compose_html_start, compose_html_end); return ESP_OK; }
static esp_err_t mqtt_page_handler(httpd_req_t *req) { send_html(req, mqtt_html_start, mqtt_html_end); return ESP_OK; }
static esp_err_t js_page_handler(httpd_req_t *req) { send_html(req, js_html_start, js_html_end); return ESP_OK; }

// GET /api/mqtt - MQTT config
static esp_err_t mqtt_api_get_handler(httpd_req_t *req)
{
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
    config.max_uri_handlers = 40;
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

    httpd_uri_t api_info = {
        .uri = "/api",
        .method = HTTP_GET,
        .handler = api_info_handler
    };
    httpd_register_uri_handler(server, &api_info);

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
