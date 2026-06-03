#pragma once

#include "esp_err.h"
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
