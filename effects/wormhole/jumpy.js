// @mode strip
//
// Jumpy — a single travelling head races around one ring of the wormhole,
// accumulating distance as it goes. When it has covered a random
// `nextJumpDistance` it JUMPS to the facing ring, leaving a decaying tail
// behind on the ring it vacated. Its speed drifts: it keeps easing toward a
// fresh random target speed, so the head visibly surges and slows.
//
// On the lamp: one bright bead orbits a ring, dragging a fading comet tail;
// at irregular intervals it teleports to another ring and the abandoned tail
// shrinks away. The result is a restless point of light hopping up and down
// the construct.
//
// Strip mode: x = position-on-ring (0..23), y = ring index. This is an
// inherently two-ring pattern — it picks ring 0 and ring 1 (clamped to the
// available rings) as the pair the head bounces between. With h == 1 it just
// keeps running on the single ring without jumping (degrades gracefully).
//
// Ported from WormholeJumpyPatternV1.cpp. The C++ `computeTravel` sub-step
// integration is simplified to one Euler step per frame (advance the speed
// toward target, travel = speed * dt) — "as faithful as PLBC allows".
//
// `speedMin`/`speedMax` are head travel in ring-positions per second;
// `jumpMin`/`jumpMax` are how far (in positions) the head runs before a jump;
// `tail` is the tail length in pixels; `decay` is the per-pixel tail falloff.

// @param speedMin 1..40 = 8 Slowest head speed, ring-positions per second
// @param speedMax 4..80 = 28 Fastest head speed, ring-positions per second
// @param jumpMin 6..48 = 14 Shortest distance travelled before a jump
// @param jumpMax 12..96 = 40 Longest distance travelled before a jump
// @param tail 1..20 = 9 Tail length behind the head, in pixels
// @param decay 0.1..0.95 = 0.7 Per-pixel tail brightness falloff

// --- simulation clock ---------------------------------------------------
// @state inited: 0
// @state lastTime: 0
// --- head ---------------------------------------------------------------
// @state headPos: 0
// @state activeRing: 0
// @state distSinceJump: 0
// @state nextJump: 20
// @state curSpeed: 8
// @state targetSpeed: 8
// @state speedRemain: 0
// --- per-ring tail state (ring 0) ---------------------------------------
// @state t0Head: 0
// @state t0Len: 0
// @state t0Has: 0
// --- per-ring tail state (ring 1) ---------------------------------------
// @state t1Head: 0
// @state t1Len: 0
// @state t1Has: 0

