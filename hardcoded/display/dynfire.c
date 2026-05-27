/*
 * Phase 35 — second hardcoded effect on top of the FirePattern engine.
 *
 * Same algorithm as fire.c (rising heat matrix, value-mask cooling, HSV→RGB)
 * but the hue is derived dynamically from the lamp's baseColor — the colour
 * set via /api/color / the macOS picker. The hue_mask values (0..~25 in the
 * shipped fire8x8.pattern) act as small per-cell HUE OFFSETS from that base,
 * giving "fire in the colour you picked" instead of fire's hardcoded red.
 *
 * Implements the C++ FirePattern's baseColorMode=true branch with a cleaner
 * wrap (the original `(usedBase - hueMaskValue) - 255` was a subtle bug).
 *
 * Shares the fire_hue_mask + fire_value_mask NVS data with fire — those are
 * physical-lamp config, not per-effect. profile-burn writes them once.
 */

// @effect dynfire 30
// @param speed 0.5..10 = 3 Heat rows shifted per second
// @param cooling 0.5..3 = 1 Multiplier on the value-mask subtraction (higher = dimmer)
// @param sparkMin 0..255 = 64 Bottom-row random heat lower bound
// @param sparkMax 0..255 = 255 Bottom-row random heat upper bound
// @param hueRange 0..2 = 1 Multiplier on the hue offset (0 = single tone, 1 = mask as-is, 2 = wider)
// @param reverse 0..1 = 0 Reverse the physical strip direction (0 = bottom-up, 1 = top-down)

#include "hardcoded_effects.h"
#include "config_store.h"
#include "dynfire_params.h"
#include "js_player.h"
#include "esp_log.h"
#include "esp_random.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "hc_dynfire";

/* Reuses the same NVS strings fire.c reads — masks describe the lamp's
 * physical fire shape, not which effect renders it. */
#define NVS_KEY_HUE_MASK   "fire_hue_mask"
#define NVS_KEY_VALUE_MASK "fire_value_mask"

#define DYNFIRE_MAX_CELLS 1024

static int      s_w = 0, s_h = 0;
static uint8_t *s_heat        = NULL;
static uint8_t *s_next_row    = NULL;
static uint8_t *s_hue_mask    = NULL;
static uint8_t *s_value_mask  = NULL;
static float    s_phase       = 0.f;
static uint32_t s_last_ms     = 0;

/* ------------------------------------------------------------------------
 * Shared with fire.c — small enough to duplicate; if we add a third effect
 * on this engine, factor mask_csv + HSV/RGB helpers into a common file.
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
        ESP_LOGW(TAG, "mask parse: unexpected char '%c' at row %d col %d", c, row, col);
        return false;
    }
    if (have_digit && row < h && col < w) {
        if (acc > 255) acc = 255;
        dst[row * w + col] = (uint8_t)acc;
    }
    return true;
}

static esp_err_t load_mask_from_nvs(const char *key, uint8_t *dst, int w, int h)
{
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

/* Standard RGB→hue (0..255). Saturation and value ignored — caller already
 * uses a fixed sat/value or the value-mask. Grayscale (R=G=B) returns 0;
 * the caller can detect that and skip hue blending if desired. */
static uint8_t rgb_to_hue(uint8_t r, uint8_t g, uint8_t b)
{
    float rf = r / 255.f, gf = g / 255.f, bf = b / 255.f;
    float mx = fmaxf(rf, fmaxf(gf, bf));
    float mn = fminf(rf, fminf(gf, bf));
    float d = mx - mn;
    if (d == 0.f) return 0;
    float h;
    if (mx == rf)      h = (gf - bf) / d + (gf < bf ? 6.f : 0.f);
    else if (mx == gf) h = (bf - rf) / d + 2.f;
    else               h = (rf - gf) / d + 4.f;
    h /= 6.f;
    int hue = (int)(h * 256.f);
    if (hue < 0)   hue += 256;
    if (hue > 255) hue %= 256;
    return (uint8_t)hue;
}

