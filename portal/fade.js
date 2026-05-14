// Random fade-in / hold / fade-out per LED. Phase 22 — rewritten stateless:
// each LED's animation phase is a deterministic pseudo-random offset inside
// a (fadeIn + hold + fadeOut + dark gap) cycle, so the whole panel looks
// like a bunch of independent breathing LEDs but no per-LED state array is
// allocated. The earlier object-array version hit the FreeRTOS task watchdog
// on 16×16 panels (256 object allocations + 1280 array pushes on the first
// frame is several seconds of mJS work — past the 5–15 s watchdog).
//
// Density is encoded as the fraction of the cycle that's "active" — at
// density=1.0 every LED is always doing one of the three phases; at 0.5
// each LED spends half the cycle in a dark gap.

// @param density 0..1 = 0.75 Fraction of LEDs animating concurrently
// @param fadeIn 50..15000 = 400 Fade-in duration in ms
// @param hold 0..15000 = 200 Time at peak brightness in ms
// @param fadeOut 50..20000 = 800 Fade-out duration in ms

let fpsHint = 10;  // dt = 1000/fpsHint per frame; exact value isn't critical

function render(frame, w, h, baseColor, params) {
  let total = w * h;
  let dt = 1000 / fpsHint;
  let now = frame * dt;

  let fi = params.fadeIn;
  let hd = params.hold;
  let fo = params.fadeOut;
  let density = params.density;
  if (density < 0.01) density = 0.01;
  if (density > 1)    density = 1;
  let active = fi + hd + fo;
  let cycle  = active / density;       // dark gap = cycle - active
  let holdEnd = fi + hd;

  let r = baseColor[0];
  let g = baseColor[1];
  let b = baseColor[2];

  let pixels = [];
  for (let i = 0; i < total; i++) {
    // Knuth multiplicative hash mod cycle — gives each LED an apparently
    // random phase offset that's reproducible across frames (so the same LED
    // continues its cycle smoothly between renders).
    let offset = (i * 2654435761) % cycle;
    let t = (now + offset) % cycle;

    let bright = 0;
    if (t < fi)             bright = t / fi;
    else if (t < holdEnd)   bright = 1;
    else if (t < active)    bright = 1 - (t - holdEnd) / fo;
    // else: in the dark gap → bright stays 0

    pixels.push([
      Math.floor(r * bright),
      Math.floor(g * bright),
      Math.floor(b * bright)
    ]);
  }
  return pixels;
}
