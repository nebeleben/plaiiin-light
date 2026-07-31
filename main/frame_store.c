#include "frame_store.h"
#include "led_control.h"

#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "frame_store";
static const char *FRAME_PATH = "/storage/frame.bin";
static const char MAGIC[4] = {'P', 'L', 'F', 'R'};

esp_err_t frame_store_save(int w, int h, const uint8_t *rgb, size_t len)
{
    if (w <= 0 || h <= 0 || !rgb || len != (size_t)(w * h * 3)) {
        return ESP_ERR_INVALID_ARG;
    }
    FILE *f = fopen(FRAME_PATH, "wb");
    if (!f) return ESP_FAIL;
    uint8_t hdr[8] = {MAGIC[0], MAGIC[1], MAGIC[2], MAGIC[3],
                      (uint8_t)(w & 0xFF), (uint8_t)((w >> 8) & 0xFF),
                      (uint8_t)(h & 0xFF), (uint8_t)((h >> 8) & 0xFF)};
    bool ok = fwrite(hdr, 1, sizeof(hdr), f) == sizeof(hdr)
           && fwrite(rgb, 1, len, f) == len;
    fclose(f);
    if (!ok) { remove(FRAME_PATH); return ESP_FAIL; }
    ESP_LOGI(TAG, "Saved %dx%d frame (%u B)", w, h, (unsigned)len);
    return ESP_OK;
}

esp_err_t frame_store_load(int w, int h, uint8_t **out, size_t *out_len)
{
    if (w <= 0 || h <= 0 || !out || !out_len) return ESP_ERR_INVALID_ARG;
    FILE *f = fopen(FRAME_PATH, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;
    uint8_t hdr[8];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)
        || memcmp(hdr, MAGIC, 4) != 0) {
        fclose(f);
        return ESP_ERR_NOT_FOUND;
    }
    int fw = hdr[4] | (hdr[5] << 8);
    int fh = hdr[6] | (hdr[7] << 8);
    if (fw != w || fh != h) {
        fclose(f);
        ESP_LOGW(TAG, "Stored frame is %dx%d, lamp is %dx%d — ignoring", fw, fh, w, h);
        return ESP_ERR_NOT_FOUND;
    }
    size_t len = (size_t)(w * h * 3);
    uint8_t *buf = malloc(len);
    if (!buf) { fclose(f); return ESP_ERR_NO_MEM; }
    size_t got = fread(buf, 1, len, f);
    fclose(f);
    if (got != len) { free(buf); return ESP_ERR_NOT_FOUND; }
    *out = buf;
    *out_len = len;
    return ESP_OK;
}

bool frame_store_exists(int w, int h)
{
    uint8_t *buf = NULL; size_t len = 0;
    if (frame_store_load(w, h, &buf, &len) != ESP_OK) return false;
    free(buf);
    return true;
}

void frame_store_erase(void)
{
    // remove() on an unmounted VFS (no /storage SPIFFS attached yet) just
    // returns an error harmlessly — nothing to clean up in that case, the
    // caller doesn't need to check.
    remove(FRAME_PATH);
}

esp_err_t frame_store_display(void)
{
    int w = led_control_get_logical_w();
    int h = led_control_get_logical_h();
    uint8_t *buf = NULL; size_t len = 0;
    esp_err_t err = frame_store_load(w, h, &buf, &len);
    if (err != ESP_OK) return err;
    // Transient like the WS stream path — the drawn frame is content, not
    // the user's persisted solid color; last_color must not follow it.
    // led_color_t is exactly 3 packed uint8_t fields (verified in
    // led_control.h — no padding), so the raw RGB buffer casts directly.
    led_control_set_logical((const led_color_t *)buf, w, h);
    free(buf);
    return ESP_OK;
}
