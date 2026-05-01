#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#include <stddef.h>
#include <stdint.h>

/** Register JS CRUD and playback HTTP endpoints. */
esp_err_t js_api_register(httpd_handle_t server);

// Transport-agnostic helpers — also called from bt_service.c so HTTP and BLE
// share the same playback / upload state machines.

/** Validate + write a script + return ok flag (1 ok, 0 fail). When 0, err_buf
 *  is filled with a short message (validation or storage error). */
int js_api_write_script(const char *name, const char *body, size_t len,
                        char *err_buf, size_t err_len);

/** Load + start playback of script `name`. fps>0 (default 10). */
esp_err_t js_api_play(const char *name, int fps);

/** Step to the next/previous script alphabetically and play it.
 *  Returns ESP_OK + writes chosen name into out_name on success. */
esp_err_t js_api_play_next(char *out_name, size_t out_len);
esp_err_t js_api_play_prev(char *out_name, size_t out_len);

/** Stop playback. */
void js_api_stop(void);
