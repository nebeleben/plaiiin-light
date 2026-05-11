// JS script parameters — extracted from `// @param` magic comments in the
// script source and exposed as a `params` object passed as the 5th arg to
// render(). User overrides are persisted as a JSON sidecar in the storage
// partition next to the script's .js file.
//
// Format (one declaration per line, anywhere in the source):
//   // @param NAME MIN..MAX = DEFAULT DESCRIPTION
// Example:
//   // @param density 0..1 = 0.75 Fraction of LEDs animating at peak
//
// Only numeric (float) params are supported in v1; that's enough for the
// fade.js / plasma.js use cases.

#pragma once

#include <stddef.h>
#include <stdbool.h>

#include "esp_err.h"
#include "mjs.h"

#define JS_PARAMS_MAX            8
#define JS_PARAM_NAME_MAX        24
#define JS_PARAM_DESC_MAX        96

typedef struct {
    char  name[JS_PARAM_NAME_MAX];
    float min;
    float max;
    float def;
    float value;        // current value (override or default)
    char  desc[JS_PARAM_DESC_MAX];
} js_param_t;

typedef struct {
    js_param_t items[JS_PARAMS_MAX];
    int        count;
} js_params_schema_t;

/// Scan source for `// @param ...` lines and populate schema. Returns count.
/// Sets value=def for each entry; callers that want overrides should follow
/// up with js_params_load_overrides(name, schema).
int js_params_parse(const char *source, js_params_schema_t *out);

/// Apply persisted overrides from `<name>.params.json`. Missing file is OK
/// (values stay at def). Unknown keys in the file are ignored.
esp_err_t js_params_load_overrides(const char *name, js_params_schema_t *schema);

/// Persist current values to `<name>.params.json`. Only writes parameters
/// whose value differs from def, to keep the sidecar small.
esp_err_t js_params_save_overrides(const char *name, const js_params_schema_t *schema);

/// Merge the JSON object `{"name":value,...}` into schema. Returns the
/// number of parameters actually updated (clamped to [min,max]).
int js_params_apply_json(js_params_schema_t *schema, const char *json, size_t len);

/// Serialize schema to JSON suitable for GET /api/js/<name>/params:
///   {"items":[{"name":"density","min":0,"max":1,"default":0.75,
///              "value":0.5,"description":"…"}, ...]}
/// Writes at most max_len bytes (incl. NUL). Returns bytes written, or -1.
int js_params_to_json(const js_params_schema_t *schema, char *out, size_t max_len);

/// Build an mjs object {name: value, ...} for injection as the 5th render
/// argument. Caller owns nothing; mjs's GC reaps the value with the context.
mjs_val_t js_params_to_mjs(struct mjs *mjs, const js_params_schema_t *schema);

/// Drop the sidecar (no error if missing). Called when the script itself
/// is deleted so we don't leak stale overrides.
void js_params_drop(const char *name);
