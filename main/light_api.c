#include "light_api.h"
#include "led_control.h"
#include "ws_server.h"
#include "config_store.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "light_api";

// POST /api/power  body: {"on":true} or {"on":false}
static esp_err_t power_handler(httpd_req_t *req)
{
    char buf[64] = {0};
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }

    bool on = (strstr(buf, "true") != NULL);
    led_control_power(on);

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

    led_control_set_all(colors, idx);
    free(colors);
    free(buf);

    ESP_LOGI(TAG, "Set %d LED colors", idx);

    httpd_resp_set_type(req, "application/json");
    char resp[64];
    snprintf(resp, sizeof(resp), "{\"status\":\"ok\",\"count\":%d}", idx);
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

// GET /api/brightness -> {"brightness":255}
static esp_err_t brightness_get_handler(httpd_req_t *req)
{
    char resp[32];
    snprintf(resp, sizeof(resp), "{\"brightness\":%d}", led_control_get_brightness());
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

// POST /api/brightness  body: {"brightness":128}
static esp_err_t brightness_set_handler(httpd_req_t *req)
{
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

// GET /api/rotation -> {"rotation":0|90|180|270}
static esp_err_t rotation_get_handler(httpd_req_t *req)
{
    int32_t r = config_get_i32_or(CONFIG_KEY_ROTATION, 0);
    if (r != 0 && r != 90 && r != 180 && r != 270) r = 0;
    char resp[32];
    snprintf(resp, sizeof(resp), "{\"rotation\":%ld}", (long)r);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

// POST /api/rotation {"rotation":N}  — applied client-side only; server just persists.
static esp_err_t rotation_set_handler(httpd_req_t *req)
{
    char buf[64] = {0};
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    char *p = strstr(buf, "rotation");
    int r = -1;
    if (p) { p = strchr(p, ':'); if (p) r = atoi(p + 1); }
    if (r != 0 && r != 90 && r != 180 && r != 270) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "rotation must be 0, 90, 180 or 270");
        return ESP_FAIL;
    }
    config_store_set_i32(CONFIG_KEY_ROTATION, (int32_t)r);
    return rotation_get_handler(req);
}

// GET /api/state -> {"on":true,"color":[r,g,b],"mode":"api"|"stream","brightness":255}
static esp_err_t state_get_handler(httpd_req_t *req)
{
    led_color_t c = led_control_get_last_color();
    bool on = led_control_is_on();
    const char *mode = (ws_server_get_mode() == LAMP_MODE_STREAM) ? "stream" : "api";
    uint8_t brightness = led_control_get_brightness();

    char resp[128];
    snprintf(resp, sizeof(resp),
             "{\"on\":%s,\"color\":[%u,%u,%u],\"mode\":\"%s\",\"brightness\":%u}",
             on ? "true" : "false",
             c.r, c.g, c.b,
             mode,
             brightness);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

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

    httpd_uri_t rotation_get = {
        .uri = "/api/rotation",
        .method = HTTP_GET,
        .handler = rotation_get_handler
    };
    httpd_register_uri_handler(server, &rotation_get);

    httpd_uri_t rotation_set = {
        .uri = "/api/rotation",
        .method = HTTP_POST,
        .handler = rotation_set_handler
    };
    httpd_register_uri_handler(server, &rotation_set);

    return ESP_OK;
}
