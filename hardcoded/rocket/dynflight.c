/*
 * Phase 35 — hardcoded rocket dynfire.
 *
 * Same segment layout and reversed-flow body fire as rocket/fire.c, but the
 * fire palette is derived from the lamp's base color instead of red-yellow.
 * Booster halo, booster inner, and tip rings all use shades of the base
 * color (bright for the outer halo, dim for everything else).
 *
 * Body hue varies along the gradient from the base hue (top, where heat is
 * injected) shifted by `hueRange * (1 - row_fraction)` — same idea as the
 * tower's dynfire, but laid out vertically along the rocket body.
 */

// @effect dynflight 30
// @param speed 0.5..10 = 3 Heat rows shifted per second
// @param cooling 0.5..3 = 1 Multiplier on the value-mask subtraction
// @param sparkMin 0..255 = 64 Spawn-row random heat lower bound
// @param sparkMax 0..255 = 255 Spawn-row random heat upper bound
// @param hueRange 0..2 = 1 Multiplier on the row hue offset (0 = single tone, 1 = mask as-is, 2 = wider)
// @param boosterPulse 0..1 = 0.3 Booster halo pulse depth (0 = steady, 1 = full swing)

#include "hardcoded_effects.h"
#include "config_store.h"
#include "dynflight_params.h"
#include "led_control.h"
#include "js_player.h"
#include "esp_log.h"
#include "esp_random.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "hc_rocket_dynflight";

#define BODY_W           16    /* cols around the cylinder (matrix1 + matrix2) */
#define BODY_H           8     /* rows along the rocket axis (matrix height)  */
#define MATRIX_SIDE      8     /* each body matrix is 8x8 */

#define BOOSTER_INNER_BASE   0
#define BOOSTER_INNER_LEN    8
#define BOOSTER_OUTER_BASE   (BOOSTER_INNER_BASE + BOOSTER_INNER_LEN)
#define BOOSTER_OUTER_LEN    24
#define BODY_M1_BASE         (BOOSTER_OUTER_BASE + BOOSTER_OUTER_LEN) /* 32 */
#define BODY_M2_BASE         (BODY_M1_BASE + 64)                      /* 96 */
#define TIP_INNER_BASE       (BODY_M2_BASE + 64)                      /* 160 */
#define TIP_INNER_LEN        8
#define TIP_OUTER_BASE       (TIP_INNER_BASE + TIP_INNER_LEN)         /* 168 */
#define TIP_OUTER_LEN        16
#define ROCKET_TOTAL_LEDS    (TIP_OUTER_BASE + TIP_OUTER_LEN)         /* 184 */

#define DIM_VALUE            48

static uint8_t s_heat[BODY_H][BODY_W];
static uint8_t s_next_row[BODY_W];
/* Per-row hue OFFSET from the base hue. Top row offset = 0 (pure base);
 * bottom row offset shifts toward "deeper" by hueRange units, so the body
 * fades into a darker tint at the booster end. */
static uint8_t s_row_offset[BODY_H];
static float    s_phase = 0.f;
static uint32_t s_last_ms = 0;

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

/* See hardcoded/rocket/fire.c — simple serpentine on axis=1 per matrix.
 * The top→bottom flow is achieved by flipping body_row in the render loop,
 * not in the chain mapping. */
static int body_xy_to_chain(int col, int body_row)
{
    int matrix_base, m_col;
    if (col < MATRIX_SIDE) {
        matrix_base = BODY_M1_BASE;
        m_col = col;
    } else {
        matrix_base = BODY_M2_BASE;
        m_col = col - MATRIX_SIDE;
    }
    int row_raw = (m_col & 1) ? (MATRIX_SIDE - 1 - body_row) : body_row;
    return matrix_base + m_col * MATRIX_SIDE + row_raw;
}

static void generate_next_row(void)
{
    int lo = (int)dynflight_params[DYNFLIGHT_PARAM_sparkMin];
    int hi = (int)dynflight_params[DYNFLIGHT_PARAM_sparkMax];
    if (lo < 0) lo = 0;
    if (lo > 255) lo = 255;
    if (hi < 0) hi = 0;
    if (hi > 255) hi = 255;
    if (hi < lo) { int t = lo; lo = hi; hi = t; }
    int span = (hi - lo) + 1;
    for (int x = 0; x < BODY_W; x++) {
        s_next_row[x] = (uint8_t)(lo + (esp_random() % (uint32_t)span));
    }
}

static void shift_heat(void)
{
    for (int y = BODY_H - 1; y > 0; y--) {
        memcpy(s_heat[y], s_heat[y - 1], BODY_W);
    }
    memcpy(s_heat[0], s_next_row, BODY_W);
}

esp_err_t dynflight_init(int w, int h)
{
    (void)h;
    if (w != ROCKET_TOTAL_LEDS) {
        ESP_LOGE(TAG, "rocket dynflight expects %d LEDs, got %d", ROCKET_TOTAL_LEDS, w);
        return ESP_ERR_INVALID_STATE;
    }
    dynflight_params_load_nvs();

    memset(s_heat,     0, sizeof(s_heat));
    memset(s_next_row, 0, sizeof(s_next_row));

    /* Per-row hue offset, max at row 0 (top, near tip) and 0 at the bottom.
     * The body renders hue = base_hue - offset*hueRange, so the top sits at
     * a darker variant ("red" equivalent in fire's palette) and the bottom
     * at the pure base color ("yellow" equivalent). Matches the desired
     * top→bottom flow with darker color at top, pure base at bottom. */
    for (int y = 0; y < BODY_H; y++) {
        s_row_offset[y] = (uint8_t)(25 * (BODY_H - 1 - y) / (BODY_H - 1));
    }

    s_phase   = 0.f;
    s_last_ms = 0;
    generate_next_row();
    shift_heat();
    generate_next_row();
    ESP_LOGI(TAG, "init: body dynflight + booster halo + dim tip");
    return ESP_OK;
}

