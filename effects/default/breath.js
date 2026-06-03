// Smooth sinusoidal whole-panel breath — all LEDs in unison, bright then
// dim then bright. Mild gamma squashing on the wave so the transition
// dwells longer near the dim end, which looks more like a real breath
// than a pure sine.
//
// `frequency` is breaths per second: 0.25 ≈ one breath every 4 s, which
// roughly matches a relaxed human breathing rate.

// @param frequency 0.05..2.0 = 0.25 Breaths per second
// @param floor 0..0.5 = 0.25 Minimum brightness (lifts the dark valleys)

function shade(x, y, idx, frame, base, params) {
  // 2π / 1000 ≈ 0.00628318 — combine wall-clock ms × frequency × 2π.
  let phase = time * params.frequency * 0.00628318;
  let s = (sinLUT(phase) + 1) * 0.5;        // 0..1 linear
  let bright = s * s;                        // squared → smoother low end
  bright = params.floor + bright * (1 - params.floor);
  emitBright(bright);
}
