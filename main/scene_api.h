#pragma once
#include "esp_http_server.h"
// Scenes — named, saved snapshots of the lamp's user-facing state, recalled
// in one call (see scene_api.c for the document and the activation order).
esp_err_t scene_api_register(httpd_handle_t server);
