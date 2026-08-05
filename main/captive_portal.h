#pragma once

#include "esp_http_server.h"

/**
 * Captive portal: redirects all HTTP requests to the config page
 * when in AP mode. Serves the embedded config HTML.
 */

esp_err_t captive_portal_register(httpd_handle_t server);

/**
 * Registers a catch-all HTTPD_404_NOT_FOUND error handler that redirects
 * unmatched requests to /network while in AP (provisioning) mode, and falls
 * through to a normal 404 otherwise. Costs zero max_uri_handlers slots
 * (registered via httpd_register_err_handler, not a URI handler).
 */
esp_err_t captive_portal_register_err_handler(httpd_handle_t server);
