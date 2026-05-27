/*
 * Phase 35 — first port of FirePattern.cpp from lampos/adaptations/ into the
 * new hardcoded-effects framework. Algorithm matches the C++ original 1:1:
 *
 *   - A 2D heat buffer of width × height bytes (row-major, bottom = row 0).
 *   - Each "tick" (configurable via the `speed` param) shifts every row UP
 *     and injects a fresh random row at the bottom (`generateLine`).
 *   - Each frame interpolates between the two adjacent rows by the current
 *     within-tick phase (0..1), subtracts a per-cell value_mask × cooling,
 *     and renders the result through HSV→RGB with hue from hue_mask.
 *
 * Where it differs from the C++:
 *   - Time-based (uses time_ms delta) rather than per-frame, so the visual
 *     speed is independent of FPS.
 *   - Generic over physical grid size — masks loaded from NVS at init().
 *   - No baseColor blending today (the original `calculateHueValue` could
 *     hue-shift by the lamp's base color; deferred to a follow-up).
 *
 * Masks are stored in NVS as CSV-of-CSV strings, written by profile-burn.sh
 * from the matching adaptations/fire/fire<W>x<H>.pattern JSON. Shape:
 *     row0c0,row0c1,...,row0cW-1;row1c0,...,;...;rowH-1c0,...
 * Values 0..255 uint8. Whitespace and inter-cell newlines are tolerated.
 */

// @effect fire 30
// @param speed 0.5..10 = 3 Heat rows shifted per second
// @param cooling 0.5..3 = 1 Multiplier on the value-mask subtraction (higher = dimmer)
// @param sparkMin 0..255 = 64 Bottom-row random heat lower bound
// @param sparkMax 0..255 = 255 Bottom-row random heat upper bound
// @param reverse 0..1 = 0 Reverse the physical strip direction (0 = bottom-up, 1 = top-down)

#include "hardcoded_effects.h"
#include "config_store.h"
#include "fire_params.h"     /* generated — exposes float fire_params[] live values + enum */
#include "esp_log.h"
#include "esp_random.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "hc_fire";

#define NVS_KEY_HUE_MASK   "fire_hue_mask"
#define NVS_KEY_VALUE_MASK "fire_value_mask"

/* Hard ceiling so a misconfigured pattern can't blow the heap. 32x32 = 1024
 * cells, way more than any real lamp we have. */
#define FIRE_MAX_CELLS 1024

static int      s_w = 0, s_h = 0;
static uint8_t *s_heat        = NULL;   /* w*h, row-major; row 0 = bottom */
static uint8_t *s_next_row    = NULL;   /* w bytes — the pending row that will become row 0 */
static uint8_t *s_hue_mask    = NULL;   /* w*h, row-major */
static uint8_t *s_value_mask  = NULL;   /* w*h, row-major */
static float    s_phase       = 0.f;    /* 0..1 within the current shift cycle */
static uint32_t s_last_ms     = 0;

/* ------------------------------------------------------------------------
 * CSV-of-CSV mask parsing. Tolerates whitespace anywhere; rows separated
 * by ';', cells by ',', trailing separators OK. Returns true on success.
 * Missing cells default to 0; extra cells are silently dropped. Matches
 * the spirit of FirePattern.cpp:fillMasks() which used Arduino String.
 * ------------------------------------------------------------------------ */
static bool parse_mask(const char *src, uint8_t *dst, int w, int h)
{
    if (!src || !dst) return false;
    memset(dst, 0, (size_t)w * h);
    const char *p = src;
    int row = 0, col = 0, acc = 0, have_digit = 0;
    while (*p && row < h) {
        char c = *p++;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        if (c >= '0' && c <= '9') {
            acc = acc * 10 + (c - '0');
            have_digit = 1;
            continue;
        }
        if (c == ',' || c == ';') {
            if (have_digit && col < w) {
                if (acc > 255) acc = 255;
                dst[row * w + col] = (uint8_t)acc;
                col++;
            }
            acc = 0; have_digit = 0;
            if (c == ';') { row++; col = 0; }
            continue;
        }
        /* unknown char — abort */
        ESP_LOGW(TAG, "mask parse: unexpected char '%c' at row %d col %d", c, row, col);
        return false;
    }
    /* Flush trailing number (last cell without a separator). */
    if (have_digit && row < h && col < w) {
        if (acc > 255) acc = 255;
        dst[row * w + col] = (uint8_t)acc;
    }
    return true;
}

