// Drops — water beads running down a window. Smooth background light in the
// lamp's base colour, with sparse vertical streaks (a drop head with a
// fading trail above it) sliding from top to bottom. Each drop rolls its
// OWN wobble decision (amplitude in [0, wobbleAmp] and a phase, both keyed
// on the drop's generation hash) — so within the same column some drops
// fall nearly straight, others swing the full width. Streaks are a
// hue-rotated tint of the base; backlight pixels stay pure base, so drops
// only stand out by colour, not by brightness.
//
// To let a wobbling drop visibly cross column boundaries, the shader checks
// the ±2 lanes around each pixel and takes the strongest drop contribution
// — combined with a soft horizontal falloff (`dropWidth`) this lets a drop
// straddle adjacent columns when it wobbles between them.
//
// byForm tower effect.

// @param speed 0.5..10 = 3 Average drop fall speed (rows per second)
// @param trailLength 0.1..1 = 0.5 Trail length as fraction of tower height
// @param density 0.05..0.6 = 0.25 Fraction of time each column shows a drop
// @param dropWidth 0.5..3 = 1.2 Drop horizontal width (pixels, soft falloff)
// @param wobbles 0..6 = 2 Number of lateral wobbles during the fall (0 = straight)
// @param wobbleAmp 0..3 = 1.5 Max wobble amplitude in pixels — each drop draws an actual amplitude from [0, wobbleAmp]
// @param hueShift 0..0.3 = 0.12 Hue rotation at the drop head (cycles)

function shade(x, y, idx, frame, base, params) {
  let t = time * 0.001;
  let trailLen = params.trailLength * (h - 1);
  if (trailLen < 1) { trailLen = 1; }
  /* Cache hot params so the loop body avoids repeated struct loads.
   * invDensity converts the per-drop position division into a multiply. */
  let density = params.density;
  let invDensity = 1 / density;

  let dropBoost = 0;
  /* Sweep ±2 lanes so a drop wobbling out of its home column lights up the
   * neighbouring pixels too. Each lane runs its own deterministic drop
   * sequence keyed by hash(lane). */
  for (let dx = -2; dx <= 2; dx++) {
    let lane = x + dx;
    let speed = params.speed * (0.7 + hash(lane) * 0.6);
    let period = (h + trailLen) / (speed * density);
    let phase = (t + hash(lane + 7777) * 100) / period;
    let gen = floor(phase);
    phase = phase - gen;

    if (phase < density) {
      let dropY = phase * invDensity * (h + trailLen) - trailLen;
      /* Wobble is a sine of dropY (so it depends on vertical position, not
       * time — the drop traces a stable wandering path as it falls).
       * `wobbles` cycles per fall. Per-drop randomisation: the amplitude
       * is scaled by hash(lane*31+gen) (= 0..1), and the wobble phase is
       * offset by hash(lane*31+gen+13) — so each new drop in a column
       * decides afresh whether to wobble much, a little, or barely at all. */
      let wobble = sinLUT(dropY / max(1, h - 1) * params.wobbles * 6.28318 + hash(lane * 31 + gen + 13) * 6.28318) * params.wobbleAmp * hash(lane * 31 + gen);
      let xFall = 1 - abs(x - lane - wobble) / params.dropWidth;
      let dy = y - dropY;
      if (dy <= 0 && dy > -trailLen && xFall > 0) {
        /* x*x instead of pow(x, 2) — pow() is a general exp/log
         * combination, much slower than one multiply. */
        let f = 1 + dy / trailLen;
        let boost = xFall * f * f;
        if (boost > dropBoost) { dropBoost = boost; }
      }
    }
  }

  /* Hue rotation around base, scaled by dropBoost so backlight stays pure.
   * Skip the matrix entirely when dropBoost==0 — most pixels are backlight,
   * so this short-circuit saves 1 cosLUT + 1 sinLUT + 9 mults + 6 clamps
   * per skipped pixel. */
  if (dropBoost > 0) {
    let theta = dropBoost * params.hueShift * 6.28318;
    let cT = cosLUT(theta);
    let sT = sinLUT(theta);
    let k = 1 - cT;
    let k3 = k * 0.33333;
    let sR = sT * 0.57735;

    let br = base.r * 0.00392;
    let bg = base.g * 0.00392;
    let bb = base.b * 0.00392;

    let rr = br * (cT + k3) + bg * (k3 - sR) + bb * (k3 + sR);
    let gg = br * (k3 + sR) + bg * (cT + k3) + bb * (k3 - sR);
    let bC = br * (k3 - sR) + bg * (k3 + sR) + bb * (cT + k3);

    emit(clamp01(rr) * 255, clamp01(gg) * 255, clamp01(bC) * 255);
  } else {
    emit(base.r, base.g, base.b);
  }
}
