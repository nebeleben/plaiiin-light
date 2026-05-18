// Smouldering fire — every LED independently eases from its current colour
// to a fresh random colour near baseColor, then rolls a new target. A
// faithful port of the C++ BlazePattern: each pixel has its own start and
// target RGB offsets, an easing timer and a fade duration.
//
// Per-pixel state (8 slots, the hard limit): the start RGB offsets from
// base, the target RGB offsets from base, the playback time the current
// fade began at, and the fade duration in ms. The eased colour is
// recomputed from these every frame against the shared `time` clock.

// @param deviationMin 0..255 = 5 Smallest random colour offset from base
// @param deviationMax 0..255 = 40 Largest random colour offset from base
// @param fadeSpeedMin 1..15000 = 500 Shortest fade duration in ms
// @param fadeSpeedMax 1..15000 = 2500 Longest fade duration in ms

// @state.pixel startR: 0
// @state.pixel startG: 0
// @state.pixel startB: 0
// @state.pixel targR: 0
// @state.pixel targG: 0
// @state.pixel targB: 0
// @state.pixel beganAt: 0
// @state.pixel duration: 0

function shade(x, y, idx, frame, base, params) {
  // `time` is wall-clock ms since /api/play, shared by every pixel in the
  // frame. `beganAt` stores the `time` value at which this pixel's current
  // fade started; elapsed = time - beganAt. This replaces the C++ per-frame
  // delta accumulation and is fps-independent.
  let dur = duration.pixel;
  let elapsed = time - beganAt.pixel;

  // duration == 0 marks an uninitialised pixel; otherwise a finished fade
  // (elapsed past its duration) rolls a fresh target — mirrors C++
  // pickNewTarget() being called once progress reaches 1.
  let needNew = 0;
  if (dur == 0) {
    needNew = 1;
  } else if (elapsed >= dur) {
    needNew = 1;
  }

  if (needNew == 1) {
    // pickNewTarget(): the old target becomes the new start.
    startR.pixel = targR.pixel;
    startG.pixel = targG.pixel;
    startB.pixel = targB.pixel;

    // A single `deviation` pick in [deviationMin, deviationMax]; each
    // channel then gets an independent offset in [-deviation, +deviation].
    let span = params.deviationMax - params.deviationMin;
    let deviation = params.deviationMin + random() * span;
    targR.pixel = (random() * 2 - 1) * deviation;
    targG.pixel = (random() * 2 - 1) * deviation;
    targB.pixel = (random() * 2 - 1) * deviation;

    // durationMs = random(fadeSpeedMin, fadeSpeedMax), floored at 1.
    let dspan = params.fadeSpeedMax - params.fadeSpeedMin;
    dur = params.fadeSpeedMin + random() * dspan;
    if (dur < 1) {
      dur = 1;
    }
    duration.pixel = dur;
    beganAt.pixel = time;
    elapsed = 0;
  }

  // progress = clamp(elapsed / duration, 0, 1).
  let progress = clamp01(elapsed / dur);

  // Eased colour: base + start + (target - start) * progress, per channel.
  let r = base.r + startR.pixel + (targR.pixel - startR.pixel) * progress;
  let g = base.g + startG.pixel + (targG.pixel - startG.pixel) * progress;
  let b = base.b + startB.pixel + (targB.pixel - startB.pixel) * progress;

  emit(r, g, b);
}
