#pragma once

#include "esp_http_server.h"

/**
 * Starts the HTTP server and registers all handlers.
 */

httpd_handle_t http_server_start(void);
void http_server_stop(httpd_handle_t server);

/**
 * Send a portal HTML page, injecting the Phase 9 pairing meta tags + inline
 * auth.js shim before </head>. Call this from any page handler (config,
 * network, ota, api…) so the page picks up the bearer-token plumbing and
 * the "this lamp is paired" banner without each handler reinventing it.
 *
 * `page_id` is the short logical name ("compose", "config", …) used to
 * select per-page extras (e.g. compose gets <meta plk-aikey>). Pass NULL
 * when no per-page extras are needed.
 */
void portal_send_html(httpd_req_t *req, const uint8_t *start, const uint8_t *end,
                      const char *page_id);
