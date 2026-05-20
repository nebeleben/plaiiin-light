// @mode strip
//
// AP-mode onboarding indicator — soft blue breath across the full wormhole.
// Same calm-blue vibe as the built-in error_light fallback (slow blue pulse
// on LEDs 0..2), but spread over the whole construct so it reads as "I'm
// waiting to be set up" rather than a broken-LED dot. Firmware caps overall
// brightness to ~30 % while in AP mode so this is comfortable to leave on
// for hours during onboarding.
//
// Strip mode: the grid is the flat 96-LED strip (24 LEDs × 4 rings = idx
// 0..95). We don't need per-ring logic — the whole construct breathes in
// unison.

// @param period 1..10 = 3.5  Seconds per full breath
// @param peak   0.05..1 = 0.7 Peak intensity of the breath (0..1)
// @param floor  0..0.3 = 0.04 Minimum intensity (so it never goes fully dark)

function shade(x, y, idx, frame, base, params) {
  // Phase 0..1 through the breath cycle.
  let t = time * 0.001;
  let cycles = t / params.period;
  let phase = cycles - floor(cycles);

  // Half-sine 0 → 1 → 0 over one cycle. sinLUT(phase * 2π) gives a full sine
  // wave; |…| folds the second half back upward into a single soft hump.
  let s = sinLUT(phase * 6.28318);
  if (s < 0) { s = -s; }

  // Square the hump so the transition dwells longer near the dim end —
  // feels more like a real breath than a pure abs(sine).
  let bright = s * s * params.peak;
  bright = params.floor + bright * (1 - params.floor);

  // Cool calm blue, slightly cyan-tilted so it reads as "waiting" not "alarm".
  emit(0, 30 * bright, 255 * bright);
}
