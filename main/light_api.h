#pragma once

#include "esp_http_server.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * Light API: HTTP endpoints for controlling LEDs.
 *   POST /api/power  {"on": true/false}
 *   POST /api/color  {"colors": [[r,g,b], ...]}
 *
 * The transport-agnostic helpers (light_api_apply_*) are also called from
 * the BLE GATT layer so HTTP and BLE writes share state machines.
 */

esp_err_t light_api_register(httpd_handle_t server);

// Transport-agnostic helpers — call these from any transport (HTTP, BLE).
// They take care of persistent-mode awareness (e.g. /api/color in js mode
// only updates baseColor, never the framebuffer) and NVS persistence.
void light_api_apply_power(bool on);
void light_api_apply_color_solid(uint8_t r, uint8_t g, uint8_t b);
/// 0 = ok, -1 = unknown mode string. Accepts "api" / "js" / "stream".
int  light_api_apply_mode(const char *mode);
/// Current effective mode string ("api" | "js" | "stream") for status
/// reporting — "stream" while a WS session owns the lamp, else the persisted
/// mode. Always NUL-terminates `out`.
void light_api_get_mode(char *out, size_t out_len);

// Stream takeover/restore — called by the WS layer (ws_server.c) when pixel
// frames start/stop arriving, so streaming auto-suspends the JS effect and
// auto-resumes the persisted mode on disconnect, with no client cooperation.
void light_api_enter_stream(void);
void light_api_exit_stream(void);
