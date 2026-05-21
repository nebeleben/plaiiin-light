/*
 * Phase 35 — cube dynfire.
 *
 * Mirrors hardcoded/cube/fire.c structure (4 independent 8x8 fires on the
 * side walls + twinkling embers on the top) but derives hue from the lamp's
 * base color, the way hardcoded/tower/dynfire.c does. hue_mask values shift
 * the per-cell hue around the base; hueRange scales that shift.
 *
 * Shares fire_hue_mask + fire_value_mask NVS with the tower/cube fire — the
 * masks describe the physical flame shape on an 8x8 face, not the rendering
 * style.
 */

// @effect dynfire 30
// @param speed 0.5..10 = 3 Heat rows shifted per second
// @param cooling 0.5..3 = 1 Multiplier on the value-mask subtraction (higher = dimmer)
// @param sparkMin 0..255 = 64 Bottom-row random heat lower bound
// @param sparkMax 0..255 = 255 Bottom-row random heat upper bound
// @param hueRange 0..2 = 1 Multiplier on the hue offset (0 = single tone, 1 = mask as-is, 2 = wider)
// @param topSparkle 0..1 = 1 Enable the twinkling-ember top cap (0 = top dark)

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

static const char *TAG = "hc_cube_dynfire";

#define NVS_KEY_HUE_MASK   "fire_hue_mask"
#define NVS_KEY_VALUE_MASK "fire_value_mask"

#define FACE_W      8
#define FACE_H      8
#define FACE_PIXELS (FACE_W * FACE_H)
#define SIDE_FACES  4
#define TOP_FACE    4
#define TOTAL_FACES 5

static uint8_t s_heat[SIDE_FACES][FACE_H][FACE_W];
static uint8_t s_next_row[SIDE_FACES][FACE_W];
static uint8_t s_hue_mask[FACE_H][FACE_W];
static uint8_t s_value_mask[FACE_H][FACE_W];
static float    s_phase = 0.f;
static uint32_t s_last_ms = 0;
static uint8_t  s_top_life[FACE_PIXELS];

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

