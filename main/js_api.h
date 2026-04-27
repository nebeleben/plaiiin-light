#pragma once

#include "esp_http_server.h"

/** Register JS CRUD and playback HTTP endpoints. */
esp_err_t js_api_register(httpd_handle_t server);
