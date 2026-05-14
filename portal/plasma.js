// Flowing plasma effect — three sin layers per pixel. Pure shader (no
// state), exactly the kind of pattern the DSL was built for.
//
// `speed` controls how fast the plasma moves; `scale` controls spatial
// frequency (smaller = larger blobs); `floor` lifts the dark valleys.

// @param speed 0.005..0.2 = 0.05 Animation speed
// @param scale 0.1..1.5 = 0.55 Spatial frequency (smaller = bigger blobs)
// @param floor 0..0.5 = 0.05 Minimum brightness (lifts the dark valleys)

function shade(x, y, idx, frame, base, params) {
  let t = frame * params.speed;
  let v = sinLUT(x * params.scale + t)
        + sinLUT(y * params.scale * 0.7 - t * 0.6)
        + sinLUT((x + y) * params.scale * 0.5 + t * 0.8);
  let bright = (v + 3) / 6;
  bright = params.floor + bright * (1 - params.floor);
  emitBright(bright);
}
