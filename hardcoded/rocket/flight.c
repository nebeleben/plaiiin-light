/*
 * Phase 35 — hardcoded rocket fire.
 *
 * Rocket chain layout (base → tip):
 *   idx   0..7    booster_inner   ( 8-LED ring)
 *   idx   8..31   booster_outer   (24-LED ring)
 *   idx  32..95   body_matrix_1   (8x8, cols 0..7  of the cylinder)
 *   idx  96..159  body_matrix_2   (8x8, cols 8..15 of the cylinder)
 *   idx 160..167  tip_inner       ( 8-LED ring)
 *   idx 168..183  tip_outer       (16-LED ring)
 *   total = 184 LEDs
 *
 * The two body matrices are joined HORIZONTALLY (the cylinder is wider, not
 * taller): the body is an 8-row × 16-col cylinder, with cols wrapping
 * seamlessly (col 15 sits next to col 0). Each matrix is wired serpentine
 * on axis=1 (chain advances down a column, odd columns reversed).
 *
 * Visual model — the rocket is "flying" upward fast: flames trail down the
 * body away from the tip. Heat is injected at body row 0 (top, near tip)
 * and propagates down toward the booster. The booster's outer ring glows
 * bright (engine halo); inner ring stays dim. The tip rings stay dim too
 * — the nose is the leading edge, mostly dark.
 *
 * Per-row hue gradient is synthesised in code (top = yellow, bottom = red)
 * so the rocket doesn't depend on a NVS fire pattern file. Each col is an
 * independent heat column; cylinder-wrap doesn't need special handling.
 */

// @effect flight 30
// @param speed 0.5..10 = 3 Heat rows shifted per second
// @param cooling 0.5..3 = 1 Multiplier on the value-mask subtraction
// @param sparkMin 0..255 = 64 Spawn-row random heat lower bound
// @param sparkMax 0..255 = 255 Spawn-row random heat upper bound
// @param boosterPulse 0..1 = 0.3 Booster halo pulse depth (0 = steady, 1 = full swing)

#include "hardcoded_effects.h"
#include "config_store.h"
#include "flight_params.h"
#include "led_control.h"
#include "esp_log.h"
#include "esp_random.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "hc_rocket_flight";

#define BODY_W           16    /* cols around the cylinder (matrix1 + matrix2) */
#define BODY_H           8     /* rows along the rocket axis (matrix height)  */
#define BODY_CELLS       (BODY_W * BODY_H)
#define MATRIX_SIDE      8     /* each body matrix is 8x8 */

#define BOOSTER_INNER_BASE   0
#define BOOSTER_INNER_LEN    8
#define BOOSTER_OUTER_BASE   (BOOSTER_INNER_BASE + BOOSTER_INNER_LEN)
#define BOOSTER_OUTER_LEN    24
#define BODY_BASE            (BOOSTER_OUTER_BASE + BOOSTER_OUTER_LEN)
#define BODY_M1_BASE         BODY_BASE                      /* idx  32 */
#define BODY_M2_BASE         (BODY_BASE + 64)               /* idx  96 */
#define TIP_INNER_BASE       (BODY_BASE + 128)              /* idx 160 */
#define TIP_INNER_LEN        8
#define TIP_OUTER_BASE       (TIP_INNER_BASE + TIP_INNER_LEN) /* idx 168 */
#define TIP_OUTER_LEN        16
#define ROCKET_TOTAL_LEDS    (TIP_OUTER_BASE + TIP_OUTER_LEN) /* 184 */

#define DIM_VALUE            48      /* dark-end value for booster_inner + tip */
#define BRIGHT_HUE           25      /* yellow-orange — halo "light-end" */
#define DARK_HUE             0       /* deep red — body bottom + dim rings */

static uint8_t s_heat[BODY_H][BODY_W];
static uint8_t s_next_row[BODY_W];
static uint8_t s_hue_mask[BODY_H];   /* per-row hue, synthesised at init */
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

/* (col, body_row) → full-chain idx. col 0..7 lives in matrix 1 (idx 32..95);
 * col 8..15 in matrix 2 (idx 96..159). Simple serpentine on axis=1 per
 * matrix; origin/rotation are not applied here (rocket is registered as a
 * "strip" and the actual physical wiring matched this naive mapping in
 * testing — the desired top→bottom flow is achieved by flipping the
 * body_row in the render loop, not in the chain mapping). */
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
    int lo = (int)flight_params[FLIGHT_PARAM_sparkMin];
    int hi = (int)flight_params[FLIGHT_PARAM_sparkMax];
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

/* "Shift up" in algorithm-space pushes heat from row 0 toward row BODY_H-1.
 * Since we render row 0 at body TOP (no flip), this visually moves heat
 * DOWN the body — the reversed flow the rocket needs. */
static void shift_heat(void)
{
    for (int y = BODY_H - 1; y > 0; y--) {
        memcpy(s_heat[y], s_heat[y - 1], BODY_W);
    }
    memcpy(s_heat[0], s_next_row, BODY_W);
}

