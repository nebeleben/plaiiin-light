// Random fade-in / hold / fade-out per LED. Each LED runs an independent
// animation; new LEDs are seeded over time until ~density of the panel is
// active. Uses baseColor as the tint — brightness modulates only.
//
// State (declared as globals so it persists across render() calls within
// a single playback session — see `js_player.c`).

// @param density 0..1 = 0.75 Fraction of LEDs animating concurrently
// @param fadeIn 50..3000 = 400 Fade-in duration in ms
// @param hold 0..3000 = 200 Time at peak brightness in ms
// @param fadeOut 50..5000 = 800 Fade-out duration in ms

let states = null;     // [{ phase, t, in, hold, out }] or null while idle
let lastFrame = -1;
let fpsHint = 10;      // assume 10 fps; refined when frame deltas show otherwise

function ensureStates(total) {
  if (states && states.length === total) return;
  states = [];
  for (let i = 0; i < total; i++) {
    states.push({ phase: 0, t: 0, dur: { in: 400, hold: 200, out: 800 } });
  }
}

function render(frame, w, h, baseColor, params) {
  let total = w * h;
  ensureStates(total);

  // dt in ms: assume the player ticks at fpsHint; 100ms/frame at 10 fps.
  // The exact value isn't critical — phases are time-based, not frame-based,
  // so a fps mismatch just stretches/compresses the animation.
  let dt = 1000 / fpsHint;
  if (lastFrame >= 0 && frame > lastFrame) dt = 1000 / fpsHint;
  lastFrame = frame;

  // Count currently-animating LEDs and seed new ones if under target density.
  let active = 0;
  for (let i = 0; i < total; i++) if (states[i].phase !== 0) active++;
  let target = Math.floor(total * params.density);
  // Seed at most a few per frame so the panel ramps up smoothly instead of
  // exploding into noise on the first tick.
  let toSeed = target - active;
  if (toSeed > 4) toSeed = 4;
  while (toSeed > 0) {
    let i = Math.floor(Math.random() * total);
    if (states[i].phase === 0) {
      states[i].phase = 1; // fade-in
      states[i].t = 0;
      states[i].dur.in = params.fadeIn;
      states[i].dur.hold = params.hold;
      states[i].dur.out = params.fadeOut;
      toSeed--;
    } else {
      // Tried a busy slot — bail to avoid scanning the whole grid this tick.
      toSeed--;
    }
  }

  // Advance each LED's animation and produce its current brightness.
  let pixels = [];
  let r = baseColor[0];
  let g = baseColor[1];
  let b = baseColor[2];
  for (let i = 0; i < total; i++) {
    let s = states[i];
    let bright = 0;
    if (s.phase === 1) {              // fade in
      s.t = s.t + dt;
      bright = s.t / s.dur.in;
      if (bright >= 1) { bright = 1; s.phase = 2; s.t = 0; }
    } else if (s.phase === 2) {       // hold
      s.t = s.t + dt;
      bright = 1;
      if (s.t >= s.dur.hold) { s.phase = 3; s.t = 0; }
    } else if (s.phase === 3) {       // fade out
      s.t = s.t + dt;
      bright = 1 - s.t / s.dur.out;
      if (bright <= 0) { bright = 0; s.phase = 0; s.t = 0; }
    }
    pixels.push([
      Math.floor(r * bright),
      Math.floor(g * bright),
      Math.floor(b * bright)
    ]);
  }
  return pixels;
}
