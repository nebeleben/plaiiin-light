#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

/**
 * Saved draw images — a small library of frame_store-format stills on the
 * js_storage SPIFFS partition. Each image is /storage/img-NNN.pli with the
 * exact frame.bin layout: "PLFR" magic + u16 LE width + u16 LE height + raw
 * row-major RGB. NNN is 001..IMAGE_STORE_MAX, lowest free slot on save.
 *
 * The img-NNN id is what scenes and the API refer to. A user-chosen label
 * (optional, at most IMAGE_STORE_LABEL_MAX bytes of UTF-8) sits next to it in
 * /storage/img-NNN.lbl, so renaming never breaks a scene that uses the image.
 *
 * Unlike frame_store, load does NOT require the stored geometry to match the
 * current grid — clients render thumbnails of any geometry; only "show"
 * (image_api) and the carousel effect enforce a match.
 */

#define IMAGE_STORE_MAX      30
#define IMAGE_STORE_NAME_CAP 12   /* "img-NNN" + NUL */
#define IMAGE_STORE_LABEL_MAX 32  /* bytes, without the NUL */

typedef struct {
    char name[IMAGE_STORE_NAME_CAP];
    char label[IMAGE_STORE_LABEL_MAX + 1];   /* "" when none */
    int  w;
    int  h;
} image_store_entry_t;

/** Copy the current frame.bin into the lowest free img-NNN.pli slot.
 *  Writes the new name ("img-007") into out_name. `label` may be NULL or ""
 *  (no label). ESP_ERR_NOT_FOUND when no frame is stored (or it mismatches
 *  the current geometry — same rule as frame_store_load), ESP_ERR_NO_MEM when
 *  all slots are taken, ESP_ERR_INVALID_ARG for a bad label. */
esp_err_t image_store_save_current(const char *label, char *out_name, size_t out_cap);

/** Set (or with NULL / "" clear) one image's label. ESP_ERR_NOT_FOUND for a
 *  missing image, ESP_ERR_INVALID_ARG for a malformed name or label. */
esp_err_t image_store_set_label(const char *name, const char *label);

/** Fill `out` with up to `max` entries, sorted by name (slot order).
 *  Returns the number of entries written. Skips files with a bad magic. */
int image_store_list(image_store_entry_t *out, int max);

/** Load one image. *out is malloc'd (caller frees), *out_len = w*h*3.
 *  Any stored geometry is accepted; the caller decides whether it fits.
 *  ESP_ERR_NOT_FOUND for a missing/invalid name or unreadable file. */
esp_err_t image_store_load(const char *name, int *out_w, int *out_h,
                           uint8_t **out, size_t *out_len);

/** Delete one image. ESP_ERR_INVALID_ARG for a malformed name,
 *  ESP_ERR_NOT_FOUND when the file doesn't exist. */
esp_err_t image_store_delete(const char *name);

/** Remove every img-NNN.pli. Called by factory_reset_full() — a previous
 *  owner's images must not survive a reset. Harmless if SPIFFS isn't
 *  mounted yet (remove() just fails), matching frame_store_erase(). */
void image_store_erase_all(void);