static esp_err_t load_mask_from_nvs(const char *key, uint8_t *dst)
{
    size_t bufsz = (size_t)FACE_PIXELS * 6 + 32;
    char *src = (char *)malloc(bufsz);
    if (!src) return ESP_ERR_NO_MEM;
    config_get_str_or(key, src, bufsz, "");
    if (!src[0]) {
        free(src);
        ESP_LOGW(TAG, "NVS key '%s' empty — mask defaults to all zero", key);
        memset(dst, 0, (size_t)FACE_PIXELS);
        return ESP_OK;
    }
    bool ok = parse_mask(src, dst, FACE_W, FACE_H);
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

/* Source-space (col, row) → chain offset within a face. Respects the live
 * rotation / origin / serpentine / serp_axis from led_control — same math
 * as fire.c (kept duplicated for now; lift into a shared header if a third
 * cube effect arrives). */
static int face_xy_to_local(int col, int row)
{
    int rot    = led_control_get_rotation();
    int origin = led_control_get_origin();
    bool serp  = led_control_get_serpentine();
    int axis   = led_control_get_serp_axis();

    int px = col, py = row;
    if (rot == 90) {
        int tmp = px;
        px = FACE_W - 1 - py;
        py = tmp;
    } else if (rot == 180) {
        px = FACE_W - 1 - px;
        py = FACE_H - 1 - py;
    } else if (rot == 270) {
        int tmp = px;
        px = py;
        py = FACE_H - 1 - tmp;
    }

    if (origin == 1 || origin == 3) px = FACE_W - 1 - px;
    if (origin == 2 || origin == 3) py = FACE_H - 1 - py;

    if (axis == 1) {
        int col_chain = px;
        int row_chain = py;
        if (serp && (col_chain & 1)) row_chain = FACE_H - 1 - row_chain;
        return col_chain * FACE_H + row_chain;
    } else {
        int row_chain = py;
        int col_chain = px;
        if (serp && (row_chain & 1)) col_chain = FACE_W - 1 - col_chain;
        return row_chain * FACE_W + col_chain;
    }
}

static void generate_next_row(int face)
{
    int lo = (int)dynfire_params[DYNFIRE_PARAM_sparkMin];
    int hi = (int)dynfire_params[DYNFIRE_PARAM_sparkMax];
    if (lo < 0) lo = 0;
    if (lo > 255) lo = 255;
    if (hi < 0) hi = 0;
    if (hi > 255) hi = 255;
    if (hi < lo) { int t = lo; lo = hi; hi = t; }
    int span = (hi - lo) + 1;
    for (int x = 0; x < FACE_W; x++) {
        s_next_row[face][x] = (uint8_t)(lo + (esp_random() % (uint32_t)span));
    }
}

static void shift_up(int face)
{
    for (int y = FACE_H - 1; y > 0; y--) {
        memcpy(s_heat[face][y], s_heat[face][y - 1], FACE_W);
    }
    memcpy(s_heat[face][0], s_next_row[face], FACE_W);
}

esp_err_t dynfire_init(int w, int h)
{
    (void)h;
    if (w != TOTAL_FACES * FACE_PIXELS) {
        ESP_LOGE(TAG, "cube dynfire expects %d LEDs, got %d",
                 TOTAL_FACES * FACE_PIXELS, w);
        return ESP_ERR_INVALID_STATE;
    }

    dynfire_params_load_nvs();

    memset(s_heat,     0, sizeof(s_heat));
    memset(s_next_row, 0, sizeof(s_next_row));
    memset(s_top_life, 0, sizeof(s_top_life));

    if (load_mask_from_nvs(NVS_KEY_HUE_MASK,   (uint8_t *)s_hue_mask)   != ESP_OK) {
        ESP_LOGW(TAG, "hue mask parse failed — using zeros (flat offset)");
    }
    if (load_mask_from_nvs(NVS_KEY_VALUE_MASK, (uint8_t *)s_value_mask) != ESP_OK) {
        ESP_LOGW(TAG, "value mask parse failed — using zeros (no cooling)");
    }

    s_phase   = 0.f;
    s_last_ms = 0;
    for (int f = 0; f < SIDE_FACES; f++) {
        generate_next_row(f);
        shift_up(f);
        generate_next_row(f);
    }
    ESP_LOGI(TAG, "init: 4 side dynfires + sparkle top");
    return ESP_OK;
}

void dynfire_deinit(void) { /* nothing to free */ }

static void render_side(int face, led_color_t *frame_face,
                        float cooling, float hueRange, uint8_t base_hue)
{
    for (int y = 0; y < FACE_H; y++) {
        for (int x = 0; x < FACE_W; x++) {
            uint8_t a = s_heat[face][y][x];
            uint8_t b = (y == 0) ? s_next_row[face][x]
                                 : s_heat[face][y - 1][x];
            float v = (1.f - s_phase) * (float)a + s_phase * (float)b;
            float mask = (float)s_value_mask[y][x] * cooling;
            int val = (int)(v - mask);
            if (val < 0) val = 0;
            if (val > 255) val = 255;

            int rendered_row = (FACE_H - 1) - y;
            int local = face_xy_to_local(x, rendered_row);

            int offset = (int)((float)s_hue_mask[y][x] * hueRange);
            int hue = ((int)base_hue + offset) % 256;
            if (hue < 0) hue += 256;
            hsv_to_rgb((uint8_t)hue, 255, (uint8_t)val,
                       &frame_face[local].r,
                       &frame_face[local].g,
                       &frame_face[local].b);
        }
    }
}

static void render_top(led_color_t *frame_top, float speed, uint8_t base_hue)
{
    int decay = 220;
    uint32_t spawn_threshold = 0x06000000;
    if (speed > 0.f) {
        spawn_threshold = (uint32_t)((float)spawn_threshold * (speed / 3.f));
        if (spawn_threshold < 0x01000000) spawn_threshold = 0x01000000;
    }

    for (int i = 0; i < FACE_PIXELS; i++) {
        int life = (s_top_life[i] * decay) >> 8;
        if (esp_random() < spawn_threshold) life = 255;
        s_top_life[i] = (uint8_t)life;
    }

    for (int row = 0; row < FACE_H; row++) {
        for (int col = 0; col < FACE_W; col++) {
            int src_i = row * FACE_W + col;
            int local = face_xy_to_local(col, row);
            uint8_t v = s_top_life[src_i];
            hsv_to_rgb(base_hue, 255, v,
                       &frame_top[local].r,
                       &frame_top[local].g,
                       &frame_top[local].b);
        }
    }
}

esp_err_t dynfire_render_frame(int w, int h, led_color_t *frame, uint32_t time_ms)
{
    (void)h;
    if (w != TOTAL_FACES * FACE_PIXELS) return ESP_ERR_INVALID_STATE;

    uint8_t br = 0, bg = 0, bb = 0;
    js_player_get_base_color(&br, &bg, &bb);
    uint8_t base_hue = rgb_to_hue(br, bg, bb);

    float speed = dynfire_params[DYNFIRE_PARAM_speed];
    if (speed <= 0.f) speed = 0.5f;
    uint32_t dt_ms = (s_last_ms == 0) ? 0 : (time_ms - s_last_ms);
    s_last_ms = time_ms;
    s_phase += (float)dt_ms * 0.001f * speed;
    while (s_phase >= 1.f) {
        for (int f = 0; f < SIDE_FACES; f++) {
            shift_up(f);
            generate_next_row(f);
        }
        s_phase -= 1.f;
    }
    if (s_phase < 0.f) s_phase = 0.f;

    float cooling  = dynfire_params[DYNFIRE_PARAM_cooling];
    if (cooling < 0.f) cooling = 0.f;
    float hueRange = dynfire_params[DYNFIRE_PARAM_hueRange];

    for (int f = 0; f < SIDE_FACES; f++) {
        render_side(f, &frame[f * FACE_PIXELS], cooling, hueRange, base_hue);
    }
    if (dynfire_params[DYNFIRE_PARAM_topSparkle] >= 0.5f) {
        render_top(&frame[TOP_FACE * FACE_PIXELS], speed, base_hue);
    }
    return ESP_OK;
}
