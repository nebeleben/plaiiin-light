#include "image_store.h"
#include "led_control.h"

#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "image_store";
static const char MAGIC[4] = {'P', 'L', 'F', 'R'};
static const char *FRAME_PATH = "/storage/frame.bin";

/* Slot number (1..IMAGE_STORE_MAX) for a well-formed "img-NNN" name, else -1.
 * Strict: exactly 7 chars, three digits — this is also the path-traversal
 * guard for names arriving from HTTP. */
static int slot_from_name(const char *name)
{
    if (!name || strlen(name) != 7 || strncmp(name, "img-", 4) != 0) return -1;
    for (int i = 4; i < 7; i++) {
        if (name[i] < '0' || name[i] > '9') return -1;
    }
    int n = (name[4] - '0') * 100 + (name[5] - '0') * 10 + (name[6] - '0');
    return (n >= 1 && n <= IMAGE_STORE_MAX) ? n : -1;
}

static void slot_path(int slot, char *out, size_t cap)
{
    snprintf(out, cap, "/storage/img-%03d.pli", slot);
}

/* Read just the header of one slot file. Returns ESP_OK with *w and *h filled,
 * ESP_ERR_NOT_FOUND when absent or the magic is wrong. */
static esp_err_t slot_header(int slot, int *w, int *h)
{
    char path[32];
    slot_path(slot, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;
    uint8_t hdr[8];
    bool ok = fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr)
           && memcmp(hdr, MAGIC, 4) == 0;
    fclose(f);
    if (!ok) return ESP_ERR_NOT_FOUND;
    *w = hdr[4] | (hdr[5] << 8);
    *h = hdr[6] | (hdr[7] << 8);
    return ESP_OK;
}

esp_err_t image_store_save_current(char *out_name, size_t out_cap)
{
    if (!out_name || out_cap < IMAGE_STORE_NAME_CAP) return ESP_ERR_INVALID_ARG;

    /* Validate the source frame the same way frame_store_load does: magic +
     * geometry matches the current logical grid. Copy the whole file raw. */
    int lw = led_control_get_logical_w();
    int lh = led_control_get_logical_h();
    FILE *src = fopen(FRAME_PATH, "rb");
    if (!src) return ESP_ERR_NOT_FOUND;
    uint8_t hdr[8];
    if (fread(hdr, 1, sizeof(hdr), src) != sizeof(hdr)
        || memcmp(hdr, MAGIC, 4) != 0) {
        fclose(src);
        return ESP_ERR_NOT_FOUND;
    }
    int fw = hdr[4] | (hdr[5] << 8);
    int fh = hdr[6] | (hdr[7] << 8);
    if (fw != lw || fh != lh) { fclose(src); return ESP_ERR_NOT_FOUND; }

    int slot = -1;
    for (int s = 1; s <= IMAGE_STORE_MAX; s++) {
        int w, h;
        if (slot_header(s, &w, &h) != ESP_OK) { slot = s; break; }
    }
    if (slot < 0) { fclose(src); return ESP_ERR_NO_MEM; }

    char path[32];
    slot_path(slot, path, sizeof(path));
    FILE *dst = fopen(path, "wb");
    if (!dst) { fclose(src); return ESP_FAIL; }
    bool ok = fwrite(hdr, 1, sizeof(hdr), dst) == sizeof(hdr);
    uint8_t buf[256];
    while (ok) {
        size_t n = fread(buf, 1, sizeof(buf), src);
        if (n == 0) break;
        ok = fwrite(buf, 1, n, dst) == n;
    }
    fclose(src);
    fclose(dst);
    if (!ok) { remove(path); return ESP_FAIL; }

    snprintf(out_name, out_cap, "img-%03d", slot);
    ESP_LOGI(TAG, "Saved %dx%d frame as %s", fw, fh, out_name);
    return ESP_OK;
}

int image_store_list(image_store_entry_t *out, int max)
{
    if (!out || max <= 0) return 0;
    int count = 0;
    for (int s = 1; s <= IMAGE_STORE_MAX && count < max; s++) {
        int w, h;
        if (slot_header(s, &w, &h) != ESP_OK) continue;
        snprintf(out[count].name, IMAGE_STORE_NAME_CAP, "img-%03d", s);
        out[count].w = w;
        out[count].h = h;
        count++;
    }
    return count;
}

esp_err_t image_store_load(const char *name, int *out_w, int *out_h,
                           uint8_t **out, size_t *out_len)
{
    if (!out_w || !out_h || !out || !out_len) return ESP_ERR_INVALID_ARG;
    int slot = slot_from_name(name);
    if (slot < 0) return ESP_ERR_NOT_FOUND;
    char path[32];
    slot_path(slot, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;
    uint8_t hdr[8];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)
        || memcmp(hdr, MAGIC, 4) != 0) {
        fclose(f);
        return ESP_ERR_NOT_FOUND;
    }
    int w = hdr[4] | (hdr[5] << 8);
    int h = hdr[6] | (hdr[7] << 8);
    /* Same sanity ceiling as the hardcoded runtime (HC_RENDER_MAX_PIXELS). */
    if (w <= 0 || h <= 0 || w * h > 1024) { fclose(f); return ESP_ERR_NOT_FOUND; }
    size_t len = (size_t)(w * h * 3);
    uint8_t *buf = malloc(len);
    if (!buf) { fclose(f); return ESP_ERR_NO_MEM; }
    size_t got = fread(buf, 1, len, f);
    fclose(f);
    if (got != len) { free(buf); return ESP_ERR_NOT_FOUND; }
    *out_w = w;
    *out_h = h;
    *out = buf;
    *out_len = len;
    return ESP_OK;
}

esp_err_t image_store_delete(const char *name)
{
    int slot = slot_from_name(name);
    if (slot < 0) return ESP_ERR_INVALID_ARG;
    char path[32];
    slot_path(slot, path, sizeof(path));
    return remove(path) == 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

void image_store_erase_all(void)
{
    for (int s = 1; s <= IMAGE_STORE_MAX; s++) {
        char path[32];
        slot_path(s, path, sizeof(path));
        remove(path);
    }
}
