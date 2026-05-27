// Dancing chicken — a 16×16 procedural pixel-art chicken that bobs to a
// beat. The whole bird offsets vertically on a sin wave, the head wiggles
// side to side on a faster sin (out of phase with the body), and the comb
// pulses brightness on the beat. The chicken is drawn from a handful of
// simple shape tests (rectangles + one squashed ellipse for the body), so
// there are no sprite arrays — pure per-pixel math fits in PLBC's local
// budget.
//
// Body parts (chicken-local coords, origin top-left at panel coords 0,0):
//   legs   x ∈ {6, 10},  y ∈ 12..14   orange
//   body   ellipse around (8, 9.5), rx=3, ry=2.5   yellow
//   wing   3×2 patch on body side    darker yellow
//   head   circle around (9, 5) r=2  white
//   comb   x=9..11, y=2..3           red (pulses)
//   eye    single pixel              black
//   beak   x=11..12, y=5             orange
//
// byForm display effect.

// @param beat 0.5..6 = 2 Beats per second (controls bob and wiggle speed)
// @param bobAmp 0..2 = 1 Vertical bob amplitude in pixels
// @param wiggleAmp 0..2 = 1 Head wiggle amplitude in pixels
// @param combPulse 0..1 = 0.6 Comb brightness pulse depth (0 = steady, 1 = full)
// @param bgTint 0..1 = 0 Background blend toward base colour (0 = black, 1 = full base)

function shade(x, y, idx, frame, base, params) {
  let t = time * 0.001;
  /* Whole-chicken vertical bob. Round so the sprite snaps to pixel rows —
   * sub-pixel chickens look fuzzy at 16×16. */
  let bob = round(sinLUT(t * params.beat) * params.bobAmp);
  /* Head wiggle: 2× the body beat, +90° phase offset → the head leads the
   * bob, like a real chicken's pecking-and-bobbing cadence. */
  let wiggle = round(sinLUT(t * params.beat * 2 + 1.57) * params.wiggleAmp);

  /* Chicken-local coords: subtract bob from y so the whole bird floats. */
  let cy = y - bob;
  let cx = x;
  /* Head coords also shift horizontally by wiggle. */
  let hx = cx - wiggle;

  let r = 0; let g = 0; let bC = 0; let hit = 0;

  /* --- legs (lowest, drawn first so body covers their top) -------------- */
  if (cy >= 12 && cy <= 14 && (cx == 6 || cx == 10)) {
    r = 255; g = 110; bC = 0; hit = 1;
  }

  /* --- body: ellipse (dx/3)^2 + (dy/2.5)^2 <= 1 ------------------------- */
  if (!hit && cy >= 7 && cy <= 12) {
    let dx = cx - 8;
    let dy = cy * 2 - 19;   /* (cy - 9.5) * 2 — avoid float to keep ints */
    if (dx * dx * 25 + dy * dy * 9 <= 225) {
      r = 255; g = 220; bC = 60;
      /* Wing patch on the right side of the body — darker yellow. */
      if (cx >= 9 && cx <= 11 && cy >= 9 && cy <= 10) {
        r = 200; g = 160; bC = 30;
      }
      hit = 1;
    }
  }

  /* --- head: circle r=2 around (9, 5), shifted by wiggle ---------------- */
  if (!hit) {
    let hdx = hx - 9;
    let hdy = cy - 5;
    if (hdx * hdx + hdy * hdy <= 4) {
      r = 245; g = 245; bC = 235;
      /* Eye: one black pixel on the head. */
      if (hx == 10 && cy == 4) { r = 0; g = 0; bC = 0; }
      hit = 1;
    }
  }

  /* --- comb: 2×2 red tuft on top of head, pulses with beat -------------- */
  if (!hit && hx >= 9 && hx <= 10 && cy >= 2 && cy <= 3) {
    let pulse = 1 - params.combPulse * 0.5 * (1 + sinLUT(t * params.beat));
    r = 230 * pulse; g = 20 * pulse; bC = 30 * pulse;
    hit = 1;
  }

  /* --- beak: 2 orange pixels right of head ------------------------------ */
  if (!hit && hx == 11 && cy == 5) {
    r = 255; g = 140; bC = 0; hit = 1;
  }

  if (hit) {
    emit(r, g, bC);
  } else {
    /* Background: black, optionally tinted with base colour. */
    emit(base.r * params.bgTint, base.g * params.bgTint, base.b * params.bgTint);
  }
}