void dynflight_deinit(void) { /* nothing to free */ }

static void render_body(led_color_t *frame, float cooling, float hueRange, uint8_t base_hue)
{
    for (int y = 0; y < BODY_H; y++) {
        int offset = (int)((float)s_row_offset[y] * hueRange);
        /* Body fades AWAY from base_hue toward "deeper" tones as you move
         * down — mirrors how the fire palette goes yellow→red top-to-bottom.
         * Subtract so the bottom feels darker; wrap into [0,255]. */
        int hue = ((int)base_hue - offset) % 256;
        if (hue < 0) hue += 256;
        for (int x = 0; x < BODY_W; x++) {
            uint8_t a = s_heat[y][x];
            uint8_t b = (y == 0) ? s_next_row[x] : s_heat[y - 1][x];
            float v = (1.f - s_phase) * (float)a + s_phase * (float)b;
            int val = (int)(v - cooling * 50.f * (float)y / (float)(BODY_H - 1));
            if (val < 0) val = 0;
            if (val > 255) val = 255;
            /* Flip body_row so heat[0] (fresh, darker hue) lands at the top
             * of the body and heat[BODY_H-1] (oldest, pure base) lands at
             * the bottom. Flow = top→bottom; colors track that. */
            int idx = body_xy_to_chain(x, BODY_H - 1 - y);
            hsv_to_rgb((uint8_t)hue, 255, (uint8_t)val,
                       &frame[idx].r, &frame[idx].g, &frame[idx].b);
        }
    }
}

static void render_booster_and_tip(led_color_t *frame, uint32_t time_ms,
                                   float pulse_depth, uint8_t base_hue)
{
    float pulse = 0.5f + 0.5f * sinf((float)time_ms * 2.f * 3.14159f * 1.5f / 1000.f);
    float vmin  = 255.f * (1.f - pulse_depth);
    int   outer_val = (int)(vmin + (255.f - vmin) * pulse);
    if (outer_val < 0)   outer_val = 0;
    if (outer_val > 255) outer_val = 255;

    uint8_t br, bg, bb;

    hsv_to_rgb(base_hue, 255, DIM_VALUE, &br, &bg, &bb);
    for (int i = 0; i < BOOSTER_INNER_LEN; i++) {
        frame[BOOSTER_INNER_BASE + i].r = br;
        frame[BOOSTER_INNER_BASE + i].g = bg;
        frame[BOOSTER_INNER_BASE + i].b = bb;
    }

    hsv_to_rgb(base_hue, 255, (uint8_t)outer_val, &br, &bg, &bb);
    for (int i = 0; i < BOOSTER_OUTER_LEN; i++) {
        frame[BOOSTER_OUTER_BASE + i].r = br;
        frame[BOOSTER_OUTER_BASE + i].g = bg;
        frame[BOOSTER_OUTER_BASE + i].b = bb;
    }

    hsv_to_rgb(base_hue, 255, DIM_VALUE, &br, &bg, &bb);
    for (int i = 0; i < TIP_INNER_LEN; i++) {
        frame[TIP_INNER_BASE + i].r = br;
        frame[TIP_INNER_BASE + i].g = bg;
        frame[TIP_INNER_BASE + i].b = bb;
    }
    for (int i = 0; i < TIP_OUTER_LEN; i++) {
        frame[TIP_OUTER_BASE + i].r = br;
        frame[TIP_OUTER_BASE + i].g = bg;
        frame[TIP_OUTER_BASE + i].b = bb;
    }
}

esp_err_t dynflight_render_frame(int w, int h, led_color_t *frame, uint32_t time_ms)
{
    (void)h;
    if (w != ROCKET_TOTAL_LEDS) return ESP_ERR_INVALID_STATE;

    uint8_t br = 0, bg = 0, bb = 0;
    js_player_get_base_color(&br, &bg, &bb);
    uint8_t base_hue = rgb_to_hue(br, bg, bb);

    float speed = dynflight_params[DYNFLIGHT_PARAM_speed];
    if (speed <= 0.f) speed = 0.5f;
    uint32_t dt_ms = (s_last_ms == 0) ? 0 : (time_ms - s_last_ms);
    s_last_ms = time_ms;
    s_phase += (float)dt_ms * 0.001f * speed;
    while (s_phase >= 1.f) {
        shift_heat();
        generate_next_row();
        s_phase -= 1.f;
    }
    if (s_phase < 0.f) s_phase = 0.f;

    float cooling     = dynflight_params[DYNFLIGHT_PARAM_cooling];
    if (cooling < 0.f) cooling = 0.f;
    float hueRange    = dynflight_params[DYNFLIGHT_PARAM_hueRange];
    float pulse_depth = dynflight_params[DYNFLIGHT_PARAM_boosterPulse];
    if (pulse_depth < 0.f) pulse_depth = 0.f;
    if (pulse_depth > 1.f) pulse_depth = 1.f;

    render_body(frame, cooling, hueRange, base_hue);
    render_booster_and_tip(frame, time_ms, pulse_depth, base_hue);
    return ESP_OK;
}
