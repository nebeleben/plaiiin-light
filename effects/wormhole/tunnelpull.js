// @mode strip
//
// Tunnel Pull — light dragged down the depth axis of the wormhole. A bright
// ring-front travels ring-to-ring along `y` (depth) and *accelerates* as it
// goes, so each ring lights a little sooner than the last — the construct
// reads as matter being sucked into the wormhole throat.
//
// On the lamp every physical ring (a 24-LED circle) flashes whole, in order,
// faster and faster, then the pull restarts from the mouth. A soft trailing
// glow lingers on the rings the front has already passed, so you see a comet
// of rings rather than a single hard flash. Drawn in the lamp's base colour.
//
// Strip mode: x = position-on-ring (0..23, wraps at the seam), y = ring index
// (0 = wormhole mouth, h-1 = throat). The whole ring shares one brightness,
// so the seam at x 0/23 is automatically continuous.
//
// `speed` sets the base fall rate; `accel` is how much faster the front gets
// per ring of depth; `trail` is how long the glow lingers behind the front.

// @param speed 0.1..3 = 0.7 Base rings-per-second the front falls
// @param accel 0..2 = 0.8 Extra fall speed gained per ring of depth
// @param trail 0.1..4 = 1.4 Length of the lingering glow behind the front (rings)

function shade(x, y, idx, frame, base, params) {
  // Wall-clock seconds since playback start drives the pull.
  let t = time * 0.001;

  // Cycle: the front falls from ring 0 toward ring h, then restarts. The
  // depth covered grows quadratically because the front accelerates, so we
  // solve depth(t) = speed*t + accel*t*t for the front's current ring.
  // A full cycle ends when the front has passed the last ring.
  let span = h + params.trail;
  // Time for the (accelerating) front to cover `span` rings.
  let cycleT = (-params.speed + sqrt(params.speed * params.speed
              + 4 * (params.accel + 0.0001) * span))
              / (2 * (params.accel + 0.0001));
  let local = t % cycleT;

  // Front position in ring units at this instant.
  let front = params.speed * local + params.accel * local * local;

  // Depth of THIS ring behind the front (positive = front has passed it).
  let behind = front - y;

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
