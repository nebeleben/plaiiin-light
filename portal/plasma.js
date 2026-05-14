// Flowing plasma effect — sums three sin waves into a 0..1 brightness mask
// and applies it to baseColor. Pure function of (frame, x, y, baseColor) so
// no globals are needed; the whole field re-renders each tick.
//
// `speed` controls how fast the plasma moves; `scale` controls spatial
// frequency (smaller = larger blobs).
//
// Phase 22 — Calls Math.sinLUT (a 256-entry firmware LUT, ~0.01 error) which
// is ~10× cheaper than Math.sin via mJS marshalling. Earlier we tried
// precomputing row/column sin tables to drop the trig count, but mJS array
// indexing is O(n) — every `tbl[x]` walked the property list — so the
// "optimization" was net slower than just calling the LUT inline. macOS
// preview polyfills Math.sinLUT / cosLUT to Math.sin / cos.

// @param speed 0.005..0.2 = 0.05 Animation speed
// @param scale 0.1..1.5 = 0.55 Spatial frequency (smaller = bigger blobs)
// @param floor 0..0.5 = 0.05 Minimum brightness (lifts the dark valleys)

function render(frame, w, h, baseColor, params) {
  let pixels = [];
  let t = frame * params.speed;
  let s = params.scale;
  let r = baseColor[0];
  let g = baseColor[1];
  let b = baseColor[2];
  let floor = params.floor;
  // Phase 22 — time-only phases hoist out of the inner loop. Three sin calls
  // per pixel remain (one of them — (x+y) — can't be factored without
  // building arrays, and arrays would cost more than they save in mJS).
  let tA = t;
  let tB = -t * 0.6;
  let tC = t * 0.8;
  for (let y = 0; y < h; y++) {
    let yA = y * s * 0.7;
    let yC = y * s * 0.5;
    for (let x = 0; x < w; x++) {
      // Three independent sin layers — sum is in [-3, 3]; remap to [0, 1].
      let v = Math.sinLUT(x * s + tA)
            + Math.sinLUT(yA + tB)
            + Math.sinLUT(x * s * 0.5 + yC + tC);
      let bright = (v + 3) / 6;
      bright = floor + bright * (1 - floor);
      pixels.push([
        Math.floor(r * bright),
        Math.floor(g * bright),
        Math.floor(b * bright)
      ]);
    }
  }
  return pixels;
}
