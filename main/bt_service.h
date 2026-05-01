#pragma once

#include "esp_err.h"
#include <stdbool.h>

/**
 * BLE GATT service for plaiiinlight devices.
 *
 * Brings up NimBLE, advertises a 128-bit primary service whose characteristics
 * mirror the HTTP API surface (power, color, mode, play next/prev, current,
 * and a chunked script-upload protocol), plus WiFi provisioning so the macOS
 * app can hand off creds during onboarding.
 *
 * BLE is intentionally a SUBSET of the HTTP API — no websocket pixel streaming
 * (throughput too low) and no large script reads. Use it for setup + simple
 * control; switch to WiFi for everything else.
 *
 * Lifecycle policy (CONFIG_KEY_BT_ENABLED):
 *   "auto"   — bring BT up only when WiFi is unavailable; tear down once
 *              WiFi associates. This is the default.
 *   "always" — keep BT up regardless.
 *   "never"  — never init the stack.
 */

esp_err_t bt_service_start(void);

/** Tell BT that WiFi just came up. In "auto" mode this triggers a teardown
 *  to free ~30 KB RAM and stop advertising. No-op in "always" / "never". */
void bt_service_notify_wifi_connected(void);

/** True if the BT stack is currently initialised. */
bool bt_service_is_running(void);
