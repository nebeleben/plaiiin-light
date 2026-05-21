/*
 * Phase 35 — cube fire effect.
 *
 * The cube chains 5 8x8 panels into one 320-LED strip:
 *   faces 0..3 = front/right/back/left side walls (the horizontal band)
 *   face  4    = top cap
 *
 * For the sides we run four INDEPENDENT 8x8 FirePattern heat matrices so each
 * wall flickers on its own — a single shared matrix would make all four walls
 * pulse in lockstep, which reads as a tiled image rather than a fire.
 *
 * For the top cap we keep the cube's "ember" idea: each top LED has its own
 * decaying brightness; new sparkles are born at random intervals so the top
 * twinkles softly above the flames. Sparkles use the same warm hue as the
 * fire's brightest band so the top reads as the same fire continuing upward.
 *
 * Mask data is the same fire_hue_mask / fire_value_mask 8x8 used by the tower
 * — the masks describe an 8x8 flame shape, and we apply that shape on every
 * side face.
 *
 * Per-face wiring (matches the cube profile defaults: serpentine=y, axis=1):
 *   each face's first LED is col=0, row=0 in the panel; the chain steps down
 *   column 0 then up column 1 (serpentine), repeated for all 8 columns.
 */

// @effect fire 30
// @param speed 0.5..10 = 3 Heat rows shifted per second
// @param cooling 0.5..3 = 1 Multiplier on the value-mask subtraction (higher = dimmer)
// @param sparkMin 0..255 = 64 Bottom-row random heat lower bound
// @param sparkMax 0..255 = 255 Bottom-row random heat upper bound
// @param topSparkle 0..1 = 1 Enable the twinkling-ember top cap (0 = top dark)

#include "hardcoded_effects.h"
#include "config_store.h"
#include "fire_params.h"
#include "esp_log.h"
#include "esp_random.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "hc_cube_fire";

#define NVS_KEY_HUE_MASK   "fire_hue_mask"
#define NVS_KEY_VALUE_MASK "fire_value_mask"

#define FACE_W       8
#define FACE_H       8
#define FACE_PIXELS  (FACE_W * FACE_H)
#define SIDE_FACES   4
#define TOP_FACE     4
#define TOTAL_FACES  5

/* Per-side heat state — heat[face][row][col], row 0 = bottom of flame. */
static uint8_t s_heat[SIDE_FACES][FACE_H][FACE_W];
static uint8_t s_next_row[SIDE_FACES][FACE_W];
static uint8_t s_hue_mask[FACE_H][FACE_W];
static uint8_t s_value_mask[FACE_H][FACE_W];
static float    s_phase = 0.f;
static uint32_t s_last_ms = 0;

/* Top-cap sparkle state — one decaying brightness per top LED. */
static uint8_t s_top_life[FACE_PIXELS];

/* Same parser as the tower's fire — CSV-of-CSV with whitespace tolerance. */
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

/* Source/viewer-space (col, row) → chain offset within a face. Mirrors the
 * inverse of led_control's chain_to_panel + rotation pipeline so the lamp's
 * orientation config (rotation / origin / serpentine / serp_axis) controls
 * how every face is drawn — same conventions as set_logical for single-panel
 * forms. (col=0, row=0) is the top-left of the source image. */