static esp_err_t load_mask_from_nvs(const char *key, uint8_t *dst, int w, int h)
{
    /* NVS strings can be up to ~4 KB; allocate enough for a generous matrix. */
    size_t bufsz = (size_t)w * h * 6 + 32;
    if (bufsz < 256) bufsz = 256;
    char *src = (char *)malloc(bufsz);
    if (!src) return ESP_ERR_NO_MEM;
    config_get_str_or(key, src, bufsz, "");
    if (!src[0]) {
        free(src);
        ESP_LOGW(TAG, "NVS key '%s' empty — mask defaults to all zero", key);
        memset(dst, 0, (size_t)w * h);
        return ESP_OK;
    }
    bool ok = parse_mask(src, dst, w, h);
    free(src);
    return ok ? ESP_OK : ESP_FAIL;
}

/* ------------------------------------------------------------------------
 * HSV→RGB — straight port of FirePattern.cpp::HSVtoRGB. Inputs 0..255.
 * ------------------------------------------------------------------------ */
static void hsv_to_rgb(uint8_t ih, uint8_t is, uint8_t iv,
                       uint8_t *or_, uint8_t *og, uint8_t *ob)
{
    float h = ih / 256.f;
    float s = is / 256.f;
    float v = iv / 256.f;
    float r = 0, g = 0, b = 0;
    if (s == 0.f) {
        r = g = b = v;
    } else {
        h *= 6.f;
        int i = (int)h;
        float f = h - i;
        float p = v * (1.f - s);
        float q = v * (1.f - s * f);
        float t = v * (1.f - s * (1.f - f));
        switch (i) {
            case 0: r = v; g = t; b = p; break;
            case 1: r = q; g = v; b = p; break;
            case 2: r = p; g = v; b = t; break;
            case 3: r = p; g = q; b = v; break;
            case 4: r = t; g = p; b = v; break;
            case 5: r = v; g = p; b = q; break;
            default: r = g = b = 0; break;
        }
    }
    int rr = (int)(r * 255.f);
    int gg = (int)(g * 255.f);
    int bb = (int)(b * 255.f);
    if (rr < 0) rr = 0; else if (rr > 255) rr = 255;
    if (gg < 0) gg = 0; else if (gg > 255) gg = 255;
    if (bb < 0) bb = 0; else if (bb > 255) bb = 255;
    *or_ = (uint8_t)rr;
    *og  = (uint8_t)gg;
    *ob  = (uint8_t)bb;
}

/* ------------------------------------------------------------------------
 * Inject a fresh row of random heat at the bottom. Matches
 * FirePattern.cpp::generateLine which used Arduino random(64, 255) per cell.
 * ------------------------------------------------------------------------ */
static void generate_next_row(void)
{
    int lo = (int)fire_params[FIRE_PARAM_sparkMin];
    int hi = (int)fire_params[FIRE_PARAM_sparkMax];
    if (lo < 0) lo = 0;
    if (lo > 255) lo = 255;
    if (hi < 0) hi = 0;
    if (hi > 255) hi = 255;
    if (hi < lo) { int t = lo; lo = hi; hi = t; }
    int span = (hi - lo) + 1;
    for (int x = 0; x < s_w; x++) {
        s_next_row[x] = (uint8_t)(lo + (esp_random() % (uint32_t)span));
    }
}

/* Shift every row up by one — bottom (row 0) gets the pending next_row. */
static void shift_up(void)
{
    for (int y = s_h - 1; y > 0; y--) {
        memcpy(s_heat + (size_t)y * s_w,
               s_heat + (size_t)(y - 1) * s_w,
               (size_t)s_w);
    }
    memcpy(s_heat, s_next_row, (size_t)s_w);
}

/* ------------------------------------------------------------------------
 * Lifecycle: init / render_frame / deinit
 * ------------------------------------------------------------------------ */

