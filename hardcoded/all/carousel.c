/*
 * Carousel — cycles through the saved draw images (image_store) with a
 * crossfade. Form-independent: lives in hardcoded/all/ and only touches the
 * logical grid, so it compiles into every firmware.
 *
 * Playback model: a monotonically advancing slot index derived from time_ms
 * and the dwell tune. On each slot change the current pixels become the
 * "previous" buffer and the next image is loaded; the first `fade` seconds
 * of a slot blend previous → current. The image list is rescanned at every
 * cycle wrap (and every ~2 s while empty) so images saved or deleted while
 * the carousel runs join/leave without restarting playback. Images whose
 * stored geometry doesn't match the current grid are skipped at scan time.
 */

// @effect carousel 30
// @param dwell 1..60 = 5 Seconds each saved image is shown
// @param fade 0..5 = 1 Crossfade seconds between images
// @param shuffle 0..1 = 0 Random order (1) instead of slot order (0)

#include "hardcoded_effects.h"
#include "image_store.h"
#include "carousel_params.h"   /* generated — float carousel_params[] + enum */
#include "esp_log.h"
#include "esp_random.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "hc_carousel";

#define CAROUSEL_MAX_PIXELS 1024

static int      s_w = 0, s_h = 0;
static char     s_names[IMAGE_STORE_MAX][IMAGE_STORE_NAME_CAP];
static int      s_order[IMAGE_STORE_MAX];   /* playback order → index into s_names */
static int      s_count = 0;
static int32_t  s_slot = -1;                /* last slot rendered; -1 = none yet */
static uint8_t *s_cur = NULL;               /* w*h*3 — image of the current slot */
static uint8_t *s_prev = NULL;              /* w*h*3 — image of the previous slot */
static bool     s_have_prev = false;
static uint32_t s_last_scan_ms = 0;

static void shuffle_order(void)
{
    for (int i = s_count - 1; i > 0; i--) {
        int j = (int)(esp_random() % (uint32_t)(i + 1));
        int t = s_order[i];
        s_order[i] = s_order[j];
        s_order[j] = t;
    }
}

/* Refresh s_names/s_order from the store, keeping only geometry matches. */
static void rescan(void)
{
    image_store_entry_t entries[IMAGE_STORE_MAX];
    int n = image_store_list(entries, IMAGE_STORE_MAX);
    s_count = 0;
    for (int i = 0; i < n; i++) {
        if (entries[i].w != s_w || entries[i].h != s_h) continue;
        memcpy(s_names[s_count], entries[i].name, IMAGE_STORE_NAME_CAP);
        s_order[s_count] = s_count;
        s_count++;
    }
    if (carousel_params[CAROUSEL_PARAM_shuffle] >= 0.5f) shuffle_order();
}

/* Load the image for playback position `pos` into `dst` (w*h*3). Fills black
 * when the file vanished between scan and load. */
static void load_into(int pos, uint8_t *dst)
{
    size_t need = (size_t)s_w * s_h * 3;
    int iw = 0, ih = 0;
    uint8_t *buf = NULL;
    size_t len = 0;
    if (pos < s_count
        && image_store_load(s_names[s_order[pos]], &iw, &ih, &buf, &len) == ESP_OK) {
        if (iw == s_w && ih == s_h && len == need) {
            memcpy(dst, buf, need);
            free(buf);
            return;
        }
        free(buf);
    }
    memset(dst, 0, need);
}

esp_err_t carousel_init(int w, int h)
{
    if (w <= 0 || h <= 0) return ESP_ERR_INVALID_ARG;
    if ((size_t)w * h > CAROUSEL_MAX_PIXELS) return ESP_ERR_INVALID_SIZE;
    s_w = w;
    s_h = h;
    carousel_params_load_nvs();
    size_t bytes = (size_t)w * h * 3;
    s_cur = (uint8_t *)calloc(bytes, 1);
    s_prev = (uint8_t *)calloc(bytes, 1);
    if (!s_cur || !s_prev) {
        free(s_cur);
        free(s_prev);
        s_cur = NULL;
        s_prev = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_slot = -1;
    s_have_prev = false;
    s_last_scan_ms = 0;
    rescan();
    ESP_LOGI(TAG, "init %dx%d — %d matching image(s)", w, h, s_count);
    return ESP_OK;
}

void carousel_deinit(void)
{
    free(s_cur);  s_cur = NULL;
    free(s_prev); s_prev = NULL;
    s_w = s_h = 0;
    s_count = 0;
}

esp_err_t carousel_render_frame(int w, int h, led_color_t *frame, uint32_t time_ms)
{
    if (w != s_w || h != s_h || !s_cur || !s_prev) return ESP_ERR_INVALID_STATE;
    int total = w * h;

    if (s_count == 0) {
        /* Nothing to show — black frame, but keep looking for new saves. */
        if (time_ms - s_last_scan_ms > 2000) {
            s_last_scan_ms = time_ms;
            rescan();
            if (s_count > 0) s_slot = -1;   /* start fresh on first image */
        }
        memset(frame, 0, (size_t)total * sizeof(led_color_t));
        return ESP_OK;
    }

    float dwell_s = carousel_params[CAROUSEL_PARAM_dwell];
    if (dwell_s < 1.f) dwell_s = 1.f;
    uint32_t dwell_ms = (uint32_t)(dwell_s * 1000.f);
    int32_t slot = (int32_t)(time_ms / dwell_ms);

    if (slot != s_slot) {
        /* Advancing (or first frame). Cycle wrap → rescan so new/deleted
         * images are reflected once per full pass. */
        if (s_slot >= 0 && s_count > 0 && slot % s_count == 0) rescan();
        if (s_count == 0) return carousel_render_frame(w, h, frame, time_ms);
        if (s_slot >= 0) {
            memcpy(s_prev, s_cur, (size_t)total * 3);
            s_have_prev = true;
        }
        load_into((int)(slot % s_count), s_cur);
        s_slot = slot;
    }

    /* Blend previous → current over the first `fade` seconds of the slot. */
    float fade_s = carousel_params[CAROUSEL_PARAM_fade];
    float alpha = 1.f;
    if (s_have_prev && fade_s > 0.01f) {
        float t = (float)(time_ms % dwell_ms) * 0.001f;
        alpha = t / fade_s;
        if (alpha >= 1.f) { alpha = 1.f; s_have_prev = false; }
    }
    for (int i = 0; i < total; i++) {
        if (alpha >= 1.f) {
            frame[i].r = s_cur[i * 3];
            frame[i].g = s_cur[i * 3 + 1];
            frame[i].b = s_cur[i * 3 + 2];
        } else {
            frame[i].r = (uint8_t)((1.f - alpha) * s_prev[i * 3]     + alpha * s_cur[i * 3]);
            frame[i].g = (uint8_t)((1.f - alpha) * s_prev[i * 3 + 1] + alpha * s_cur[i * 3 + 1]);
            frame[i].b = (uint8_t)((1.f - alpha) * s_prev[i * 3 + 2] + alpha * s_cur[i * 3 + 2]);
        }
    }
    return ESP_OK;
}
