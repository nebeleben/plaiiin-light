// Fire — a per-pixel reformulation of the classic C++ FirePattern cellular
// automaton (lampos/adaptations/FirePattern.cpp), ported to the PLBC
// shade() subset so it scales to any panel size.
//
// HOW THE ORIGINAL WORKED
//   The C++ kept an H×W `matrixValue` heat buffer. Every cycle it shifted
//   every row up one, dropped a fresh random line (random(64,255)) into the
//   bottom, and cross-faded rows by `pcnt`. A per-size value_mask was
//   subtracted to taper the flame; a hue_mask coloured it via HSV.
//
// HOW THIS READS IT PER-PIXEL
//   A row that shifts up N times is just "the random bottom line generated
//   N cycles ago" — and that is expressible without neighbours or arrays.
//   With  gen  = floor(time / period)  and  pcnt = the fractional remainder,
//   the heat feeding a pixel whose distance from the flame base is `rb`
//   rows is  lerp(noise(x, gen-rb-1), noise(x, gen-rb), pcnt).  `noise` is a
//   deterministic Wang-hash (PLBC `hash` built-in) of (x, generation), so it
//   is stable per column per cycle exactly like the CA's stored buffer.
//
//   The W×H value_mask is replaced by a smooth formula of normalized x/y:
//   heat is cooled more toward the top of the panel and toward the side
//   columns, matching the original mask's ramp. The hue_mask + HSV step is
//   replaced by a direct heat -> RGB fire gradient (black -> red -> orange
//   -> yellow -> white), which is cheaper and scales cleanly.
//
//   The flame base sits at the bottom panel row (y = h-1) and rises toward
//   y = 0, so `rb = (h-1) - y` is the row's age in shift-cycles.
//
// byForm effect — installed at profile-burn time, not a general built-in.

// @param speed 0.2..4 = 1 Flame rise speed (cycles per second-ish)
// @param cooling 0.3..2 = 1 How fast the flame fades toward the top
// @param intensity 0.4..1.6 = 1 Overall brightness / fuel of the flame
// @param baseTint 0..1 = 0 Blend the fire toward the lamp base colour

function shade(x, y, idx, frame, base, params) {
  // --- normalized coordinates (scale-independent) ----------------------
  let wm = max(1, w - 1);
  let hm = max(1, h - 1);
  let nx = x / wm;            // 0..1 across the panel
  // rb = rows above the flame base, 0 at the bottom row, 1 at the top.
  let rb = (hm - y) / hm;

  // --- generation / cross-fade (the CA's shiftUp + pcnt) ---------------
  // period in ms: higher speed -> shorter period -> quicker rise.
  let period = 600 / params.speed;
  let g = floor(time / period);
  let pcnt = (time / period) - g;     // 0..1 fractional remainder

  // age of this row in shift-cycles (an integer rows-from-base count).
  let age = floor(rb * hm + 0.5);

  // --- deterministic per-(column, generation) noise --------------------
  // hash() truncates its arg to int32 then Wang-mixes -> uniform [0,1).
  // x*131 keeps adjacent columns uncorrelated; +g advances per cycle.
  // The CA seeded its line with random(64,255); map hash 0..1 the same.
  let key = x * 131;
  let n0 = 64 + hash(key + (g - age - 1)) * 191;   // older line
  let n1 = 64 + hash(key + (g - age))     * 191;   // newer line
  let heat = (n0 * (1 - pcnt) + n1 * pcnt) / 255;  // 0..1 raw heat

  // --- value_mask reformulation (cool toward top and edges) ------------
  // Original mask: bright at the bottom, darker upward, darker at the side
  // columns vs the centre. Mask is *subtracted*, so build a cooling term.
  let edge = abs(nx - 0.5) * 2;            // 0 centre .. 1 side column
  let topFade = rb * rb;                   // grows toward the top
  let cool = (topFade * (1 + edge * 0.9)) * params.cooling;
  heat = (heat - cool) * params.intensity;
  heat = clamp01(heat);

  // --- heat -> RGB fire gradient (replaces hue_mask + HSV) -------------
  // black -> red -> orange -> yellow -> white as heat rises.
  let r = clamp01(heat * 3) * 255;                  // red ramps in first
  let gch = clamp01(heat * 3 - 1) * 255;            // green follows
  let bch = clamp01(heat * 3 - 2) * 255;            // blue last (whiten)

  // --- optional base-colour tint (the C++ base_mode) -------------------
  // Blend toward the lamp base colour, weighted by heat so dark stays dark.
  let t = params.baseTint * heat;
  r = r * (1 - t) + base.r * t;
  gch = gch * (1 - t) + base.g * t;
  bch = bch * (1 - t) + base.b * t;

  emit(r, gch, bch);
}
