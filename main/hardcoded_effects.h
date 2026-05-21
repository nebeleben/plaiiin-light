#pragma once

#include "esp_err.h"
#include "led_control.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Hardcoded effects (Phase 35).
 *
 * A second flavour of "effect", compiled directly into firmware in plain C.
 * Used for things the PLBC JS sandbox can't do — multi-frame state machines,
 * HSV math with lookup tables, anything that needs more than per-pixel
 * stateless math. Effects live under lampos/hardcoded/{FORM}/{name}.c and
 * are pulled into the build by main/CMakeLists.txt for the form named in
 * LAMPOS_FORM. The build also runs scripts/gen_hardcoded.py over those
 * sources to emit build/generated/<effect>_params.h and a static registry.
 *
 * To clients these effects look just like JS effects: they appear in
 * GET /api/js, accept GET/PUT /api/js/<name>/params, and play via
 * POST /api/play {"file":"<name>"}. js_api dispatches by name lookup —
 * hardcoded first, PLBC fallback. PLBC scripts may not collide with a
 * hardcoded name (PUT /api/js/<name> returns 409 if a hardcoded <name>
 * exists, so a user can't shadow a built-in).
 *
 * Per-frame contract: render_frame() fills the entire w*h led_color_t frame
 * the runtime hands it. Param overrides are persisted as a JSON string in
 * NVS under hc_p_<effect> (key length limited by NVS), applied on init.
 */

typedef struct hardcoded_effect_s hardcoded_effect_t;

struct hardcoded_effect_s {
    /* Effect name as clients see it. Must be unique across the registry
     * AND must not collide with any user-uploaded JS script (enforced by
     * js_api's write_handler). */
    const char *name;

    /* Default frames-per-second when /api/play doesn't specify. */
    int default_fps;

    /* Called once when playback starts. (w, h) is the logical render grid
     * (matches what led_control_set_logical expects). Reads any NVS-backed
     * state the effect needs. Return non-ESP_OK to abort playback. */
    esp_err_t (*init)(int w, int h);

    /* Per-frame whole-grid renderer. `frame` is w*h pixels owned by the
     * runtime; the effect fills RGB values. time_ms is monotonic wall-clock
     * elapsed since this playback started (matches PLBC's `time` builtin). */
    esp_err_t (*render_frame)(int w, int h, led_color_t *frame, uint32_t time_ms);

    /* JSON-serialise the current live param values into `out`. Same shape
     * as PLBC's GET /api/js/<name>/params — {"items":[{"name":...,"min":...,
     * "max":...,"default":...,"value":...,"desc":...},...]}. Returns bytes
     * written, or 0 on error. */
    int (*get_params_json)(char *out, size_t max_len);

    /* Parse a partial JSON {"name":value,...} and apply to live params.
     * Returns number of params successfully updated. */
    int (*apply_params_json)(const char *json, size_t len);

    /* Optional teardown. May be NULL. */
    void (*deinit)(void);
};

/* Registry accessors. The static array is emitted by gen_hardcoded.py into
 * build/generated/hardcoded_registry.c and may be empty (a form with no
 * hardcoded effects compiles fine). */
size_t                      hardcoded_effect_count(void);
const hardcoded_effect_t   *hardcoded_effect_at(size_t i);
const hardcoded_effect_t   *hardcoded_effect_find(const char *name);

/* Runtime — mirrors the js_player API surface so js_api can dispatch
 * uniformly. */
esp_err_t   hardcoded_runtime_start(const hardcoded_effect_t *eff, int fps);
void        hardcoded_runtime_stop(void);
bool        hardcoded_runtime_is_running(void);
const char *hardcoded_runtime_current_name(void);
float       hardcoded_runtime_get_fps(void);
