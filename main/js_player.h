#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/**
 * Runs user JS scripts on the device. Scripts define a `render(frame, w, h)`
 * function returning an array of [r,g,b] triples (one per logical pixel).
 * The player evaluates the script on a dedicated FreeRTOS task at the
 * configured FPS and pushes frames via led_control_set_all.
 */

esp_err_t js_player_init(void);

/** Validate + load a script from source. Returns an error message on failure
 *  (pointer valid until the next call). */
esp_err_t js_player_validate(const char *source, const char **err_out);

/** Start playback from the given source. */
esp_err_t js_player_start(const char *source, int fps);

/** Stop playback if running. */
void js_player_stop(void);

/** True if playback is active. */
bool js_player_is_running(void);

/** Name of the currently loaded script (NULL if none). */
const char *js_player_current_name(void);
void js_player_set_current_name(const char *name);

/** Base color passed as the 4th arg to render(). Updated by /api/color so
 *  scripts can tint themselves to whatever Home Assistant just set. */
void js_player_set_base_color(uint8_t r, uint8_t g, uint8_t b);
void js_player_get_base_color(uint8_t *r, uint8_t *g, uint8_t *b);

/** Apply a params JSON override to the currently-running script.
 *  Returns the number of parameters successfully updated.
 *  Safe to call from an HTTP handler — values are swapped under the player
 *  lock so the next frame picks them up atomically. */
int js_player_apply_params_json(const char *json, size_t len);

/** Currently-running script's params as JSON. Returns bytes written, or 0
 *  if no script is loaded. */
int js_player_dump_params_json(char *out, size_t max_len);

/** Phase 22 — Rendered-frames-per-second over a rolling 5 s window. Returns
 *  0.0 when no script has produced a frame in the window (idle / stopped /
 *  just-started). Sampled and the timestamp ring is reset on every
 *  js_player_start so a previous run's tail doesn't leak in. */
float js_player_get_fps(void);

/** Phase 22 — Diagnostic: run a pure-C render loop (no JS engine involved).
 *  Used to measure the LED-driver / scheduler ceiling independent of JS.
 *  mode == 0: fillSolid with baseColor.
 *  mode == 1: stateless fade equivalent to fade.js.
 *  Stops any current player first. fps controls the requested rate. */
esp_err_t js_player_start_cbench(int mode, int fps);
