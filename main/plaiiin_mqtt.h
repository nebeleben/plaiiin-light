#pragma once

#include "esp_err.h"

/**
 * MQTT client for PlaiiinLightOS.
 *
 * Topics (node_name from config):
 *   plaiiinlight/<node>/power/set    - receive "on" or "off"
 *   plaiiinlight/<node>/power/get    - publishes "on" or "off"
 *   plaiiinlight/<node>/color/set    - receive hex color "ff0000"
 *   plaiiinlight/<node>/color/get    - publishes hex color "ff0000"
 *   plaiiinlight/<node>/brightness/set - receive 0-255
 *   plaiiinlight/<node>/brightness/get - publishes 0-255
 *   plaiiinlight/<node>/status       - LWT: "online" / "offline"
 */

esp_err_t mqtt_client_start(void);
void mqtt_client_stop(void);
void mqtt_client_publish_state(void);

/** Stop the running client, re-read NVS settings, start again. Used by
 *  /api/mqtt POST so toggling MQTT on/off or changing host/port doesn't
 *  require a full device reboot. Safe when the client isn't running. */
esp_err_t mqtt_client_restart(void);
