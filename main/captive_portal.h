#pragma once

#include "esp_http_server.h"

/**
 * Captive portal: redirects all HTTP requests to the config page
 * when in AP mode. Serves the embedded config HTML.
 */

esp_err_t captive_portal_register(httpd_handle_t server);
