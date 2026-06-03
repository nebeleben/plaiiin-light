// @mode mirror
// @modeSwitch
//
// Twin Arc — two glowing arcs sit on opposite sides of one ring and counter-
// rotate, sweeping past each other twice per lap. Where the two arcs overlap
// they add to a bright flare. In mirror mode the firmware tiles this single
// 24-LED ring onto every physical ring, so the whole wormhole pulses with the
// same counter-rotating pair.
//
// On the lamp: two soft crescents of light orbit the circle in opposite
// directions; twice a lap they cross and briefly bloom into a single bright
// flash, then separate again — a slow, breathing scissor of light, drawn in
// the lamp's base colour.
//
// Mirror mode: the script only sees a 24×1 grid — x = 0..23, y always 0. Both
// arc centres and every pixel angle come from x/24 via sin/cos, so the arcs
// wrap seamlessly across the x 0/23 seam.
//
// `speed` is revolutions per second; `width` is each arc's angular half-width.

// @param speed 0.03..1.5 = 0.25 Revolutions per second (each arc)
// @param width 0.1..1.4 = 0.6 Angular half-width of each arc (radians)

function shade(x, y, idx, frame, base, params) {
  let t = time * 0.001;

  // This pixel's angle around the ring, 0..2π (seam-safe via x/w).
  let ang = x / w * 6.28318;

  // The two arc centres counter-rotate; arc B starts half a lap away.
  let phase = t * params.speed * 6.28318;
  let centA = phase;
  let centB = 3.14159 - phase;

  // Angular distance from this pixel to each arc centre, wrapped to [-π, π].
  let dA = ang - centA;
  dA = dA - 6.28318 * floor(dA / 6.28318 + 0.5);
  let dB = ang - centB;
  dB = dB - 6.28318 * floor(dB / 6.28318 + 0.5);

  // Each arc is a smooth falloff out to `width`; cosine-shaped for soft edges.
  let bA = 0;
  if (abs(dA) < params.width) {
    bA = (cosLUT(dA / params.width * 3.14159) + 1) * 0.5;
  }
  let bB = 0;
  if (abs(dB) < params.width) {
    bB = (cosLUT(dB / params.width * 3.14159) + 1) * 0.5;
  }

  // Additive overlap → a flare where the arcs cross, clamped to 1.
  let bright = bA + bB;
  if (bright > 1) { bright = 1; }

  // Drawn in the lamp's base colour, scaled by brightness.
  emitBright(bright);
}
