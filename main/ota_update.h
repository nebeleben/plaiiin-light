#pragma once

#include "esp_http_server.h"
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

/**
 * OTA firmware update over HTTP.
 * POST /api/ota - receives firmware binary, writes to inactive OTA partition, reboots.
 * GET /ota - serves the upload page.
 */

esp_err_t ota_update_register(httpd_handle_t server);

/**
 * BLE OTA session — the same esp_ota partition flow the HTTP handler runs,
 * driven by chunked GATT writes instead of one streamed POST body. The
 * firmware's per-form app_desc is verified against this device's form before
 * any of the image is committed (same guard as HTTP OTA), so a tower binary
 * can't be flashed onto a cube.
 *
 * Lifecycle: ota_ble_begin(total) → ota_ble_write(...) repeatedly until the
 * full image is delivered → ota_ble_end() sets the boot partition (caller then
 * reboots). ota_ble_abort() tears down a partial session. One session at a
 * time; begin() implicitly aborts any prior session.
 */
esp_err_t ota_ble_begin(size_t total_len);
esp_err_t ota_ble_write(const uint8_t *data, size_t len);
esp_err_t ota_ble_end(void);
void      ota_ble_abort(void);
/** Bytes committed so far in the active session (0 if none). */
size_t    ota_ble_received(void);
