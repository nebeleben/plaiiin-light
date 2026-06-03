#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#include <stddef.h>
#include <stdint.h>

/** Default render fps when a caller doesn't specify one — used by the
 *  chevron next/prev path, BLE play characteristic, on-boot resume, and
 *  /api/power-driven auto-start. The previous default (10) felt smooth
 *  on the old mJS engine because nothing rendered faster than that
 *  anyway; with the PLBC VM the engine ceiling is ~50 fps on a 256-LED
 *  panel, so a default of 30 just lets animations look as fluid as the
 *  hardware allows. Callers that have a UX-level fps knob (Scripts pane)
 *  override this. */
#define JS_DEFAULT_FPS 30

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

/** Phase 41 — like js_api_play but with explicit control over the wormhole
 *  render-mode auto-switch. `autoswitch` true (the js_api_play default) applies
 *  the effect's declared @mode on a wormhole lamp; pass false from the
 *  POST /api/wormhole reinit path so re-launching the current effect doesn't
 *  clobber a mode the user just set by hand. */
esp_err_t js_api_play_ex(const char *name, int fps, bool autoswitch);

/** Step to the next/previous script alphabetically and play it.
 *  Returns ESP_OK + writes chosen name into out_name on success. */
esp_err_t js_api_play_next(char *out_name, size_t out_len);
esp_err_t js_api_play_prev(char *out_name, size_t out_len);

/** Stop playback. */
void js_api_stop(void);

/* Phase 35 — runtime-agnostic playback accessors. Callers that just want
 * "what's playing right now and how fast" should use these instead of the
 * PLBC-specific js_player_* getters: they automatically report whichever
 * runtime (PLBC or hardcoded) currently has the stage. */
const char *js_api_current_name(void);
float       js_api_get_fps(void);
bool        js_api_is_running(void);

/** Tunable-parameter schema for a named effect (hardcoded effect or stored
 *  .bc) as JSON: {"items":[{"name":..,"min":..,"max":..,"value":..},...]}.
 *  Returns bytes written (>0), 0 for no params, or <0 on error. Shared by the
 *  HTTP `/api/js/<name>/params` GET and the BLE script-params characteristic. */
int js_api_params_json(const char *name, char *buf, size_t cap);

/** Apply a partial params patch ({"name":value,...}) to a named effect:
 *  persists to storage (for .bc scripts) and pushes the values live if it's
 *  the running effect. Returns ESP_OK, ESP_ERR_NOT_FOUND, or ESP_FAIL. */
esp_err_t js_api_apply_params(const char *name, const char *body, size_t len);
