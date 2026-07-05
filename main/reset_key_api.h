#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/**
 * Factory-reset recovery key (Phase 34).
 *
 * Routes (registered on the main HTTP server):
 *   POST   /api/reset-key         admin + paired → mint key, store its SHA-256,
 *                                  return plaintext ONCE. 409 if unpaired.
 *   DELETE /api/reset-key         admin          → clear the armed key.
 *   GET    /api/reset-key         public         → {"available":bool}.
 *   POST   /api/reset-key/redeem  public         → {"key":"…"}; on match run
 *                                  factory_reset_full(reboot). 401 otherwise.
 *
 * The key is stored only as a hex SHA-256 (CONFIG_KEY_RESET_KEY); the plaintext
 * is shown once at mint time and never persisted or logged.
 */
esp_err_t reset_key_api_register(httpd_handle_t server);
