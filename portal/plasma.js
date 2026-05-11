// Flowing plasma effect — sums three sin waves into a 0..1 brightness mask
// and applies it to baseColor. Pure function of (frame, x, y, baseColor) so
// no globals are needed; the whole field re-renders each tick.
//
// `speed` controls how fast the plasma moves; `scale` controls spatial
// frequency (smaller = larger blobs).

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
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      // Three independent sin layers — sum is in [-3, 3]; remap to [0, 1].
      let v = Math.sin(x * s + t)
            + Math.sin(y * s * 0.7 - t * 0.6)
            + Math.sin((x + y) * s * 0.5 + t * 0.8);
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
