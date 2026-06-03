/* Phase 29 — wormhole lamp render mode. See wormhole.h and
 * docs/wormhole-api.md for the frozen contract.
 *
 * This module owns the wormhole config (wh_mode/wh_rings/wh_phys/wh_creative),
 * the geometry gate, and the single tiling function wormhole_expand() shared
 * by the JS player (js_player.c) and the WebSocket stream path (ws_server.c).
 *
 * The JSON-array config (wh_phys, wh_creative) follows the share_keys pattern:
 * a JSON array packed into one NVS string. Parsing is done with the same
 * strstr/strchr scanning the rest of the codebase uses for small JSON bodies
 * — no JSON library is pulled in for the firmware config path.
 */

#include "wormhole.h"
#include "config_store.h"
#include "esp_log.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "wormhole";

/* Cap on rings we keep per-ring config for. v1=2, v2=4; 64 leaves generous
 * head-room and bounds the static config arrays. Rings beyond this still tile
 * — they just use the all-zero / default config (which is the documented
 * behaviour for rings with no explicit entry anyway). */
#define WH_MAX_RINGS 64

/* Per-ring physical mounting facts. Set-once, applied in BOTH modes. */
typedef struct {
    uint8_t face;       /* 0|1 — half-turn the ring is mounted at */
    uint8_t direction;  /* 0|1 — winding direction the LEDs were soldered in */
    uint8_t offset;     /* 0..23 — physical index at the ring's 12-o'clock */
} wh_phys_t;

/* Per-ring creative knobs. Mirror mode only, per-lamp. */
typedef struct {
    bool  reverse;      /* extra reversal bit, XORed in */
    uint8_t offset;     /* 0..23 — extra additive bit, summed in */
    float brightness;   /* 0.0..1.0 — per-ring output scale */
} wh_creative_t;

static wormhole_mode_t s_mode = WORMHOLE_MODE_STRIP;
static int  s_rings = 1;
static wh_phys_t     s_phys[WH_MAX_RINGS];
static wh_creative_t s_creative[WH_MAX_RINGS];
static uint32_t s_stream_gen = 0;
static bool s_loaded = false;

/* ------------------------------------------------------------------------
 * Tiny JSON-array scanning helpers — strstr/strchr style, matching the rest
 * of the firmware. Operate on one NVS-stored JSON-array string.
 * ------------------------------------------------------------------------ */

/* Find the start of the (zero-based) Nth top-level object in a JSON array.
 * Returns a pointer at the object's opening '{', or NULL if there is no Nth
 * object. Only scans top-level braces, so nested objects would confuse it —
 * the wh_phys/wh_creative arrays are flat one-level objects, which is all
 * this needs to handle. */
static const char *json_array_object(const char *arr, int n)
{
    if (!arr) return NULL;
    int idx = 0;
    const char *p = arr;
    while (*p) {
        if (*p == '{') {
            if (idx == n) return p;
            /* Skip to the matching close brace (flat objects — depth 1). */
            const char *q = strchr(p, '}');
            if (!q) return NULL;
            p = q + 1;
            idx++;
            continue;
        }
        p++;
    }
    return NULL;
}

/* Read an integer field `key` out of one JSON object `obj` (which must point
 * at or before the object's '{'). The scan is bounded to the object by
 * stopping at its closing '}'. Returns `fallback` when the key is absent. */
static int json_obj_int(const char *obj, const char *key, int fallback)
{
    if (!obj) return fallback;
    const char *end = strchr(obj, '}');
    const char *p = strstr(obj, key);
    if (!p || (end && p > end)) return fallback;
    p = strchr(p, ':');
    if (!p || (end && p > end)) return fallback;
    return atoi(p + 1);
}

/* Read a boolean field — accepts true/false (and 1/0). */
static bool json_obj_bool(const char *obj, const char *key, bool fallback)
{
    if (!obj) return fallback;
    const char *end = strchr(obj, '}');
    const char *p = strstr(obj, key);
    if (!p || (end && p > end)) return fallback;
    p = strchr(p, ':');
    if (!p || (end && p > end)) return fallback;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (!strncmp(p, "true", 4))  return true;
    if (!strncmp(p, "false", 5)) return false;
    return atoi(p) != 0;
}

/* Read a float field — atof, bounded to the object. */
static float json_obj_float(const char *obj, const char *key, float fallback)
{
    if (!obj) return fallback;
    const char *end = strchr(obj, '}');
    const char *p = strstr(obj, key);
    if (!p || (end && p > end)) return fallback;
    p = strchr(p, ':');
    if (!p || (end && p > end)) return fallback;
    return (float)atof(p + 1);
}