esp_err_t flight_init(int w, int h)
{
    (void)h;
    if (w != ROCKET_TOTAL_LEDS) {
        ESP_LOGE(TAG, "rocket flight expects %d LEDs, got %d", ROCKET_TOTAL_LEDS, w);
        return ESP_ERR_INVALID_STATE;
    }
    flight_params_load_nvs();

    memset(s_heat,     0, sizeof(s_heat));
    memset(s_next_row, 0, sizeof(s_next_row));

    /* Per-row hue gradient: row 0 (top, freshly-injected heat) = deep red,
     * row BODY_H-1 (bottom, oldest heat) = yellow. The visual is "bright
     * red flames ignite at the tip and fade downward into a dim yellow
     * trail at the booster" — matches the desired top→bottom flow with
     * red at top / yellow at bottom. */
    for (int y = 0; y < BODY_H; y++) {
        int hue = DARK_HUE + (BRIGHT_HUE - DARK_HUE) * y / (BODY_H - 1);
        if (hue < 0) hue = 0;
        if (hue > 255) hue = 255;
        s_hue_mask[y] = (uint8_t)hue;
    }

    s_phase   = 0.f;
    s_last_ms = 0;
    generate_next_row();
    shift_heat();
    generate_next_row();
    ESP_LOGI(TAG, "init: body flight (8x16 reversed) + booster halo + dim tip");
    return ESP_OK;
}

void flight_deinit(void) { /* nothing to free */ }

static void render_body(led_color_t *frame, float cooling)
{
    for (int y = 0; y < BODY_H; y++) {
        uint8_t hue = s_hue_mask[y];
        for (int x = 0; x < BODY_W; x++) {
            uint8_t a = s_heat[y][x];
            uint8_t b = (y == 0) ? s_next_row[x] : s_heat[y - 1][x];
            float v = (1.f - s_phase) * (float)a + s_phase * (float)b;
            /* Gentle row-proportional cooling — flames fade out toward the
             * bottom of the body (oldest heat). The 50.f tunes the maximum
             * subtraction at the lowest row to ~50*cooling. */
            int val = (int)(v - cooling * 50.f * (float)y / (float)(BODY_H - 1));
            if (val < 0) val = 0;
            if (val > 255) val = 255;
            /* Flip body_row so heat[0] (fresh) lands at the body's TOP edge
             * (near the tip), and heat[BODY_H-1] (oldest, dimmest) lands at
             * the bottom (near the booster). Flow direction = top→bottom. */
            int idx = body_xy_to_chain(x, BODY_H - 1 - y);
            hsv_to_rgb(hue, 255, (uint8_t)val,
                       &frame[idx].r, &frame[idx].g, &frame[idx].b);
        }
    }
}

static void render_booster_and_tip(led_color_t *frame, uint32_t time_ms, float pulse_depth)
{
    /* Booster outer: bright yellow with a slow pulse (engine throb).
     * pulse runs at ~1.5 Hz, modulating value between (1-pulse_depth)*255..255. */
    float pulse = 0.5f + 0.5f * sinf((float)time_ms * 2.f * 3.14159f * 1.5f / 1000.f);
    float vmin  = 255.f * (1.f - pulse_depth);
    int   outer_val = (int)(vmin + (255.f - vmin) * pulse);
    if (outer_val < 0)   outer_val = 0;
    if (outer_val > 255) outer_val = 255;

    uint8_t br, bg, bb;

    /* Booster inner ring: dim red, steady. */
    hsv_to_rgb(DARK_HUE, 255, DIM_VALUE, &br, &bg, &bb);
    for (int i = 0; i < BOOSTER_INNER_LEN; i++) {
        frame[BOOSTER_INNER_BASE + i].r = br;
        frame[BOOSTER_INNER_BASE + i].g = bg;
        frame[BOOSTER_INNER_BASE + i].b = bb;
    }

    /* Booster outer ring: bright yellow-orange halo, pulsing. */
    hsv_to_rgb(BRIGHT_HUE, 255, (uint8_t)outer_val, &br, &bg, &bb);
    for (int i = 0; i < BOOSTER_OUTER_LEN; i++) {
        frame[BOOSTER_OUTER_BASE + i].r = br;
        frame[BOOSTER_OUTER_BASE + i].g = bg;
        frame[BOOSTER_OUTER_BASE + i].b = bb;
    }

    /* Tip rings: dim red, both. */
    hsv_to_rgb(DARK_HUE, 255, DIM_VALUE, &br, &bg, &bb);
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

esp_err_t flight_render_frame(int w, int h, led_color_t *frame, uint32_t time_ms)
{
    (void)h;
    if (w != ROCKET_TOTAL_LEDS) return ESP_ERR_INVALID_STATE;

    float speed = flight_params[FLIGHT_PARAM_speed];
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

    float cooling = flight_params[FLIGHT_PARAM_cooling];
    if (cooling < 0.f) cooling = 0.f;
    float pulse_depth = flight_params[FLIGHT_PARAM_boosterPulse];
    if (pulse_depth < 0.f) pulse_depth = 0.f;
    if (pulse_depth > 1.f) pulse_depth = 1.f;

    render_body(frame, cooling);
    render_booster_and_tip(frame, time_ms, pulse_depth);
    return ESP_OK;
}
