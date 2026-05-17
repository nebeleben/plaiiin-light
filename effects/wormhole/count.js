// Count — direction / serpentine debugging effect. Walks a single bright
// head LED from logical index 0 up to the last LED, leaving every LED it
// passed lit white behind it. The leading LED is red so the direction of
// travel is unmistakable.
//
// Use it to verify wiring config: the white fill advances in *logical* index
// order, so a wrong `serpentine` / `origin` / `rotation` setting shows up as
// a visible zigzag or tear instead of a clean raster sweep.
//
// `speed` is LEDs advanced per frame. At 30 fps, speed 1 ≈ 30 LEDs/s.
//
// byForm / debug-only — installed at profile-burn time, not a general
// built-in.

// @param speed 0.2..8 = 1 LEDs advanced per frame

function shade(x, y, idx, frame, base, params) {
  let total = w * h;
  // Head walks 0..total, then wraps (one all-white frame marks the cycle).
  let head = floor(frame * params.speed) % (total + 1);
  if (idx < head) {
    emit(255, 255, 255);   // already counted — white trail
  } else if (idx == head) {
    emit(255, 0, 0);       // current head — red, shows travel direction
  } else {
    emit(0, 0, 0);         // not yet reached
  }
}
