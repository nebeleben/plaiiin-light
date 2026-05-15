// Stateful twinkle / sparkle. Each LED carries its own `energy` slot;
// every frame the energy decays multiplicatively, and with `spawnRate`
// probability the LED re-ignites to full brightness. Different LEDs fire
// at different moments, so the panel looks like glittering points whose
// individual decays are independent.
//
// Tunables:
//   * `decay`   — fraction of energy kept per frame. Higher = longer fade.
//                 At fps=30, decay=0.94 takes ~1.1 s to fall to 1/e.
//   * `spawnRate` — per-LED ignition probability per frame. With 256 LEDs
//                  at fps=30, spawnRate=0.003 gives ~23 new sparks/sec
//                  across the panel.

// @state.pixel energy: 0

// @param decay 0.85..0.99 = 0.94 Energy kept per frame (higher = longer tail)
// @param spawnRate 0.0005..0.05 = 0.003 Per-LED ignition probability per frame

function shade(x, y, idx, frame, base, params) {
  let e = energy.pixel * params.decay;
  if (random() < params.spawnRate) {
    e = 1;
  }
  energy.pixel = e;
  emitBright(e);
}
