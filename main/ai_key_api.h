#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/**
 * /api/ai/key — store the AI provider's API key in NVS so it a) survives
 * reboots, b) is shared across browsers/clients hitting the same lamp's
 * /compose page, and c) gets wiped by factory_reset_full().
 *
 *   GET    /api/ai/key  →  {"hasKey":true,"len":51,"key":"sk-…"}
 *   PUT    /api/ai/key  body: raw key (no JSON wrapping)
 *   DELETE /api/ai/key  →  {"status":"ok"}
 *
 * Threat model: the lamp lives on the user's WiFi. Anyone already on the
 * network can hit the API; returning the raw key on GET is the same trust
 * level as exposing /api/color or /api/ota — fine for a hobby device.
 */
esp_err_t ai_key_api_register(httpd_handle_t server);
