// Horse ride — a galloping-horse silhouette scrolls across the panel. The
// horse is drawn as a procedural silhouette (body ellipse + neck rectangle
// + head + tail) in a single colour, with four legs that cycle through a
// 4-phase Muybridge-style gallop. Dust puffs trail from the rear hooves on
// the push-off phases.
//
// Gallop phases (cycled at strideRate × 4 per second):
//   0 = front legs forward, hind legs back (full extension)
//   1 = legs gathered underneath (the suspended-in-air moment)
//   2 = front legs back, hind legs forward (push-off)
//   3 = legs gathered again (second suspension)
//
// byForm display effect.

// @param speed 2..14 = 7 Scroll speed in pixels per second (sign chooses direction)
// @param strideRate 1.5..6 = 3 Gallop strides per second
// @param dust 0..1 = 0.6 Dust trail strength under rear hooves
// @param bodyHue 0..1 = 0.08 Horse silhouette hue (0 = pure base colour, 1 = brown)
// @param bgTint 0..1 = 0 Background blend toward base colour

function shade(x, y, idx, frame, base, params) {
  let t = time * 0.001;

  /* Scroll anchor with wrap; sprite ≈ 14 px wide so off-panel both sides. */
  let span = w + 16;
  let pos = t * params.speed;
  pos = pos - span * floor(pos / span);
  let anchor = pos - 8;
  let cx = x - anchor;
  let cy = y;

  /* Gallop phase 0..3. */
  let ph = floor(t * params.strideRate * 4);
  ph = ph - 4 * floor(ph / 4);

  /* Final colour accumulator. kind=0 background, 1 horse, 2 dust. */
  let r = 0; let g = 0; let bC = 0; let kind = 0;

  /* --- legs: 1px diagonals from pivot (cy=10) to foot (cy=13) ---------- */
  if (cy >= 10 && cy <= 13) {
    let hindX = 4; let frontX = 10;
    if (ph == 0) { hindX = 2; frontX = 12; }
    else if (ph == 2) { hindX = 6; frontX = 8; }
    let hindLegX = 4 + (hindX - 4) * (cy - 10) / 3;
    let frontLegX = 10 + (frontX - 10) * (cy - 10) / 3;
    if (abs(cx - hindLegX) < 0.6 || abs(cx - frontLegX) < 0.6) {
      kind = 1;
    }
  }

  /* --- body ellipse: centre (7, 8), rx=4, ry=2 ------------------------- */
  if (kind == 0 && cy >= 6 && cy <= 10 && cx >= 3 && cx <= 11) {
    let dx = cx - 7;
    let dy = cy - 8;
    if (dx * dx + dy * dy * 4 <= 16) { kind = 1; }
  }

  /* --- neck (diagonal) and head (2×2) ---------------------------------- */
  if (kind == 0 && cx >= 9 && cx <= 11 && cy >= 4 && cy <= 7) {
    if (cy >= 4 + (11 - cx) * 0.7) { kind = 1; }
  }
  if (kind == 0 && cx >= 11 && cx <= 12 && cy >= 3 && cy <= 4) { kind = 1; }

  /* --- tail: flowing back, slight wave --------------------------------- */
  if (kind == 0 && cx >= 1 && cx <= 3 && cy >= 5 && cy <= 9) {
    let wave = sinLUT(t * params.strideRate * 6 + cy * 0.8) * 0.6;
    let tailX = 3 + wave - (9 - cy) * 0.4;
    if (abs(cx - tailX) < 0.8) { kind = 1; }
  }

  /* --- dust puff behind rear hooves on push-off phases ----------------- */
  if (kind == 0 && params.dust > 0 && (ph == 0 || ph == 2)) {
    if (cy >= 12 && cy <= 14 && cx >= -3 && cx <= 0) {
      let ddx = cx + 1;
      let ddy = cy - 13;
      let d2 = ddx * ddx + ddy * ddy;
      if (d2 <= 4) {
        let f = (1 - d2 / 4) * params.dust;
        r = 200 * f; g = 170 * f; bC = 130 * f;
        kind = 2;
      }
    }
  }

  if (kind == 1) {
    let mix = params.bodyHue;
    let or_ = base.r * (1 - mix) + 30 * mix;
    let og = base.g * (1 - mix) + 18 * mix;
    let ob = base.b * (1 - mix) + 10 * mix;
    emit(or_, og, ob);
  } else if (kind == 2) {
    emit(r, g, bC);
  } else {
    emit(base.r * params.bgTint, base.g * params.bgTint, base.b * params.bgTint);
  }
}
