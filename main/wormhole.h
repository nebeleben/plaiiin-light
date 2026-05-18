#pragma once

#include "led_control.h"
#include "esp_err.h"
#include <stdbool.h>

/**
 * Phase 29 — wormhole lamp render mode.
 *
 * A wormhole lamp is N rings of 24 LEDs. The physical chain places `ring r`
 * at physical indices `r*24 .. r*24+23`. Two render modes:
 *
 *  - strip  — the effect renders the whole construct. Render grid =
 *             24 x ringCount; a script sees x = position-on-ring, y = ring.
 *             The default, out-of-the-box mode.
 *  - mirror — the effect renders ONE 24-LED ring; firmware tiles that ring
 *             onto every physical ring with per-ring transforms. Firmware-only;
 *             the VM and shade() never see it.
 *
 * The one architectural change is decoupling the render grid from the physical
 * strip: the player renders a `wormhole_render_pixels()`-pixel render buffer,
 * then `wormhole_expand()` produces the `led_count`-pixel physical buffer that
 * goes to led_control. `wormhole_expand()` is the single tiling function used
 * by BOTH the JS player and the WebSocket stream path.
 *
 * In strip mode with default physical config (all rings face=0, direction=0,
 * offset=0) `wormhole_expand()` is a byte-identical copy — existing wormhole
 * builds are unaffected.
 *
 * All config lives in NVS (CONFIG_KEY_WH_*); see docs/wormhole-api.md for the
 * frozen contract.
 */

typedef enum {
    WORMHOLE_MODE_STRIP = 0,   // effect renders the whole construct (default)
    WORMHOLE_MODE_MIRROR = 1,  // effect renders one ring, firmware tiles it
} wormhole_mode_t;

/** True when lamp_form == "wormhole". All other wormhole_* calls are only
 *  meaningful for a wormhole lamp; callers gate on this first. */
bool wormhole_is_wormhole(void);

/** (Re-)read wh_mode/wh_rings/wh_phys/wh_creative from config_store and apply
 *  the boot fallback: if wh_mode == "mirror" but the geometry gate fails, fall
 *  back to strip and log a warning. Call once at startup and again after any
 *  POST /api/wormhole. Bumps the stream generation counter so an active
 *  WebSocket stream can detect a mid-stream mode change. Safe to call when the
 *  lamp is not a wormhole — it is then a cheap no-op. */
void wormhole_reload(void);

/** Like wormhole_reload() but does NOT bump the stream generation. Used after
 *  POST /api/wormhole/creative — creative knobs take effect on the next frame
 *  and never close an active stream (the render geometry is unchanged). */
void wormhole_reload_creative(void);

/** Effective render mode after the geometry gate. */
wormhole_mode_t wormhole_mode(void);

/** Explicit ring count (wh_rings, default led_count / 24, >= 1). */
int wormhole_rings(void);

/** Pixels the effect / stream client must render: 24 in mirror mode,
 *  24 * rings in strip mode. */
int wormhole_render_pixels(void);

/** The geometry gate from docs/wormhole-api.md: mirror mode is allowed only
 *  when lamp_form == "wormhole" AND led_count % 24 == 0 AND
 *  wh_rings == led_count / 24. */
bool wormhole_mirror_allowed(void);

/** Stream generation counter. wormhole_reload() bumps it on every call so the
 *  WebSocket handler can snapshot it when a stream opens and detect a
 *  mid-stream mode/rings change (close code 4002). */
uint32_t wormhole_stream_generation(void);

/** Per-ring physical config accessor (ring 0..rings-1). Out params may be
 *  NULL. Rings out of range yield the all-zero default. */
void wormhole_get_phys(int ring, int *face, int *direction, int *offset);

/** Per-ring creative config accessor (ring 0..rings-1). Out params may be
 *  NULL. Rings out of range yield {reverse:false, offset:0, brightness:1.0}. */
void wormhole_get_creative(int ring, bool *reverse, int *offset, float *brightness);

/** The single tiling function — used by both the JS player and the WebSocket
 *  stream path. For each ring r (0..rings-1) and local position p (0..23):
 *
 *      srcRing = mirror ? 0 : r
 *      reverse = face[r] ^ direction[r] ^ (mirror ? creativeReverse[r] : 0)
 *      shift   = (physOffset[r] + (mirror ? creativeOffset[r] : 0)) mod 24
 *      bright  = mirror ? creativeBrightness[r] : 1.0
 *      q = reverse ? (23 - p) : p
 *      s = (q + shift) mod 24
 *      physical[r*24 + p] = render[srcRing*24 + s] * bright
 *
 *  `render`   carries `render_pixels` pixels (24 in mirror, 24*rings in strip).
 *  `physical` receives `rings * 24` pixels. All index arithmetic is mod-24 and
 *  in-bounds by construction. In strip mode with default physical config this
 *  is a byte-identical copy. */
void wormhole_expand(const led_color_t *render, int render_pixels,
                     led_color_t *physical, int rings, bool mirror);
