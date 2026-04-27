#pragma once

#include "esp_http_server.h"

/**
 * WebSocket server for continuous LED color streaming.
 *
 * Binary protocol (each frame):
 *   Byte 0:    Command (0x01 = color frame, 0x02 = power, 0x03 = clear)
 *   For color frame (0x01):
 *     Bytes 1-2: LED count (big-endian uint16)
 *     Bytes 3+:  RGB data (3 bytes per LED: R, G, B)
 *   For power (0x02):
 *     Byte 1:    0x00 = off, 0x01 = on
 *   For clear (0x03):
 *     No additional data
 *
 * Also supports:
 *   POST /api/mode  {"mode":"stream"} or {"mode":"api"}
 */

typedef enum {
    LAMP_MODE_API,     // Accept individual HTTP API calls
    LAMP_MODE_STREAM   // Accept WebSocket color frames
} lamp_mode_t;

esp_err_t ws_server_register(httpd_handle_t server);
lamp_mode_t ws_server_get_mode(void);
