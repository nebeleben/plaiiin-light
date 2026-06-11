// Matrix rain — falling "glyph" columns in the lamp's base colour. Per-
// column stateless: the column index seeds its own speed and start offset
// via hash(), so every column drops at its own pace. The leading pixel
// brightens toward white (controlled by `headWhite`), the trail fades
// behind it. Glyphs flicker by switching brightness on a hash keyed to
// (col, row, integer time) — gives the "characters change as they fall"
// Matrix look without storing actual glyph values.
//
// Colour is driven by the lamp's base colour, so the colour picker turns
// the classic green wall into orange rain, blue rain, etc. — pick the
// classic green (0, 255, 0) in the picker for the canonical Matrix look.
//
// byForm display effect.

// @param speed 0.3..6 = 1.8 Average fall speed (rows per second)
// @param trailLength 0.2..1 = 0.6 Fraction of column height the trail covers
// @param density 0.2..1 = 0.7 Fraction of columns showing a drop at any moment
// @param flicker 0..1 = 0.5 Glyph flicker rate (0 = solid trail, 1 = stuttery)
// @param headWhite 0..1 = 0.7 How much the leading pixel washes out to white (0 = pure base, 1 = full white)

function shade(x, y, idx, frame, base, params) {
  let t = time * 0.001;

  /* Per-column timing: speed jitter ±50% and a random phase offset so
   * columns aren't all in lockstep. */
  let speed = params.speed * (0.5 + hash(x) * 1.0);
  let trailLen = params.trailLength * h;
  if (trailLen < 1) { trailLen = 1; }
  /* The head travels from -trailLen (off-top) to h + trailLen (fully off the
   * bottom) so the trail keeps falling off after the head exits, instead of
   * the whole column blanking the instant the head hits the bottom row. The
   * span is h + 2*trailLen; period scales with it so `speed` stays rows/sec. */
  let span = h + 2 * trailLen;
  let period = span / (speed * params.density);
  let phase = (t + hash(x + 4321) * 100) / period;
  let gen = floor(phase);
  phase = phase - gen;

  let bright = 0;
  let isHead = 0;
  if (phase < params.density) {
    /* Head position: travels from -trailLen (off-top) to h + trailLen (trail
     * fully drained off the bottom). dy = head - y; positive = pixel is in
     * the trail above the head. */
    let head = phase / params.density * span - trailLen;
    let dy = head - y;
    if (dy >= 0 && dy < trailLen) {
      /* Trail brightness: brightest at the head, fades to 0 at the tail.
       * Squared so the head reads as a sharp tip. */
      let f = 1 - dy / trailLen;
      bright = f * f;
      if (dy < 0.6) { isHead = 1; }
      /* Glyph flicker: stutter brightness on a (col, row, time-bucket) hash.
       * Time bucket = 6 Hz × flicker strength so flicker > 0 quantises time. */
      if (params.flicker > 0) {
        let bucket = floor(t * 6 * params.flicker);
        let fk = hash(x * 17 + y * 131 + bucket + gen * 7);
        if (fk < 0.25 * params.flicker) { bright = bright * 0.3; }
      }
    }
  }

  if (bright <= 0) {
    emit(0, 0, 0);
  } else {
    /* Trail = base colour × brightness. Head additionally washes toward
     * white by headWhite, so a bright tip pops even when base is dim. */
    let r = base.r * bright;
    let g = base.g * bright;
    let bC = base.b * bright;
    if (isHead == 1) {
      let hw = params.headWhite;
      r = r * (1 - hw) + 255 * hw;
      g = g * (1 - hw) + 255 * hw;
      bC = bC * (1 - hw) + 255 * hw;
    }
    emit(r, g, bC);
  }
}