static int face_xy_to_local(int col, int row)
{
    int rot    = led_control_get_rotation();
    int origin = led_control_get_origin();
    bool serp  = led_control_get_serpentine();
    int axis   = led_control_get_serp_axis();

    /* Inverse rotation: undo what set_logical's rotation sampling step does
     * (forward maps panel (px,py) → source (sx,sy); we have (sx,sy) = (col,row)
     * and want (px,py)). */
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

    /* Inverse origin (the transform is involutive — same code applies). */
    if (origin == 1 || origin == 3) px = FACE_W - 1 - px;
    if (origin == 2 || origin == 3) py = FACE_H - 1 - py;

    /* Inverse serpentine: (px, py) is now in pre-origin panel space, which
     * for axis=1 equals (col_chain, row_chain_post_serp). */
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
    int lo = (int)fire_params[FIRE_PARAM_sparkMin];
    int hi = (int)fire_params[FIRE_PARAM_sparkMax];
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

esp_err_t fire_init(int w, int h)
{
    (void)h;
    /* Runtime passes (w=led_count, h=1) for the cube. Sanity-check we got the
     * expected 5x8x8 = 320 chain — the effect makes assumptions about face
     * layout that only hold for that geometry. */
    if (w != TOTAL_FACES * FACE_PIXELS) {
        ESP_LOGE(TAG, "cube fire expects %d LEDs, got %d", TOTAL_FACES * FACE_PIXELS, w);
        return ESP_ERR_INVALID_STATE;
    }

    fire_params_load_nvs();

    memset(s_heat,      0, sizeof(s_heat));
    memset(s_next_row,  0, sizeof(s_next_row));
    memset(s_top_life,  0, sizeof(s_top_life));

    if (load_mask_from_nvs(NVS_KEY_HUE_MASK,   (uint8_t *)s_hue_mask)   != ESP_OK) {
        ESP_LOGW(TAG, "hue mask parse failed — using zeros (all red)");
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
    ESP_LOGI(TAG, "init: 4 side fires + sparkle top");
    return ESP_OK;
}

void fire_deinit(void)
{
    /* Static buffers — nothing to free. */
}

/* Render one 8x8 side face into the slice of `frame` starting at face_base. */
static void render_side(int face, led_color_t *frame_face, float cooling)
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

            /* Mask row 0 = bottom of the flame; render it onto the face's
             * bottom edge by flipping y. */
            int rendered_row = (FACE_H - 1) - y;
            int local = face_xy_to_local(x, rendered_row);
            uint8_t hue = s_hue_mask[y][x];
            hsv_to_rgb(hue, 255, (uint8_t)val,
                       &frame_face[local].r,
                       &frame_face[local].g,
                       &frame_face[local].b);
        }
    }
}

/* Twinkling top: each top LED has its own decaying life. New sparkles are
 * born at random intervals. The hue copies the mask's hottest band so the
 * top reads as embers from the fire below. */
static void render_top(led_color_t *frame_top, float speed)
{
    /* Multiplicative decay tuned for "warm twinkle, not flash" — about 0.6
     * seconds half-life at speed 3. */
    int decay = 220;  /* out of 256 — life *= decay/256 each frame */

    /* Spawn rate: target ~3-5 live sparkles steady-state. */
    uint32_t spawn_threshold = 0x06000000;
    if (speed > 0.f) {
        /* Faster speed → faster spawn — keeps the top in step with the side
         * flame's apparent activity. */
        spawn_threshold = (uint32_t)((float)spawn_threshold * (speed / 3.f));
        if (spawn_threshold < 0x01000000) spawn_threshold = 0x01000000;
    }

    /* Pick a representative hot-band hue from the mask — col 4 row 0 if the
     * mask has data; fall back to a warm orange (~25) otherwise. */
    uint8_t hue = s_hue_mask[0][4];
    if (hue == 0) {
        for (int x = 0; x < FACE_W; x++) {
            if (s_hue_mask[0][x] > hue) hue = s_hue_mask[0][x];
        }
        if (hue == 0) hue = 25;
    }

    for (int i = 0; i < FACE_PIXELS; i++) {
        int life = (s_top_life[i] * decay) >> 8;
        if (esp_random() < spawn_threshold) life = 255;
        s_top_life[i] = (uint8_t)life;
    }

    /* Top face wiring: the chain enters the top face at face-local idx 256
     * (= TOP_FACE*64). We map (col, row) the same way as the sides; users
     * iterate physical mounting if a particular twinkle layout reads wrong. */
    for (int row = 0; row < FACE_H; row++) {
        for (int col = 0; col < FACE_W; col++) {
            int src_i = row * FACE_W + col;
            int local = face_xy_to_local(col, row);
            uint8_t v = s_top_life[src_i];
            hsv_to_rgb(hue, 255, v,
                       &frame_top[local].r,
                       &frame_top[local].g,
                       &frame_top[local].b);
        }
    }
}

esp_err_t fire_render_frame(int w, int h, led_color_t *frame, uint32_t time_ms)
{
    (void)h;
    if (w != TOTAL_FACES * FACE_PIXELS) return ESP_ERR_INVALID_STATE;

    float speed = fire_params[FIRE_PARAM_speed];
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

    float cooling = fire_params[FIRE_PARAM_cooling];
    if (cooling < 0.f) cooling = 0.f;

    for (int f = 0; f < SIDE_FACES; f++) {
        render_side(f, &frame[f * FACE_PIXELS], cooling);
    }

    if (fire_params[FIRE_PARAM_topSparkle] >= 0.5f) {
        render_top(&frame[TOP_FACE * FACE_PIXELS], speed);
    }
    /* else: top stays black (calloc'd by the runtime each frame). */

    return ESP_OK;
}
