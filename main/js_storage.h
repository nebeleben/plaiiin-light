#pragma once

#include "esp_err.h"
#include <stddef.h>

/** Mounts the storage SPIFFS partition and exposes JS CRUD helpers. */
esp_err_t js_storage_init(void);

/** Returns a JSON array of names: ["sparkle","sunrise",...] */
esp_err_t js_storage_list(char *out, size_t max_len);

/** Reads a script by name. `out` must be freed by the caller on success. */
esp_err_t js_storage_read(const char *name, char **out, size_t *len_out);

/** Writes a script by name. Overwrites if it exists. */
esp_err_t js_storage_write(const char *name, const char *source, size_t len);

esp_err_t js_storage_remove(const char *name);
