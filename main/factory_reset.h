#pragma once

#include "esp_err.h"
#include <stdbool.h>

/**
 * Factory-reset operations for lamp recovery.
 *
 * `wifi`  — wipes WiFi STA credentials only. The device drops back to AP
 *           mode (or BLE if the BT policy allows) so the user can re-onboard
 *           on a different network.
 * `full`  — personal-data wipe: WiFi creds, the JS file that was last
 *           selected for js mode, and the AI api key. Hardware config
 *           (lamp form/type, pins, pixel-group, orientation, button
 *           mappings) is KEPT so the device boots up still operational
 *           after the reset — just network + key need to be re-supplied.
 *           JS scripts on SPIFFS are kept; only `current_js` (the
 *           last-played pointer) is cleared.
 *
 * If `reboot` is true the function calls esp_restart() before returning;
 * pass false from HTTP handlers so the response can be sent first.
 */
esp_err_t factory_reset_wifi(bool reboot);
esp_err_t factory_reset_full(bool reboot);
