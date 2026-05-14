// Random fade-in / hold / fade-out per LED. Each LED runs on its own phase
// inside a (fadeIn + hold + fadeOut + dark gap) cycle, offset by a
// deterministic per-LED hash — so the panel looks like independent
// breathing LEDs.
//
// Phase 23 — DSL syntax. Stateless: per-LED phase is derived from idx, so
// no @state needed. The whole shader compiles to ~50 bytes of bytecode.
// Density is encoded as the fraction of the cycle that's "active": at
// density=1.0 every LED is always doing one of the three phases; at 0.5
// each LED spends half the cycle dark.

// @param density 0..1 = 0.75 Fraction of LEDs animating concurrently
// @param fadeIn 50..15000 = 400 Fade-in duration in ms
// @param hold 0..15000 = 200 Time at peak brightness in ms
// @param fadeOut 50..20000 = 800 Fade-out duration in ms

function shade(x, y, idx, frame, base, params) {
  let dt = 100;                              // ~10 fps tick, exact value isn't critical
  let now = frame * dt;
  let active = params.fadeIn + params.hold + params.fadeOut;
  let cycle = active / max(params.density, 0.01);
  let holdEnd = params.fadeIn + params.hold;

  // Per-LED phase offset, scattered via hash. hash(i) ∈ [0,1) → mul by cycle.
  let offset = hash(idx) * cycle;
  let t = (now + offset) % cycle;

  let bright = 0;
  if (t < params.fadeIn) {
    bright = t / params.fadeIn;
  } else if (t < holdEnd) {
    bright = 1;
  } else if (t < active) {
    bright = 1 - (t - holdEnd) / params.fadeOut;
  }
  // else: in the dark gap → bright stays 0

  emitBright(bright);
}
