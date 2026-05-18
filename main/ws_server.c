#include "ws_server.h"
#include "led_control.h"
#include "pairing.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "ws_server";
static lamp_mode_t s_mode = LAMP_MODE_API;

// WebSocket binary protocol commands
#define CMD_COLOR_FRAME  0x01
#define CMD_POWER        0x02
#define CMD_CLEAR        0x03

static void handle_ws_binary(uint8_t *data, size_t len)
{
    if (len < 1) return;

    uint8_t cmd = data[0];

    switch (cmd) {
        case CMD_COLOR_FRAME: {
            if (len < 3) return;
            // Auto-enable LEDs without restoring old color
            if (!led_control_is_on()) led_control_enable();
            uint16_t count = (data[1] << 8) | data[2];
            size_t expected = 3 + (count * 3);
            if (len < expected) {
                ESP_LOGW(TAG, "Short color frame: got %d, expected %d", (int)len, (int)expected);
                count = (len - 3) / 3;
            }

            int led_max = led_control_get_count();
            if (count > led_max) count = led_max;

            led_color_t *frame = (led_color_t *)malloc(count * sizeof(led_color_t));
            if (!frame) {
                ESP_LOGE(TAG, "OOM on WS frame (%u LEDs)", (unsigned)count);
                return;
            }
            for (int i = 0; i < count; i++) {
                int offset = 3 + (i * 3);
                frame[i].r = data[offset];
                frame[i].g = data[offset + 1];
                frame[i].b = data[offset + 2];
            }
            // Route based on what the sender is providing:
            //   count == logical area → expand via led_control_set_logical
            //   count == physical count → treat as pre-expanded physical frame
            int lw = led_control_get_logical_w();
            int lh = led_control_get_logical_h();
            int logical_total = lw * lh;
            if (lw > 0 && lh > 0 && (int)count == logical_total && logical_total != led_max) {
                led_control_set_logical(frame, lw, lh);
            } else if (lw > 0 && lh > 0 && (int)count == logical_total) {
                // logical == physical (no grouping) but caller sent logical-sized frame;
                // still use set_logical so serpentine + grouping expansion run uniformly.
                led_control_set_logical(frame, lw, lh);
            } else {
                led_control_set_all(frame, count);
            }
            free(frame);
            break;
        }
        case CMD_POWER: {
            if (len < 2) return;
            led_control_power(data[1] != 0);
            break;
        }
        case CMD_CLEAR: {
            led_control_clear();
            break;
        }
        default:
            ESP_LOGW(TAG, "Unknown WS command: 0x%02x", cmd);
            break;
    }
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        // WebSocket handshake — auth happens here (only chance to set HTTP
        // status before the upgrade), token comes via ?token= query.
        if (pairing_ws_check(req, PL_ROLE_USER) != ESP_OK) return ESP_FAIL;
        ESP_LOGI(TAG, "WebSocket client connected");
        return ESP_OK;
    }

    // Receive WebSocket frame
    httpd_ws_frame_t ws_pkt = {0};
    ws_pkt.type = HTTPD_WS_TYPE_BINARY;

    // Get frame length
    esp_err_t err = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (err != ESP_OK) return err;

    if (ws_pkt.len > 0) {
        ws_pkt.payload = malloc(ws_pkt.len);
        if (!ws_pkt.payload) return ESP_ERR_NO_MEM;

        err = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (err == ESP_OK && ws_pkt.type == HTTPD_WS_TYPE_BINARY) {
            handle_ws_binary(ws_pkt.payload, ws_pkt.len);
        }
        free(ws_pkt.payload);
    }

    return ESP_OK;
}

// /api/mode now lives in light_api.c (it spans api / js / stream).
// ws_server only owns the volatile flag flipped by /api/mode "stream".

esp_err_t ws_server_register(httpd_handle_t server)
{
    httpd_uri_t ws_uri = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = ws_handler,
        .is_websocket = true
    };
    httpd_register_uri_handler(server, &ws_uri);
    return ESP_OK;
}

lamp_mode_t ws_server_get_mode(void)
{
    return s_mode;
}

void ws_server_set_mode(lamp_mode_t mode)
{
    s_mode = mode;
}
