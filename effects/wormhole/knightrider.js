// @mode mirror
// @modeSwitch
//
// KnightRider — the K.I.T.T. scanner: a bright head with a cosine-shaped
// core and a pow() tail, sweeping across the loop. In bounce mode the head
// runs to one end, reverses, and runs back; in cycle mode it loops cleanly
// around the ring in one direction. A leading-edge boost makes the front of
// the head flare slightly brighter than the rest, selling the direction of
// travel.
//
// On the lamp: a glowing scanner streaks across the wormhole ring, dragging
// a fading comet tail behind it. Every row shows the same scanner position,
// so the whole construct pulses as one bar of light.
//
// The head position is derived purely from `time`: a triangle wave over the
// ring in bounce mode, a modulo ramp in cycle mode — so the effect is
// stateless and fps-independent. The original's brief edge-pause on bounce
// is dropped; the head simply turns around.
//
// `secsPerRound` is seconds for one full sweep (there-and-back in bounce
// mode, once around in cycle mode). `tailLen` is the tail length in pixels;
// `tailFade` its per-pixel decay. `coreWidth` is the bright core width.
// `trail` scales tail brightness. `boost` flares the leading edge. `mode`
// picks bounce (< 0.5) or cycle (>= 0.5).

// @param secsPerRound 0.3..8 = 2.2 Seconds for one full sweep
// @param tailLen 1..16 = 7 Tail length in pixels
// @param tailFade 0.05..0.95 = 0.55 Tail per-pixel decay (higher = longer tail)
// @param coreWidth 0.5..6 = 1.5 Width of the bright core in pixels
// @param trail 0..1 = 0.65 Tail brightness scale
// @param boost 1..1.5 = 1.25 Leading-edge brightness boost
// @param mode 0..1 = 0 Movement: < 0.5 bounce, >= 0.5 cycle

function shade(x, y, idx, frame, base, params) {
  let t = time * 0.001;
  let dimension = w;
  let cycle = 0;
  if (params.mode >= 0.5) { cycle = 1; }

  // ---- Head position + travel direction, derived from time ----
  let position = 0;
  let direction = 1;
  // Phase 0..1 of one full sweep, derived purely from time.
  let ph = t / params.secsPerRound;
  ph = ph - floor(ph);

  if (cycle == 0) {
    // Bounce: triangle wave over the span [0, dimension-1].
    let span = dimension - 1;
    if (span < 1) { span = 1; }
    let tri = ph * 2;             // 0..2 across one there-and-back sweep
    if (tri <= 1) {
      position = tri * span;      // forward leg
      direction = 1;
    } else {
      position = (2 - tri) * span; // return leg
      direction = -1;
    }
  } else {
    // Cycle: modulo ramp once around the ring.
    position = ph * dimension;
    direction = 1;
  }

  let halfCore = params.coreWidth * 0.5;

  // ---- Distance from this pixel to the head ----
  let dist = abs(x - position);
  if (cycle == 1) {
    let wrapDist = dimension - dist;
    if (wrapDist < 0) { wrapDist = 0; }
    if (wrapDist < dist) { dist = wrapDist; }
  }

  // ---- Brightness: cosine core, else pow() tail ----
  let bright = 0;
  if (halfCore > 0 && dist <= halfCore) {
    let ratio = clamp01(dist / halfCore);
    // cos(ratio * PI/2) — quarter-turn cosine.
    bright = cosLUT(ratio * 1.5707963);
  } else {
    let tailDist = dist - halfCore;
    if (tailDist < 0) { tailDist = 0; }
    if (tailDist <= params.tailLen) {
      let fade = pow(params.tailFade, tailDist);
      bright = fade * params.trail;
    }
  }

  // ---- Leading-edge boost: pixels just ahead of the head flare up ----
  if (params.boost > 1 && halfCore > 0) {
    let maxAhead = halfCore + 0.5;
    let ahead = 0;
    if (direction > 0) {
      ahead = x - position;
    } else {
      ahead = position - x;
    }
    if (cycle == 1 && ahead < 0) { ahead = ahead + dimension; }
    if (ahead >= 0 && ahead <= maxAhead) {
      bright = bright * params.boost;
    }
  }

  emitBright(clamp01(bright));
}
