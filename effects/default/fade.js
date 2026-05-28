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
  // `time` is wall-clock ms since /api/play, advanced by the player every
  // frame from esp_timer. Using it (instead of `frame * dt`) keeps
  // fadeIn/hold/fadeOut in real seconds regardless of render fps.
  let now = time;
  let active = params.fadeIn + params.hold + params.fadeOut;
  let holdEnd = params.fadeIn + params.hold;

  // Cycle length is ~active (the fade-in/hold/out trio back-to-back) with
  // a small ±5% per-LED jitter so the panel never globally re-syncs.
  // Crucially this is INDEPENDENT of density — at low density the per-LED
  // cycle still feels normal, density just controls how often each LED
  // chooses to participate.
  let cycle = active * (1 + hash(idx + 999) * 0.1);

  // Per-LED phase offset, reseeded each playback via playStart so back-to-
  // back runs look different.
  let offset = hash(idx + playStart) * cycle;
  let totalPos = now + offset;
  let local = totalPos % cycle;

  // Per-cycle participation dice roll. Every time this LED reaches the end
  // of its cycle (local wraps back to 0), a new dice is rolled: with
  // probability `density` the LED takes part in this cycle; otherwise it
  // stays dark for the whole cycle. The re-roll lines up with the cycle
  // boundary — at which point bright was naturally ~0 from the prior
  // fade-out — so there's no visual glitch.
  let cycleNum = floor(totalPos / cycle);
  let participates = hash(idx * 257 + cycleNum) < params.density;

  let bright = 0;
  if (participates) {
    if (local < params.fadeIn) {
      bright = local / params.fadeIn;
    } else if (local < holdEnd) {
      bright = 1;
    } else if (local < active) {
      bright = 1 - (local - holdEnd) / params.fadeOut;
    }
    // local ∈ [active, cycle) is the 0–5% jitter tail → stays dark
  }
  emitBright(bright);
}
