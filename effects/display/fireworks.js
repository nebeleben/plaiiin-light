// Fireworks — N rockets launch from the bottom, rise on a 1-pixel trail,
// peak somewhere in the upper half, and explode into an expanding ring of
// sparks that fades to nothing. Each rocket cycles independently with
// hash-keyed launch x, peak height, peak time, and colour. Per-pixel
// stateless: contributions from all rockets are accumulated, brightest
// wins per channel.
//
// Tightly packed on locals (PLBC cap = 32): every block reuses the same
// scratch variables instead of declaring fresh ones.
//
// byForm display effect.

// @param rockets 1..4 = 3 Number of simultaneously-tracked rockets
// @param cycle 1.5..5 = 2.8 Seconds per rocket cycle (rise + explode + cooldown)
// @param risePart 0.2..0.6 = 0.35 Fraction of cycle spent rising
// @param explodePart 0.3..0.8 = 0.5 Fraction of cycle the explosion lasts (after rise)
// @param sparkRadius 2..6 = 4 Final spark-ring radius in pixels
// @param trail 0..1 = 0.6 Brightness of the rising trail

function shade(x, y, idx, frame, base, params) {
  let t = time * 0.001;
  let N = floor(params.rockets);
  if (N < 1) { N = 1; }

  let rOut = 0; let gOut = 0; let bOut = 0;

  /* Scratch reused across phases of each rocket. */
  let pp = 0;
  let lx = 0;
  let ly = 0;
  let stop = 0;
  let v = 0;
  let cr = 0; let cg = 0; let cbl = 0;

  for (let i = 0; i < N; i++) {
    /* Per-rocket cycle phase, staggered so they don't launch together. */
    pp = t / params.cycle + i / N;
    let gen = floor(pp);
    pp = pp - gen;

    /* Per-cycle randomised launch column, peak height, colour stop. */
    lx = floor(2 + hash(i * 17 + gen) * (w - 4));
    ly = floor(2 + hash(i * 17 + gen + 101) * (h * 0.5));
    stop = floor(hash(i * 17 + gen + 211) * 6);

    if (pp < params.risePart) {
      /* Rising trail: head Y goes h-1 → ly linearly across rise phase.
       * Trail = the column from head down to the panel floor, dimmed below. */
      let curY = (h - 1) - (pp / params.risePart) * ((h - 1) - ly);
      if (x == lx && y >= curY && y <= h - 1) {
        v = (params.trail * 0.5 + 0.5 * (1 - (y - curY) / (h - curY))) * 255;
        /* Warm-white trail. */
        if (v > rOut) { rOut = v; }
        if (v * 0.8 > gOut) { gOut = v * 0.8; }
        if (v * 0.4 > bOut) { bOut = v * 0.4; }
      }
    } else if (pp < params.risePart + params.explodePart) {
      /* Explosion: ring expanding from peak; brightness fades with age. */
      let ep = (pp - params.risePart) / params.explodePart;
      /* ring distance from this pixel to the peak. */
      v = sqrt((x - lx) * (x - lx) + (y - ly) * (y - ly));
      v = abs(v - ep * params.sparkRadius);
      if (v < 1) {
        v = (1 - v) * (1 - ep) * (1 - ep) * 255;
        /* 6-stop primary colour wheel. */
        cr = 0; cg = 0; cbl = 0;
        if (stop == 0)       { cr = 1; }
        else if (stop == 1)  { cr = 1; cg = 1; }
        else if (stop == 2)  { cg = 1; }
        else if (stop == 3)  { cg = 1; cbl = 1; }
        else if (stop == 4)  { cbl = 1; }
        else                 { cr = 1; cbl = 1; }
        if (v * cr  > rOut) { rOut = v * cr; }
        if (v * cg  > gOut) { gOut = v * cg; }
        if (v * cbl > bOut) { bOut = v * cbl; }
      }
    }
    /* Cooldown phase: nothing for this rocket. */
  }

  emit(rOut, gOut, bOut);
}
