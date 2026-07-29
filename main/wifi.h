#pragma once

#include "esp_err.h"
#include "esp_wifi_types.h"
#include <stdbool.h>

/**
 * WiFi manager: connects as STA if credentials are stored,
 * otherwise starts a SoftAP for configuration.
 */

typedef enum {
    PLAIIIN_WIFI_NONE,
    PLAIIIN_WIFI_AP,    // SoftAP mode (config)
    PLAIIIN_WIFI_STA    // Station mode (connected)
} plaiiin_wifi_mode_t;

esp_err_t wifi_init(void);
plaiiin_wifi_mode_t wifi_get_mode(void);
bool wifi_is_connected(void);

/** Tear down the provisioning SoftAP at runtime (no-op unless in AP mode).
 *  Called when a lamp is claimed over BLE so it stops exposing an open
 *  captive portal. */
esp_err_t wifi_provisioning_ap_stop(void);

/** Scan for nearby APs from any WiFi state — during onboarding a STA
 *  interface is borrowed for the scan (APSTA alongside the provisioning AP,
 *  or a temporary STA start when WiFi is off) and the previous state is
 *  restored afterwards. `count` is capacity in / results out; records come
 *  strongest-first. Blocking (~2 s). */
esp_err_t wifi_scan_aps(wifi_ap_record_t *records, uint16_t *count);
