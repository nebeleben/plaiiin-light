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
  // frame from esp_timer. Using it (instead of the old `frame * dt` with a
  // hard-coded dt=100ms-per-frame) keeps fadeIn/hold/fadeOut in real
  // seconds regardless of the requested render fps.
  let now = time;
  let active = params.fadeIn + params.hold + params.fadeOut;
  let nominalCycle = active / max(params.density, 0.01);
  let holdEnd = params.fadeIn + params.hold;

  // Phase 23 — two axes of randomization:
  //   (1) Per-LED *cycle jitter*: each LED's total period is nominalCycle
  //       ×(1.0 .. 1.1), so neighbouring LEDs run at incommensurate
  //       frequencies and the composite pattern never exactly repeats.
  //       Without this, after `nominalCycle` ms the whole panel replays
  //       its sequence verbatim — you'd see the same LED-by-LED schedule
  //       on loop.
  //   (2) Per-LED *offset shuffle*, re-seeded each playback via playStart
  //       (ms-since-boot at /api/play), so a Stop / Play also gives a
  //       fresh distribution.
  // Both hashes go through the VM's Wang mixer (HASH_I opcode) — small
  // input changes avalanche to uncorrelated outputs.
  let cycle = nominalCycle * (1 + hash(idx + 999) * 0.1);
  let offset = hash(idx + playStart) * cycle;
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
