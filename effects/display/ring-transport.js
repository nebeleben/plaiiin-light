// Ring-transport — the "rings come down, lock in, flash, retract" cycle
// (Stargate-style transporter). All LEDs sit at a steady `backlight` glow
// in the lamp's base colour; the rings show up as brighter bands that
// descend from ABOVE the tower into their slots, lock 1 pixel apart
// starting from the second-to-last row, hold briefly, flash a slight
// white-tinted "kawoosh", then retract back up and fully OFF the top of
// the lamp before the cycle waits and repeats.
//
// Both descent and ascent ride a lerp between `offTop` (-2*ringWidth
// normalized, i.e. fully off the top edge) and `finalY`, so rings enter
// from off-screen and leave to off-screen instead of stopping at row 0.
//
// Per ring i (i = 0 first to descend):
//   final row     = h - 2 - i*2     ← fixed 1-pixel gap between rings;
//                                     bottom ring sits 1 row above the
//                                     floor. Cap is floor((h-1)/2) + 1
//                                     rings (4 on an 8-tall tower); extra
//                                     rings beyond that just never render.
//   descend start = i * ringStagger
//   ascent start  = holdEnd + (N-1-i) * ringStagger   ← top ring leaves first
//
// Kept lean on locals (PLBC cap = 32): start/end times recomputed inline
// in the per-ring loop.
//
// byForm tower effect.

// @param ringCount 1..6 = 3 Number of descending rings
// @param ringWidth 0.05..0.4 = 0.15 Ring vertical thickness (fraction of tower height)
// @param descendDuration 0.3..3 = 1.2 Seconds for one ring to travel top → bottom
// @param ringStagger 0..1.5 = 0.4 Seconds between consecutive ring starts (smaller = more overlap)
// @param easing 1..5 = 2.5 Motion curve power (1 = linear; higher = stronger ease-out — fast at start, slow at end — on both descent and ascent)
// @param holdDuration 0.3..3 = 1.2 Seconds rings stay locked in place (blink centred mid-hold)
// @param blinkDuration 0.1..1.5 = 0.4 Seconds the kawoosh flash takes (fade in + out)
// @param blinkLighten 0..0.5 = 0.2 Strength of the white-tinted flash
// @param waitDuration 0..3 = 1 Seconds between cycles
// @param backlight 0..1 = 0.3 Base-color brightness when no ring is at the pixel

function shade(x, y, idx, frame, base, params) {
  let N = floor(params.ringCount);
  if (N < 1) { N = 1; }

  let dd = params.descendDuration;
  let stg = params.ringStagger;
  let descPhase = dd + (N - 1) * stg;
  let holdEnd = descPhase + params.holdDuration;
  let cycle = holdEnd + descPhase + params.waitDuration;
  if (cycle < 0.1) { cycle = 0.1; }

  let t = time * 0.001;
  let tc = t - cycle * floor(t / cycle);
  let py = y / max(1, h - 1);
  /* Target position for "ring is fully off the top" — the ring's soft
   * falloff (1px outside ringWidth) is then entirely above py=0. */
  let offTop = -2 * params.ringWidth;

  let maxRing = 0;
  for (let i = 0; i < N; i++) {
    let finalY = (h - 2 - i * 2) / max(1, h - 1);
    /* Default: ring is at offTop (out of sight). The brightness check below
     * naturally skips it because |py - offTop| / ringWidth ≥ 2 > 1. */
    let ringY = offTop;
    if (tc >= i * stg && tc < i * stg + dd) {
      /* Descent: ease-out from offTop down to finalY. */
      ringY = offTop + (finalY - offTop) * (1 - pow(1 - (tc - i * stg) / dd, params.easing));
    } else if (tc >= i * stg + dd && tc < holdEnd + (N - 1 - i) * stg) {
      ringY = finalY;
    } else if (tc >= holdEnd + (N - 1 - i) * stg && tc < holdEnd + (N - 1 - i) * stg + dd) {
      /* Ascent: same ease-out shape, from finalY back to offTop. */
      ringY = offTop + (finalY - offTop) * pow(1 - (tc - holdEnd - (N - 1 - i) * stg) / dd, params.easing);
    }
    let n = abs(py - ringY) / params.ringWidth;
    if (n < 1) {
      let rb = pow(1 - n, 2);
      if (rb > maxRing) { maxRing = rb; }
    }
  }

  let v = params.backlight + maxRing * (1 - params.backlight);

  // Kawoosh: sin envelope (0→1→0) centred mid-hold.
  let blinkAdd = 0;
  let bp = tc - descPhase - (params.holdDuration - params.blinkDuration) * 0.5;
  if (bp >= 0 && bp < params.blinkDuration) {
    blinkAdd = sinLUT(bp / params.blinkDuration * 3.14159) * params.blinkLighten;
  }

  let r = base.r * v + blinkAdd * 255;
  let g = base.g * v + blinkAdd * 255;
  let bC = base.b * v + blinkAdd * 255;
  if (r > 255)  { r = 255; }
  if (g > 255)  { g = 255; }
  if (bC > 255) { bC = 255; }
  emit(r, g, bC);
}
