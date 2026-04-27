#pragma once

#include "esp_http_server.h"

/**
 * Starts the HTTP server and registers all handlers.
 */

httpd_handle_t http_server_start(void);
void http_server_stop(httpd_handle_t server);
