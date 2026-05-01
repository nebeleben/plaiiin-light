#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

/**
 * Phase 9 — pairing token + mode.
 *
 * Two states stored in NVS:
 *   pair_mode  = "unpaired" | "paired"   (default: unpaired)
 *   pair_token = 32 random bytes, base64-url encoded (43 chars)
 *
 * Unpaired: HTTP / BLE behave exactly like pre-1.6 — no auth, anything goes.
 *           Useful for first-boot, demos, and users who don't care.
 * Paired:   HTTP requires `Authorization: Bearer <token>` (or `?token=` for
 *           websocket upgrades). BLE writes require an encrypted/bonded link.
 *           Apps prove possession by replaying the token they got at pair time.
 *
 * Token comparison is constant-time so a remote attacker can't time-leak it
 * by submitting partial guesses.
 */

esp_err_t pairing_init(void);

/** True if the device is currently in paired mode. */
bool pairing_is_paired(void);

/**
 * Verify the supplied bearer token. Returns true if the device is unpaired
 * (no token required) OR if `token` matches the stored secret. Constant-time.
 */
bool pairing_check(const char *token);

/**
 * Generate a fresh token, switch the device to paired mode, and write the
 * token into `out_token` (NUL-terminated, ≤64 chars). Caller's buffer must
 * be at least 64 bytes. Existing token is overwritten — clients holding the
 * old one will start getting 401s.
 *
 * Returns ESP_OK on success, ESP_ERR_INVALID_ARG if buffer too small.
 */
esp_err_t pairing_pair(char *out_token, size_t out_len);

/**
 * Drop pairing: wipe the stored token, flip mode back to "unpaired".
 */
esp_err_t pairing_unpair(void);

/**
 * Read the current token into `out`. Used by the on-device portal page
 * server-render path, which needs to embed the token into HTML pages it
 * serves to authenticated callers. Returns ESP_OK + length, or
 * ESP_ERR_NOT_FOUND when unpaired.
 */
esp_err_t pairing_get_token(char *out, size_t out_len);

#include "esp_http_server.h"

/**
 * Standard guard for HTTP handlers in paired mode. Returns ESP_OK if the
 * request carries a valid `Authorization: Bearer <token>` header (or the
 * device is unpaired); otherwise sends a 401 + returns ESP_FAIL.
 *
 * Handlers should call this as the very first statement:
 *
 *     if (pairing_http_check(req) != ESP_OK) return ESP_FAIL;
 */
esp_err_t pairing_http_check(httpd_req_t *req);

/**
 * Variant for the websocket upgrade path, which can't easily set headers
 * during the handshake. Reads the token from the `?token=` query string.
 */
esp_err_t pairing_ws_check(httpd_req_t *req);
