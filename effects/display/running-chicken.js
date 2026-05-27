// Running chicken — faithful 8×8 pixel-art adapted from
// lampos/running_chicken.gif. 3-frame walk cycle (legs together → spread
// → push-off) loops at `strideRate` Hz; the whole sprite scrolls
// horizontally at `speed` px/s. The chicken sits in the bottom half of
// the 16×16 panel by default.
//
// Encoding: each of the 8 rows of each of the 3 frames is stored as a
// base-6 packed integer in frame state (8 palette indices × 8 columns =
// 6^8 ≈ 1.7M, fits in a float). At render time the sprite-local row is
// selected by (frame_phase, cy), then the cx-th base-6 digit is extracted
// with floor/mod. This keeps the per-pixel work to one row lookup plus
// one division + one modulo, instead of a sprawling if-cascade.
//
// Palette (matches the original gif):
//   0 = transparent (background)   #00b800 green in the gif
//   1 = black (outline)
//   2 = comb red                   #a80020
//   3 = white (body)
//   4 = orange (beak / feet)       #f8b800
//   5 = lavender (belly / wing)    #b8b8f8
//
// byForm display effect.

// @param speed -10..10 = 4 Scroll speed in pixels per second (0 = static)
// @param strideRate 1..6 = 3 Run-cycle frames per second (3 frames per cycle)
// @param anchorY -4..8 = 8 Top row of the 8-row sprite (0 = top of panel)
// @param bgTint 0..1 = 0 Background blend toward base colour

// @state r0f0 : 0
// @state r1f0 : 9072
// @state r2f0 : 65016
// @state r3f0 : 74346
// @state r4f0 : 491947
// @state r5f0 : 74659
// @state r6f0 : 15522
// @state r7f0 : 2196
// @state r0f1 : 9072
// @state r1f1 : 65016
// @state r2f1 : 74346
// @state r3f1 : 491947
// @state r4f1 : 74659
// @state r5f1 : 15522
// @state r6f1 : 79422
// @state r7f1 : 8142
// @state r0f2 : 9072
// @state r1f2 : 65016
// @state r2f2 : 74340
// @state r3f2 : 71970
// @state r4f2 : 491947
// @state r5f2 : 74659
// @state r6f2 : 15522
// @state r7f2 : 13326

function shade(x, y, idx, frame, base, params) {
  let t = time * 0.001;

  /* Sprite is 8 wide; wrap with span = w + 8 so it enters/exits cleanly. */
  let span = w + 8;
  let pos = t * params.speed;
  pos = pos - span * floor(pos / span);
  let anchor = pos - 8;

  let cx = x - anchor;
  let cy = y - params.anchorY;

  /* Outside the 8×8 sprite cell → background. */
  if (cx < 0 || cx > 7 || cy < 0 || cy > 7) {
    emit(base.r * params.bgTint, base.g * params.bgTint, base.b * params.bgTint);
  } else {
    /* Frame phase: 0, 1, 2 cycled at strideRate × 3 per second. */
    let ph = floor(t * params.strideRate * 3);
    ph = ph - 3 * floor(ph / 3);

    /* Pick the right packed row for (ph, cy). 24 cases — verbose but each
     * branch compiles to just a couple of bytecode ops. */
    let enc = 0;
    if (ph == 0) {
      if      (cy == 0) { enc = r0f0; }
      else if (cy == 1) { enc = r1f0; }
      else if (cy == 2) { enc = r2f0; }
      else if (cy == 3) { enc = r3f0; }
      else if (cy == 4) { enc = r4f0; }
      else if (cy == 5) { enc = r5f0; }
      else if (cy == 6) { enc = r6f0; }
      else              { enc = r7f0; }
    } else if (ph == 1) {
      if      (cy == 0) { enc = r0f1; }
      else if (cy == 1) { enc = r1f1; }
      else if (cy == 2) { enc = r2f1; }
      else if (cy == 3) { enc = r3f1; }
      else if (cy == 4) { enc = r4f1; }
      else if (cy == 5) { enc = r5f1; }
      else if (cy == 6) { enc = r6f1; }
      else              { enc = r7f1; }
    } else {
      if      (cy == 0) { enc = r0f2; }
      else if (cy == 1) { enc = r1f2; }
      else if (cy == 2) { enc = r2f2; }
      else if (cy == 3) { enc = r3f2; }
      else if (cy == 4) { enc = r4f2; }
      else if (cy == 5) { enc = r5f2; }
      else if (cy == 6) { enc = r6f2; }
      else              { enc = r7f2; }
    }

    /* Extract the cx-th base-6 digit. */
    let div = floor(enc / pow(6, cx));
    let digit = div - 6 * floor(div / 6);

    if (digit == 0) {
      emit(base.r * params.bgTint, base.g * params.bgTint, base.b * params.bgTint);
    } else if (digit == 1) {
      emit(0, 0, 0);
    } else if (digit == 2) {
      emit(168, 0, 32);
    } else if (digit == 3) {
      emit(255, 255, 255);
    } else if (digit == 4) {
      emit(248, 184, 0);
    } else {
      emit(184, 184, 248);
    }
  }
}
