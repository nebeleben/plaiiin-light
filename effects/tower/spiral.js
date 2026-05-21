// Spiral — smooth backlight in the lamp's base colour with a sinus hue
// ripple running vertically up the tower. Per-column phase shift wraps the
// ripple into a helix that cruises along the rocket axis. Every LED stays
// lit; only the hue moves. Overall brightness comes from the colour picker,
// not from this script.
//
// byForm tower effect.

// @param speed -0.8..0.8 = 0.2 Vertical wave travel speed (revs/sec; sign chooses up/down)
// @param phaseShift 0..6 = 2 Horizontal phase offset across the cylinder (full cycles = visible twists)
// @param waves 0.2..2 = 1 Number of colour cycles stacked vertically
// @param hueShift 0..0.2 = 0.08 Max hue rotation from base (cycles; 0 = uniform base colour)

function shade(x, y, idx, frame, base, params) {
  let t = time * 0.001;
  let hdenom = max(1, h - 1);

  let nx = x / w;
  let ny = y / hdenom;

  let phase = ny * params.waves - t * params.speed - nx * params.phaseShift;
  phase = phase - floor(phase);

  let osc = sinLUT(phase * 6.28318);
  let shift = osc * params.hueShift;
  let theta = shift * 6.28318;

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
}
