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
