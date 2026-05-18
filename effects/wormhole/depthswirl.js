// @mode strip
//
// Depth Swirl — a single bright marble spirals through the wormhole. It eases
// away from the mouth, accelerates as it nears the centre, blasts through the
// throat at ludicrous speed for a brief moment — the wormhole's centre pass —
// then decelerates again as it climbs out the far side. The angular position
// corkscrews continuously, so the marble whips fastest right at the centre.
//
// On the lamp: one lit dot orbits ring 0, sinks ring-to-ring picking up speed,
// streaks through the middle rings almost too fast to follow, then settles as
// it reaches the end — and restarts. A soft halo of light travels with the
// marble so the rings around it glow faintly; rings far from it stay dark, so
// no ring is ever permanently lit. Everything is drawn in the lamp's base
// colour.
//
// Strip mode: x = position-on-ring (0..23), y = ring index. The orbit angle
// is derived from x/24 so the dot wraps cleanly across the x 0/23 seam.
//
// `speed` is the marble's average descent rate; `swirl` is orbits-per-ring;
// `spin` accelerates the orbit with depth; `glow` is the brightness of the
// halo that travels with the marble.

// @param speed 0.1..3 = 0.6 Average rings descended per second
// @param swirl 0.25..6 = 2 Orbits the marble makes per ring of descent
// @param spin 0..2 = 0.6 Extra orbit speed gained per ring of depth
// @param glow 0..0.4 = 0.08 Brightness of the halo travelling with the marble

function shade(x, y, idx, frame, base, params) {
  let t = time * 0.001;

  // The marble covers `span` rings per cycle (span > h so it fully exits).
  let span = h + 1;
  let cycleT = span / params.speed;
  let p = (t % cycleT) / cycleT;          // cycle phase 0..1

  // Non-linear depth: slow at the mouth, ludicrous through the centre, slow
  // again at the end. Passing the phase through sin twice concentrates the
  // travel into a brief burst around p = 0.5; a small linear term keeps the
  // marble from ever fully freezing at the ends.
  let q = 2 * p - 1;
  q = sinLUT(q * 1.5708);
  q = sinLUT(q * 1.5708);
  let g = 0.5 * (1 + q);
  let depth = span * (0.12 * p + 0.88 * g);

  // This pixel's angle around its ring, 0..2π, from x/24 (seam-safe).
  let ang = x / w * 6.28318;

  // Marble's orbit angle: base swirl plus a depth-dependent spin-up — the
  // corkscrew naturally whips around fastest during the centre pass.
  let orbit = depth * params.swirl * 6.28318
            + depth * depth * params.spin * 3.14159;

  // Angular distance from this pixel to the marble, wrapped to [-π, π].
  let da = ang - orbit;
  da = da - 6.28318 * floor(da / 6.28318 + 0.5);

  // Depth distance from this ring to the marble's current ring.
  let dy = y - depth;

  let bright = 0;
  // The marble: a tight blob localised in both angle and depth.
  if (dy > -1.4 && dy < 1.4) {
    let aFall = 1 - abs(da) / 1.1;
    if (aFall > 0) {
      let dFall = 1 - abs(dy) / 1.4;
      bright = aFall * aFall * dFall;
    }
  }

  // Halo that travels WITH the marble — rings near it glow faintly, rings far
  // from it stay fully dark, so no ring is ever permanently lit.
  let gd = abs(dy);
  if (gd < 2.5) {
    let halo = params.glow * (1 - gd / 2.5);
    if (halo > bright) { bright = halo; }
  }

  // Drawn in the lamp's base colour, scaled by brightness.
  emitBright(bright);
}
