#include "ws_server.h"
#include "led_control.h"
#include "wormhole.h"
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

// Phase 29 — application close codes for wormhole stream rejection. See
// docs/wormhole-api.md. The handler closes the socket rather than queueing
// or sending an error frame.
#define WS_CLOSE_FRAME_SIZE   4001  // CMD_COLOR_FRAME pixel count != streamPixels
#define WS_CLOSE_MODE_CHANGED 4002  // POST /api/wormhole changed mode/rings mid-stream

// Result of processing one inbound binary frame: keep the socket open, or
// close it with the given application code.
typedef struct {
    bool  close;
    uint16_t code;
} ws_result_t;

// Send a WebSocket Close frame carrying a 2-byte big-endian application close
// code, then let the caller tear the socket down by returning an error.
static void ws_send_close(httpd_req_t *req, uint16_t code)
{
    uint8_t payload[2] = { (uint8_t)(code >> 8), (uint8_t)(code & 0xFF) };
    httpd_ws_frame_t close_pkt = {0};
    close_pkt.type = HTTPD_WS_TYPE_CLOSE;
    close_pkt.payload = payload;
    close_pkt.len = sizeof(payload);
    httpd_ws_send_frame(req, &close_pkt);
    ESP_LOGW(TAG, "Closing WebSocket with code %u", code);
}

// Process one inbound binary frame. For a wormhole lamp `stream_gen` is the
// stream generation snapshotted when this socket opened — a mismatch means a
// mode/rings change happened mid-stream and the socket must close (4002).
static ws_result_t handle_ws_binary(uint8_t *data, size_t len, uint32_t stream_gen)
{
    ws_result_t res = { false, 0 };
    if (len < 1) return res;

    uint8_t cmd = data[0];

    switch (cmd) {
        case CMD_COLOR_FRAME: {
            if (len < 3) return res;
            // Auto-enable LEDs without restoring old color
            if (!led_control_is_on()) led_control_enable();
            uint16_t count = (data[1] << 8) | data[2];
            size_t expected = 3 + (count * 3);
            if (len < expected) {
                ESP_LOGW(TAG, "Short color frame: got %d, expected %d", (int)len, (int)expected);
                count = (len - 3) / 3;
            }

            bool is_wormhole = wormhole_is_wormhole();

            if (is_wormhole) {
                // A render-mode change while this stream was open closes the
                // socket — the frame size the client is sending is now stale.
                if (stream_gen != wormhole_stream_generation()) {
                    res.close = true;
                    res.code = WS_CLOSE_MODE_CHANGED;
                    return res;
                }
                // The frame must carry exactly streamPixels pixels for the
                // current mode — anything else is a protocol error.
                int stream_pixels = wormhole_render_pixels();
                if ((int)count != stream_pixels) {
                    ESP_LOGW(TAG, "Wormhole frame size mismatch: got %u, expected %d",
                             (unsigned)count, stream_pixels);
                    res.close = true;
                    res.code = WS_CLOSE_FRAME_SIZE;
                    return res;
                }
                led_color_t *render = (led_color_t *)malloc(count * sizeof(led_color_t));
                if (!render) {
                    ESP_LOGE(TAG, "OOM on WS render frame (%u px)", (unsigned)count);
                    return res;
                }
                for (int i = 0; i < count; i++) {
                    int offset = 3 + (i * 3);
                    render[i].r = data[offset];
                    render[i].g = data[offset + 1];
                    render[i].b = data[offset + 2];
                }
                // Tile through the SAME wormhole_expand() the player uses.
                int rings = wormhole_rings();
                bool mirror = (wormhole_mode() == WORMHOLE_MODE_MIRROR);
                int phys_total = rings * 24;
                led_color_t *phys = (led_color_t *)malloc(phys_total * sizeof(led_color_t));
                if (!phys) {
                    ESP_LOGE(TAG, "OOM on WS physical frame (%d px)", phys_total);
                    free(render);
                    return res;
                }
                wormhole_expand(render, count, phys, rings, mirror);
                led_control_set_all(phys, phys_total);
                free(render);
                free(phys);
                break;
            }

            // --- non-wormhole: unchanged behaviour -------------------------
            int led_max = led_control_get_count();
            if (count > led_max) count = led_max;

            led_color_t *frame = (led_color_t *)malloc(count * sizeof(led_color_t));
            if (!frame) {
                ESP_LOGE(TAG, "OOM on WS frame (%u LEDs)", (unsigned)count);
                return res;
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
            if (len < 2) return res;
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
    return res;
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
            // Phase 29 — the wormhole stream generation this socket started
            // on is latched in req->sess_ctx the first time we see a frame.
            // A later wormhole_reload() bumps the global counter; the next
            // frame on this socket then sees the mismatch and closes (4002).
            if (req->sess_ctx == NULL) {
                uint32_t *gen = (uint32_t *)malloc(sizeof(uint32_t));
                if (gen) {
                    *gen = wormhole_stream_generation();
                    req->sess_ctx = gen;
                    req->free_ctx = free;
                }
            }
            uint32_t gen = req->sess_ctx ? *(uint32_t *)req->sess_ctx
                                         : wormhole_stream_generation();
            ws_result_t res = handle_ws_binary(ws_pkt.payload, ws_pkt.len, gen);
            if (res.close) {
                ws_send_close(req, res.code);
                free(ws_pkt.payload);
                // Returning an error tears the socket down.
                return ESP_FAIL;
            }
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
