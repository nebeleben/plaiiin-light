#pragma once

#include "esp_http_server.h"

/**
 * Captive portal: redirects all HTTP requests to the config page
 * when in AP mode. Serves the embedded config HTML.
 */

// Absolute redirect target for AP-mode captive-portal handlers. Must be
// absolute (not "/network") because captive probes (Apple/Android CNA
// mini-browsers, Windows NCSI) resolve a relative Location against the
// probe's own Host header (e.g. captive.apple.com), which re-depends on
// DNS resolving that host — unreliable during onboarding. The AP's fixed
// IP sidesteps that entirely.
#define CAPTIVE_PORTAL_URL "http://192.168.4.1/network"

esp_err_t captive_portal_register(httpd_handle_t server);

/**
 * Registers a catch-all HTTPD_404_NOT_FOUND error handler that redirects
 * unmatched requests to /network while in AP (provisioning) mode, and falls
 * through to a normal 404 otherwise. Costs zero max_uri_handlers slots
 * (registered via httpd_register_err_handler, not a URI handler).
 */
esp_err_t captive_portal_register_err_handler(httpd_handle_t server);
