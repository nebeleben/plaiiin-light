#pragma once

#include "esp_err.h"
#include <stdbool.h>
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

/** True if a script with this exact name (no .js suffix) exists on disk. */
bool js_storage_exists(const char *name);

/** Phase 23 — compiled bytecode is stored alongside the .js source as a
 *  separate <name>.bc file. The .js is the source of truth for editing;
 *  the .bc is what the runtime player loads. Writing the .js (via
 *  PUT /api/js/<name>) recompiles and overwrites the .bc.
 *  read_bc allocates *out (caller frees). */
esp_err_t js_storage_write_bc(const char *name, const void *buf, size_t len);
esp_err_t js_storage_read_bc(const char *name, void **out, size_t *len_out);
esp_err_t js_storage_remove_bc(const char *name);

/** True if a compiled <name>.bc exists on disk. */
bool js_storage_bc_exists(const char *name);

/** Collect JS script names (without the .js suffix) into `out`, sorted
 *  alphabetically (strcmp). Caller supplies the buffer.
 *  Returns the number of names actually written, capped at `max_names`. */
int js_storage_collect_sorted(char (*out)[64], int max_names);

/** Returns SPIFFS usage. file_count counts only *.js entries in the root. */
esp_err_t js_storage_info(size_t *total, size_t *used, size_t *file_count);
