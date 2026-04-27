#pragma once

#include "esp_http_server.h"

/**
 * OTA firmware update over HTTP.
 * POST /api/ota - receives firmware binary, writes to inactive OTA partition, reboots.
 * GET /ota - serves the upload page.
 */

esp_err_t ota_update_register(httpd_handle_t server);