esp_err_t fire_init(int w, int h)
{
    if (w <= 0 || h <= 0) return ESP_ERR_INVALID_ARG;
    if ((size_t)w * h > FIRE_MAX_CELLS) {
        ESP_LOGE(TAG, "grid %dx%d exceeds max cells (%d)", w, h, FIRE_MAX_CELLS);
        return ESP_ERR_INVALID_SIZE;
    }
    s_w = w; s_h = h;

    /* Pick up persisted param overrides written via PUT /api/js/fire/params. */
    fire_params_load_nvs();

    s_heat       = (uint8_t *)calloc((size_t)w * h, 1);
    s_next_row   = (uint8_t *)calloc((size_t)w,     1);
    s_hue_mask   = (uint8_t *)calloc((size_t)w * h, 1);
    s_value_mask = (uint8_t *)calloc((size_t)w * h, 1);
    if (!s_heat || !s_next_row || !s_hue_mask || !s_value_mask) {
        ESP_LOGE(TAG, "OOM allocating fire buffers");
        return ESP_ERR_NO_MEM;
    }

    if (load_mask_from_nvs(NVS_KEY_HUE_MASK,   s_hue_mask,   w, h) != ESP_OK) {
        ESP_LOGW(TAG, "hue mask parse failed — using zeros (all red)");
    }
    if (load_mask_from_nvs(NVS_KEY_VALUE_MASK, s_value_mask, w, h) != ESP_OK) {
        ESP_LOGW(TAG, "value mask parse failed — using zeros (no cooling)");
    }

    s_phase   = 0.f;
    s_last_ms = 0;
    generate_next_row();
    /* Seed the first frame: shift the empty heat once so row 0 has data. */
    shift_up();
    generate_next_row();
    ESP_LOGI(TAG, "init %dx%d", w, h);
    return ESP_OK;
}

void fire_deinit(void)
{
    free(s_heat);       s_heat = NULL;
    free(s_next_row);   s_next_row = NULL;
    free(s_hue_mask);   s_hue_mask = NULL;
    free(s_value_mask); s_value_mask = NULL;
    s_w = s_h = 0;
}

esp_err_t fire_render_frame(int w, int h, led_color_t *frame, uint32_t time_ms)
{
    if (w != s_w || h != s_h) return ESP_ERR_INVALID_STATE;

    /* Advance the within-tick phase based on wall-clock dt × speed. One tick
     * (phase 0 → 1) corresponds to shifting one row upward. */
    float speed = fire_params[FIRE_PARAM_speed];
    if (speed <= 0.f) speed = 0.5f;
    uint32_t dt_ms = (s_last_ms == 0) ? 0 : (time_ms - s_last_ms);
    s_last_ms = time_ms;
    s_phase += (float)dt_ms * 0.001f * speed;
    while (s_phase >= 1.f) {
        shift_up();
        generate_next_row();
        s_phase -= 1.f;
    }
    if (s_phase < 0.f) s_phase = 0.f;
    float cooling = fire_params[FIRE_PARAM_cooling];
    if (cooling < 0.f) cooling = 0.f;
    bool reverse = fire_params[FIRE_PARAM_reverse] >= 0.5f;
    int total = w * h;

    /* For each row y in [1..h-1], interpolate between row y (current) and
     * row y-1 (the one that was below it last tick — going UP). For y=0,
     * interpolate between row 0 and the pending next_row. */
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t a = s_heat[(size_t)y * w + x];
            uint8_t b = (y == 0)
                        ? s_next_row[x]
                        : s_heat[(size_t)(y - 1) * w + x];   /* row below in the buffer */
            /* The original lerps from y → y-1 as phase advances — that's the
             * UP motion (cell y dims toward what was at y-1). Same here. */
            float v = (1.f - s_phase) * (float)a + s_phase * (float)b;
            float mask = (float)s_value_mask[(size_t)y * w + x] * cooling;
            int val = (int)(v - mask);
            if (val < 0) val = 0;
            if (val > 255) val = 255;

            /* Render row top-to-bottom: matrixValue row 0 is the bottom of
             * the flame; on a top-origin matrix we flip y. */
            int rendered_y = (h - 1) - y;
            int idx = rendered_y * w + x;
            if (reverse) idx = total - 1 - idx;
            if (idx < 0 || idx >= total) continue;

            uint8_t hue = s_hue_mask[(size_t)y * w + x];
            hsv_to_rgb(hue, 255, (uint8_t)val,
                       &frame[idx].r, &frame[idx].g, &frame[idx].b);
        }
    }
    return ESP_OK;
}
