// @mode strip
//
// Depth Swirl — a single bright marble spirals down the wormhole, rolling
// around each ring as it sinks from one ring to the next, and speeding up
// the deeper it falls. The angular position advances continuously while the
// depth marches discretely ring-to-ring, so you see one glowing bead corkscrew
// down the construct and the deeper rings get a colder, dimmer cast.
//
// On the lamp: one lit dot orbits ring 0, then drops to ring 1 and keeps
// orbiting (faster), then ring 2, and so on to the throat — then it restarts
// at the mouth. Rings deeper than the marble keep a faint depth-shaded glow
// so the tunnel always has a sense of distance.
//
// Strip mode: x = position-on-ring (0..23), y = ring index. The orbit angle
// is derived from x/24 so the dot wraps cleanly across the x 0/23 seam.
//
// `speed` is how fast the marble sinks; `swirl` is orbits-per-ring (how tight
// the corkscrew is); `spin` accelerates the orbit with depth; `glow` is the
// resting brightness of the depth-shaded tunnel walls.

// @param speed 0.1..3 = 0.6 Rings descended per second
// @param swirl 0.25..6 = 2 Orbits the marble makes per ring of descent
// @param spin 0..2 = 0.6 Extra orbit speed gained per ring of depth
// @param glow 0..0.4 = 0.08 Resting brightness of the tunnel walls

function shade(x, y, idx, frame, base, params) {
  let t = time * 0.001;

  // Marble depth marches mouth -> throat, then wraps. h+1 so it fully exits
  // the last ring before restarting.
  let depth = (t * params.speed) % (h + 1);

  // This pixel's angle around its ring, 0..2π, from x/24 (seam-safe).
  let ang = x / w * 6.28318;

  // Marble's orbit angle: base swirl plus a depth-dependent spin-up. Using
  // `depth` itself in the spin term makes deeper rings whip round faster.
  let orbit = depth * params.swirl * 6.28318
            + depth * depth * params.spin * 3.14159;

  // Angular distance from this pixel to the marble, wrapped to [-π, π].
  let da = ang - orbit;
  da = da - 6.28318 * floor(da / 6.28318 + 0.5);

  // Depth distance from this ring to the marble's current ring.
  let dy = y - depth;

  let bright = 0;
  // The marble: a tight gaussian-ish blob localised in both angle and depth.
  if (dy > -1.4 && dy < 1.4) {
    let aFall = 1 - abs(da) / 1.1;
    if (aFall > 0) {
      let dFall = 1 - abs(dy) / 1.4;
      bright = aFall * aFall * dFall;
    }
  }

  // Depth-shaded tunnel wall: a faint glow on every ring, dimmer + cooler
  // the deeper it sits.
  let depthFrac = y / h;
  let wall = params.glow * (1 - depthFrac * 0.7);
  if (wall > bright) { bright = wall; }

  // Colour: marble is warm-white near the mouth, cooling to blue at depth.
  let cold = depthFrac;
  let r = (255 - cold * 200) * bright;
  let g = (220 - cold * 120) * bright;
  let b = (150 + cold * 105) * bright;
  emit(r, g, b);
}
