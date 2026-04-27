#pragma once

#include "esp_http_server.h"

/**
 * Light API: HTTP endpoints for controlling LEDs.
 *   POST /api/power  {"on": true/false}
 *   POST /api/color  {"colors": [[r,g,b], ...]}
 */

esp_err_t light_api_register(httpd_handle_t server);