static void generate_next_row(void)
{
    int lo = (int)dynfire_params[DYNFIRE_PARAM_sparkMin];
    int hi = (int)dynfire_params[DYNFIRE_PARAM_sparkMax];
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
 * Lifecycle
 * ------------------------------------------------------------------------ */

esp_err_t dynfire_init(int w, int h)
{
    if (w <= 0 || h <= 0) return ESP_ERR_INVALID_ARG;
    if ((size_t)w * h > DYNFIRE_MAX_CELLS) {
        ESP_LOGE(TAG, "grid %dx%d exceeds max cells (%d)", w, h, DYNFIRE_MAX_CELLS);
        return ESP_ERR_INVALID_SIZE;
    }
    s_w = w; s_h = h;
    dynfire_params_load_nvs();
    s_heat       = (uint8_t *)calloc((size_t)w * h, 1);
    s_next_row   = (uint8_t *)calloc((size_t)w,     1);
    s_hue_mask   = (uint8_t *)calloc((size_t)w * h, 1);
    s_value_mask = (uint8_t *)calloc((size_t)w * h, 1);
    if (!s_heat || !s_next_row || !s_hue_mask || !s_value_mask) {
        ESP_LOGE(TAG, "OOM allocating dynfire buffers");
        return ESP_ERR_NO_MEM;
    }
    if (load_mask_from_nvs(NVS_KEY_HUE_MASK,   s_hue_mask,   w, h) != ESP_OK) {
        ESP_LOGW(TAG, "hue mask parse failed — falling back to flat offset 0");
    }
    if (load_mask_from_nvs(NVS_KEY_VALUE_MASK, s_value_mask, w, h) != ESP_OK) {
        ESP_LOGW(TAG, "value mask parse failed — using zeros (no cooling)");
    }
    s_phase   = 0.f;
    s_last_ms = 0;
    generate_next_row();
    shift_up();
    generate_next_row();
    ESP_LOGI(TAG, "init %dx%d", w, h);
    return ESP_OK;
}

void dynfire_deinit(void)
{
    free(s_heat);       s_heat = NULL;
    free(s_next_row);   s_next_row = NULL;
    free(s_hue_mask);   s_hue_mask = NULL;
    free(s_value_mask); s_value_mask = NULL;
    s_w = s_h = 0;
}

esp_err_t dynfire_render_frame(int w, int h, led_color_t *frame, uint32_t time_ms)
{
    if (w != s_w || h != s_h) return ESP_ERR_INVALID_STATE;

    /* Read the current base colour ONCE per frame so the whole grid uses the
     * same hue; if the user is dragging a colour picker, each frame reflects
     * the latest value without per-pixel jitter. */
    uint8_t br = 0, bg = 0, bb = 0;
    js_player_get_base_color(&br, &bg, &bb);
    uint8_t base_hue = rgb_to_hue(br, bg, bb);

    float speed = dynfire_params[DYNFIRE_PARAM_speed];
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

    float cooling   = dynfire_params[DYNFIRE_PARAM_cooling];
    if (cooling < 0.f) cooling = 0.f;
    float hueRange  = dynfire_params[DYNFIRE_PARAM_hueRange];
    bool  reverse   = dynfire_params[DYNFIRE_PARAM_reverse] >= 0.5f;
    int   total     = w * h;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t a = s_heat[(size_t)y * w + x];
            uint8_t b = (y == 0) ? s_next_row[x]
                                 : s_heat[(size_t)(y - 1) * w + x];
            float v = (1.f - s_phase) * (float)a + s_phase * (float)b;
            float mask = (float)s_value_mask[(size_t)y * w + x] * cooling;
            int val = (int)(v - mask);
            if (val < 0) val = 0;
            if (val > 255) val = 255;

            int rendered_y = (h - 1) - y;
            int idx = rendered_y * w + x;
            if (reverse) idx = total - 1 - idx;
            if (idx < 0 || idx >= total) continue;

            /* Per-cell hue = base_hue + (mask offset × hueRange), wrapped
             * into [0, 256). Matches the C++ baseColorMode=true intent
             * without its quirky branch. */
            int offset = (int)((float)s_hue_mask[(size_t)y * w + x] * hueRange);
            int hue = ((int)base_hue + offset) % 256;
            if (hue < 0) hue += 256;
            hsv_to_rgb((uint8_t)hue, 255, (uint8_t)val,
                       &frame[idx].r, &frame[idx].g, &frame[idx].b);
        }
    }
    return ESP_OK;
}
