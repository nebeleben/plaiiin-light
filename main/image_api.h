#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/** Register the /api/images REST endpoints (saved draw images). See
 *  image_store.h for the storage model and portal/api.html for the wire
 *  contract. Reads need PL_ROLE_USER, mutations PL_ROLE_CREATOR — the same
 *  gating as GET/POST /api/frame. */
esp_err_t image_api_register(httpd_handle_t server);