/* ------------------------------------------------------------------------
 * Config load
 * ------------------------------------------------------------------------ */

bool wormhole_is_wormhole(void)
{
    char form[32] = {0};
    config_get_str_or(CONFIG_KEY_LAMP_FORM, form, sizeof(form), "");
    return strcmp(form, "wormhole") == 0;
}

static int led_count(void)
{
    return led_control_get_count();
}

/* Defaults: strip mode, rings = led_count/24, all-zero physical, default
 * creative {reverse:false, offset:0, brightness:1.0}. */
static void load_defaults(void)
{
    int lc = led_count();
    s_rings = lc / 24;
    if (s_rings < 1) s_rings = 1;
    if (s_rings > WH_MAX_RINGS) s_rings = WH_MAX_RINGS;
    s_mode = WORMHOLE_MODE_STRIP;
    for (int r = 0; r < WH_MAX_RINGS; r++) {
        s_phys[r].face = 0;
        s_phys[r].direction = 0;
        s_phys[r].offset = 0;
        s_creative[r].reverse = false;
        s_creative[r].offset = 0;
        s_creative[r].brightness = 1.0f;
    }
}

bool wormhole_mirror_allowed(void)
{
    /* The geometry gate from docs/wormhole-api.md — all three must hold. */
    if (!wormhole_is_wormhole()) return false;
    int lc = led_count();
    if (lc <= 0 || (lc % 24) != 0) return false;
    if (s_rings != lc / 24) return false;
    return true;
}

/* `bump_gen` controls whether the stream generation is incremented — true for
 * a mode/rings change (closes active streams with 4002), false for a
 * creative-only change (streams stay open, knobs apply on the next frame). */
static void wormhole_reload_internal(bool bump_gen)
{
    if (bump_gen) s_stream_gen++;

    load_defaults();
    s_loaded = true;

    if (!wormhole_is_wormhole()) {
        /* Not a wormhole — defaults are fine and never used. */
        return;
    }

    /* wh_rings — explicit ring count, default led_count/24. */
    int32_t rings = 0;
    if (config_store_get_i32(CONFIG_KEY_WH_RINGS, &rings) == ESP_OK && rings >= 1) {
        s_rings = (int)rings;
        if (s_rings > WH_MAX_RINGS) s_rings = WH_MAX_RINGS;
    }

    /* wh_phys — JSON array of {face,direction,offset}. Missing rings keep the
     * all-zero defaults from load_defaults(). */
    {
        char *buf = (char *)malloc(1024);
        if (buf) {
            if (config_store_get_str(CONFIG_KEY_WH_PHYS, buf, 1024) == ESP_OK && buf[0]) {
                for (int r = 0; r < s_rings; r++) {
                    const char *o = json_array_object(buf, r);
                    if (!o) break;
                    int face = json_obj_int(o, "face", 0);
                    int dir  = json_obj_int(o, "direction", 0);
                    int off  = json_obj_int(o, "offset", 0);
                    s_phys[r].face = (face & 1);
                    s_phys[r].direction = (dir & 1);
                    s_phys[r].offset = (uint8_t)(((off % 24) + 24) % 24);
                }
            }
            free(buf);
        }
    }

    /* wh_creative — JSON array of {reverse,offset,brightness}. Missing rings
     * keep the {reverse:false, offset:0, brightness:1.0} defaults. */
    {
        char *buf = (char *)malloc(1024);
        if (buf) {
            if (config_store_get_str(CONFIG_KEY_WH_CREATIVE, buf, 1024) == ESP_OK && buf[0]) {
                for (int r = 0; r < s_rings; r++) {
                    const char *o = json_array_object(buf, r);
                    if (!o) break;
                    s_creative[r].reverse = json_obj_bool(o, "reverse", false);
                    int off = json_obj_int(o, "offset", 0);
                    s_creative[r].offset = (uint8_t)(((off % 24) + 24) % 24);
                    float br = json_obj_float(o, "brightness", 1.0f);
                    if (br < 0.0f) br = 0.0f;
                    if (br > 1.0f) br = 1.0f;
                    s_creative[r].brightness = br;
                }
            }
            free(buf);
        }
    }

    /* wh_mode — "strip" (default) or "mirror". */
    char mode[16] = {0};
    config_get_str_or(CONFIG_KEY_WH_MODE, mode, sizeof(mode), "strip");
    if (strcmp(mode, "mirror") == 0) {
        if (wormhole_mirror_allowed()) {
            s_mode = WORMHOLE_MODE_MIRROR;
        } else {
            /* Boot fallback — never play into an invalid geometry. */
            ESP_LOGW(TAG, "wh_mode=mirror but geometry gate fails "
                          "(led_count=%d rings=%d) — falling back to strip",
                     led_count(), s_rings);
            s_mode = WORMHOLE_MODE_STRIP;
        }
    } else {
        s_mode = WORMHOLE_MODE_STRIP;
    }
}

