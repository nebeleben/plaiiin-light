#pragma once
// Scene storage — one small JSON document per named scene on the "storage"
// SPIFFS partition (next to the user's scripts and images), as
// /storage/scene.<name>.json. The document is the scene (see scene_api.c);
// this layer only stores bytes and enforces names/limits.
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#define SCENE_MAX_NAME   32      // [A-Za-z0-9_-]
#define SCENE_MAX_BYTES  2048    // a scene is a few hundred bytes; cap abuse
#define SCENE_MAX_COUNT  32

bool      scene_store_name_valid(const char *name);
bool      scene_store_exists(const char *name);
// Overwrites. ESP_ERR_INVALID_ARG bad name/size, ESP_ERR_NO_MEM at the cap
// (only for a NEW name — overwriting an existing scene is always allowed).
esp_err_t scene_store_write(const char *name, const char *json, size_t len);
// *out is malloc'd and NUL-terminated; caller frees. ESP_ERR_NOT_FOUND if absent.
esp_err_t scene_store_read(const char *name, char **out, size_t *len_out);
esp_err_t scene_store_delete(const char *name);
// Calls cb(name, arg) once per stored scene (unordered). Returns the count.
int       scene_store_list(void (*cb)(const char *name, void *arg), void *arg);