function shade(x, y, idx, frame, base, params) {
  let ringW = w;                 // positions around one ring (24)

  // ---------- Per-frame simulation (runs once, before pixel 0) ----------
  if (idx == 0) {
    // First frame: seed head + speed + first jump distance.
    if (inited < 1) {
      inited = 1;
      lastTime = time;
      headPos = 0;
      activeRing = 0;
      distSinceJump = 0;
      curSpeed = params.speedMin + random() * (params.speedMax - params.speedMin);
      targetSpeed = curSpeed;
      speedRemain = 0;
      nextJump = params.jumpMin + random() * (params.jumpMax - params.jumpMin);
      if (nextJump < 1) { nextJump = 1; }
      t0Head = 0; t0Len = 0; t0Has = 0;
      t1Head = 0; t1Len = 0; t1Has = 0;
    }

    // Delta time in seconds since last frame (guard against backwards / huge).
    let dt = (time - lastTime) * 0.001;
    lastTime = time;
    if (dt < 0) { dt = 0; }
    if (dt > 0.25) { dt = 0.25; }

    // Speed easing — Euler approximation of the C++ scheduleNextSpeedChange.
    // When the current ease window runs out, pick a fresh target + duration.
    speedRemain = speedRemain - dt;
    if (speedRemain <= 0) {
      targetSpeed = params.speedMin + random() * (params.speedMax - params.speedMin);
      speedRemain = 6 + random() * 18;          // 6..24 s ease window
    }
    // Advance current speed a fraction of the way to the target this frame.
    let ease = dt / speedRemain;
    if (ease > 1) { ease = 1; }
    curSpeed = curSpeed + (targetSpeed - curSpeed) * ease;
    if (curSpeed < 0) { curSpeed = 0; }

    // Travel this frame (single Euler step).
    let travel = curSpeed * dt;
    distSinceJump = distSinceJump + travel;
    headPos = headPos + travel;

    // Wrap the head position into [0, ringW).
    headPos = headPos - floor(headPos / ringW) * ringW;

    // Decay the inactive ring's tail by the distance travelled this frame.
    if (activeRing == 0) {
      if (t1Has > 0) {
        t1Len = t1Len - travel;
        if (t1Len <= 0) { t1Len = 0; t1Has = 0; }
      }
    } else {
      if (t0Has > 0) {
        t0Len = t0Len - travel;
        if (t0Len <= 0) { t0Len = 0; t0Has = 0; }
      }
    }

    // Jump check. Only one jump per frame is resolved (per-frame dt is tiny
    // relative to jump distances, so multi-jump is not needed).
    if (distSinceJump >= nextJump) {
      // Only jump if there is a facing ring to jump to.
      if (h > 1) {
        // Freeze a decaying tail on the ring being vacated, anchored at the
        // current head position.
        if (activeRing == 0) {
          t0Head = headPos;
          t0Len = params.tail;
          t0Has = 1;
        } else {
          t1Head = headPos;
          t1Len = params.tail;
          t1Has = 1;
        }
        // Flip to the other ring; head keeps its ring position.
        if (activeRing == 0) { activeRing = 1; } else { activeRing = 0; }
      }
      // Reset the jump accumulator and pick the next jump distance.
      distSinceJump = 0;
      nextJump = params.jumpMin + random() * (params.jumpMax - params.jumpMin);
      if (nextJump < 1) { nextJump = 1; }
    }
  }

  // ---------- Per-pixel render ------------------------------------------
  // Wormhole geometry maps ring 0 / ring 1 onto rows y == 0 / y == 1. Every
  // other row stays dark.
  let bright = 0;

  // Active ring: bright head core plus a live decaying tail.
  if (y == activeRing) {
    // Signed gap from head BACK to this pixel, wrapped into [0, ringW).
    let gap = headPos - x;
    gap = gap - floor(gap / ringW) * ringW;

    // Head core — a cosine bump ~1.5 px wide either side of the head.
    if (gap < 1.5) {
      bright = cosLUT(gap / 1.5 * 1.5708);     // cos ramp, 1 at gap 0
    }
    let backGap = ringW - gap;                 // distance the "short way" back
    if (backGap < 1.5) {
      let core2 = cosLUT(backGap / 1.5 * 1.5708);
      if (core2 > bright) { bright = core2; }
    }

    // Live tail trailing behind the head: linear falloff over `tail` pixels
    // multiplied by the per-pixel `decay` power (matches the C++ tail maths).
    if (gap >= 1 && gap <= params.tail) {
      let lin = 1 - gap / params.tail;
      let dec = pow(params.decay, gap);
      let tb = lin * dec;
      if (tb > bright) { bright = tb; }
    }
  }

  // Vacated ring 0: render its frozen, shrinking tail.
  if (y == 0 && activeRing != 0 && t0Has > 0) {
    let g0 = t0Head - x;
    g0 = g0 - floor(g0 / ringW) * ringW;
    if (g0 <= t0Len && t0Len > 0) {
      let lin0 = 1 - g0 / params.tail;
      if (lin0 < 0) { lin0 = 0; }
      let tb0 = lin0 * pow(params.decay, g0);
      if (tb0 > bright) { bright = tb0; }
    }
  }

  // Vacated ring 1: render its frozen, shrinking tail.
  if (y == 1 && activeRing != 1 && t1Has > 0) {
    let g1 = t1Head - x;
    g1 = g1 - floor(g1 / ringW) * ringW;
    if (g1 <= t1Len && t1Len > 0) {
      let lin1 = 1 - g1 / params.tail;
      if (lin1 < 0) { lin1 = 0; }
      let tb1 = lin1 * pow(params.decay, g1);
      if (tb1 > bright) { bright = tb1; }
    }
  }

  if (bright > 1) { bright = 1; }
  if (bright < 0) { bright = 0; }

  // The C++ scales baseColor by brightness — emitBright does exactly that.
  emitBright(bright);
}