void wormhole_reload(void)
{
    wormhole_reload_internal(true);
}

void wormhole_reload_creative(void)
{
    wormhole_reload_internal(false);
}

static void ensure_loaded(void)
{
    if (!s_loaded) wormhole_reload_internal(false);
}

wormhole_mode_t wormhole_mode(void)
{
    ensure_loaded();
    return s_mode;
}

bool wormhole_apply_effect_mode(wormhole_mode_t desired)
{
    if (!wormhole_is_wormhole()) return false;
    ensure_loaded();
    /* Mirror only when the geometry gate allows it; otherwise fall back to
     * strip (mirroring the boot fallback in wormhole_reload_internal). */
    const char *want = (desired == WORMHOLE_MODE_MIRROR && wormhole_mirror_allowed())
                       ? "mirror" : "strip";
    char cur[16] = {0};
    config_get_str_or(CONFIG_KEY_WH_MODE, cur, sizeof(cur), "strip");
    if (strcmp(cur, want) == 0) return false;  /* already there — no churn */
    ESP_LOGI(TAG, "auto-switch wh_mode %s -> %s (effect @mode)", cur, want);
    config_store_set_str(CONFIG_KEY_WH_MODE, want);
    wormhole_reload();  /* re-reads mode/geometry; bumps stream generation */
    return true;
}

int wormhole_rings(void)
{
    ensure_loaded();
    return s_rings;
}

int wormhole_render_pixels(void)
{
    ensure_loaded();
    return (s_mode == WORMHOLE_MODE_MIRROR) ? 24 : (24 * s_rings);
}

uint32_t wormhole_stream_generation(void)
{
    return s_stream_gen;
}

void wormhole_get_phys(int ring, int *face, int *direction, int *offset)
{
    ensure_loaded();
    if (ring < 0 || ring >= WH_MAX_RINGS) {
        if (face) *face = 0;
        if (direction) *direction = 0;
        if (offset) *offset = 0;
        return;
    }
    if (face)      *face = s_phys[ring].face;
    if (direction) *direction = s_phys[ring].direction;
    if (offset)    *offset = s_phys[ring].offset;
}

void wormhole_get_creative(int ring, bool *reverse, int *offset, float *brightness)
{
    ensure_loaded();
    if (ring < 0 || ring >= WH_MAX_RINGS) {
        if (reverse) *reverse = false;
        if (offset) *offset = 0;
        if (brightness) *brightness = 1.0f;
        return;
    }
    if (reverse)    *reverse = s_creative[ring].reverse;
    if (offset)     *offset = s_creative[ring].offset;
    if (brightness) *brightness = s_creative[ring].brightness;
}

/* ------------------------------------------------------------------------
 * The single tiling function — see docs/wormhole-api.md.
 * ------------------------------------------------------------------------ */

void wormhole_expand(const led_color_t *render, int render_pixels,
                     led_color_t *physical, int rings, bool mirror)
{
    if (!render || !physical || rings < 1) return;

    /* render must carry 24 (mirror) or 24*rings (strip) pixels. The src index
     * computed below is always within srcRing*24 .. srcRing*24+23, so this is
     * the only bound that matters. */
    int src_rings = mirror ? 1 : rings;
    if (render_pixels < src_rings * 24) return;

    for (int r = 0; r < rings; r++) {
        int rc = (r < WH_MAX_RINGS) ? r : (WH_MAX_RINGS - 1);
        int src_ring = mirror ? 0 : r;
        int reverse = s_phys[rc].face ^ s_phys[rc].direction ^
                      (mirror ? (s_creative[rc].reverse ? 1 : 0) : 0);
        int shift = (s_phys[rc].offset + (mirror ? s_creative[rc].offset : 0)) % 24;
        float bright = mirror ? s_creative[rc].brightness : 1.0f;

        for (int p = 0; p < 24; p++) {
            int q = reverse ? (23 - p) : p;
            int s = (q + shift) % 24;
            const led_color_t *src = &render[src_ring * 24 + s];
            led_color_t *dst = &physical[r * 24 + p];
            if (mirror && bright != 1.0f) {
                dst->r = (uint8_t)((float)src->r * bright);
                dst->g = (uint8_t)((float)src->g * bright);
                dst->b = (uint8_t)((float)src->b * bright);
            } else {
                *dst = *src;
            }
        }
    }
}
