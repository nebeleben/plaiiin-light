#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * PlanV3 V2.4 — persistent single-frame store for "frame" mode.
 *
 * One file on the js_storage SPIFFS partition: /storage/frame.bin =
 * "PLFR" magic + u16 LE width + u16 LE height + raw row-major RGB for the
 * LOGICAL pixel grid. The loader validates magic AND that the stored
 * geometry matches the current logical grid — a mismatch (grid config
 * changed since the frame was drawn) reads as "no frame" so the panel
 * never paints garbage.
 */
esp_err_t frame_store_save(int w, int h, const uint8_t *rgb, size_t len);
/** Load the frame for the CURRENT geometry. *out is malloc'd (caller
 *  frees), *out_len = w*h*3. ESP_ERR_NOT_FOUND when absent or mismatched. */
esp_err_t frame_store_load(int w, int h, uint8_t **out, size_t *out_len);
bool frame_store_exists(int w, int h);
/** Load + paint the stored frame onto the panel via the logical-grid path
 *  (grouping/orientation apply). Returns the load error when absent. */
esp_err_t frame_store_display(void);
