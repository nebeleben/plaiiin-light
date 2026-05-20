// @mode strip
//
// Tunnel Pull — light dragged along the depth axis of the wormhole. A bright
// ring-front travels ring-to-ring and *accelerates* as it goes, so each ring
// lights a little sooner than the last — the construct reads as matter being
// sucked through the wormhole. Each cycle the pull direction is chosen at
// random, so the light is sometimes drawn toward the throat and sometimes
// back out toward the mouth.
//
// On the lamp every physical ring (a 24-LED circle) flashes whole, in order,
// faster and faster, then the pull restarts in a (possibly new) direction. A
// soft trailing glow lingers on the rings the front has already passed.
// Drawn in the lamp's base colour.
//
// Strip mode hands the script one flat strip (idx 0..N-1). A wormhole ring is
// always 24 LEDs, so this effect derives the ring count and the current ring
// from idx itself; the whole ring shares one brightness. If it is accidentally
// played in mirror mode (w == 24) `rings` collapses to 1 and it runs on a
// single ring.
//
// `speed` sets the base fall rate; `accel` is how much faster the front gets
// per ring travelled; `trail` is how long the glow lingers behind the front.

// @param speed 0.1..3 = 0.7 Base rings-per-second the front travels
// @param accel 0..2 = 0.8 Extra speed gained per ring travelled
// @param trail 0.1..4 = 1.4 Length of the lingering glow behind the front (rings)

function shade(x, y, idx, frame, base, params) {
  // Wall-clock seconds since playback start drives the pull.
  let t = time * 0.001;

  // Wormhole geometry from idx — 24 LEDs per ring. In mirror mode w == 24 so
  // rings collapses to 1.
  let rings = floor(w / 24);
  if (rings < 1) { rings = 1; }
  let ring = floor(idx / 24);

  // The accelerating front covers `span` rings per cycle. Solve
  // depth(t) = speed*t + accel*t*t = span for the cycle duration.
  let span = rings + params.trail;
  let cycleT = (-params.speed + sqrt(params.speed * params.speed
              + 4 * (params.accel + 0.0001) * span))
              / (2 * (params.accel + 0.0001));
  let local = t % cycleT;

  // Which cycle this is — and, by a stable hash of the cycle number, which
  // way the pull runs this time (dir 1 = toward the throat, 0 = toward the
  // mouth). The direction is constant within a cycle, re-rolled each restart.
  let cyc = floor(t / cycleT);
  let dir = 0;
  if (hash(cyc) < 0.5) { dir = 1; }

  // Front position in ring units at this instant (0..span).
  let front = params.speed * local + params.accel * local * local;

  // How far THIS ring sits behind the front (positive = front has passed it).
  let behind = 0;
  if (dir > 0) {
    behind = front - ring;               // front runs mouth -> throat
  } else {
    behind = ring - (rings - 1 - front); // front runs throat -> mouth
  }

  let bright = 0;
  if (behind >= 0 && behind < params.trail) {
    // Linear fade across the trail; the head ring is brightest.
    bright = 1 - behind / params.trail;
    // Square it so the head pops and the tail dims off smoothly.
    bright = bright * bright;
  } else if (behind < 0 && behind > -0.6) {
    // A faint leading shimmer just ahead of the front.
    bright = (behind + 0.6) / 0.6 * 0.25;
  }

  // Drawn in the lamp's base colour, scaled by the front brightness.
  emitBright(bright);
}
