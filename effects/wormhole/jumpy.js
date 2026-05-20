// @mode strip
//
// Jumpy — a single travelling head races around one ring of the wormhole,
// accumulating distance as it goes. When it has covered a random
// `nextJumpDistance` it JUMPS to another ring — chosen at random from all the
// rings — leaving a decaying tail behind on the ring it vacated. Its speed
// drifts: it keeps easing toward a fresh random target speed, so the head
// visibly surges and slows.
//
// On the lamp: one bright bead orbits a ring, dragging a fading comet tail; at
// irregular intervals it teleports to a different ring and the abandoned tail
// shrinks away. The result is a restless point of light hopping all over the
// construct. Drawn in the lamp's base colour.
//
// Strip mode hands the script one flat strip (idx 0..N-1). A wormhole ring is
// always 24 LEDs, so this effect derives the ring count, the current ring and
// the position-within-ring from idx itself. If it is accidentally played in
// mirror mode (w == 24) `rings` collapses to 1 and the head simply never jumps
// — it just keeps orbiting the single ring.
//
// Ported from WormholeJumpyPatternV1.cpp. The C++ `computeTravel` sub-step
// integration is simplified to one Euler step per frame; one decaying tail is
// kept for the most-recently-vacated ring.
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
// --- decaying tail on the most-recently-vacated ring --------------------
// @state prevRing: 0
// @state prevHead: 0
// @state prevLen: 0
// @state prevHas: 0

function shade(x, y, idx, frame, base, params) {
  let ringW = 24;                       // a wormhole ring is always 24 LEDs

  // Wormhole geometry derived from idx. In mirror mode w == 24 so rings == 1.
  let rings = floor(w / 24);
  if (rings < 1) { rings = 1; }
  let ring = floor(idx / 24);
  let xr = idx - ring * 24;             // 0..23 within this ring

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
      prevRing = 0; prevHead = 0; prevLen = 0; prevHas = 0;
    }

    // Delta time in seconds since last frame (guard backwards / huge).
    let dt = (time - lastTime) * 0.001;
    lastTime = time;
    if (dt < 0) { dt = 0; }
    if (dt > 0.25) { dt = 0.25; }

    // Speed easing — Euler approximation of the C++ scheduleNextSpeedChange.
    speedRemain = speedRemain - dt;
    if (speedRemain <= 0) {
      targetSpeed = params.speedMin + random() * (params.speedMax - params.speedMin);
      speedRemain = 6 + random() * 18;          // 6..24 s ease window
    }
    let ease = dt / speedRemain;
    if (ease > 1) { ease = 1; }
    curSpeed = curSpeed + (targetSpeed - curSpeed) * ease;
    if (curSpeed < 0) { curSpeed = 0; }

    // Travel this frame (single Euler step).
    let travel = curSpeed * dt;
    distSinceJump = distSinceJump + travel;
    headPos = headPos + travel;
    headPos = headPos - floor(headPos / ringW) * ringW;

    // Decay the vacated ring's tail by the distance travelled this frame.
    if (prevHas > 0) {
      prevLen = prevLen - travel;
      if (prevLen <= 0) { prevLen = 0; prevHas = 0; }
    }

    // Jump check. Only one jump per frame is resolved (per-frame dt is tiny
    // relative to jump distances, so multi-jump is not needed).
    if (distSinceJump >= nextJump) {
      if (rings > 1) {
        // Freeze a decaying tail on the ring being vacated.
        prevRing = activeRing;
        prevHead = headPos;
        prevLen = params.tail;
        prevHas = 1;
        // Pick a different ring at random — uniform among the rings-1 others.
        let nr = activeRing + 1 + floor(random() * (rings - 1));
        nr = nr - floor(nr / rings) * rings;
        activeRing = nr;
      }
      distSinceJump = 0;
      nextJump = params.jumpMin + random() * (params.jumpMax - params.jumpMin);
      if (nextJump < 1) { nextJump = 1; }
    }
  }

  // ---------- Per-pixel render ------------------------------------------
  let bright = 0;

  // Active ring: bright head core plus a live decaying tail.
  if (ring == activeRing) {
    // Signed gap from head BACK to this pixel, wrapped into [0, ringW).
    let gap = headPos - xr;
    gap = gap - floor(gap / ringW) * ringW;

    // Head core — a cosine bump ~1.5 px wide either side of the head.
    if (gap < 1.5) {
      bright = cosLUT(gap / 1.5 * 1.5708);
    }
    let backGap = ringW - gap;
    if (backGap < 1.5) {
      let core2 = cosLUT(backGap / 1.5 * 1.5708);
      if (core2 > bright) { bright = core2; }
    }

    // Live tail trailing behind the head.
    if (gap >= 1 && gap <= params.tail) {
      let lin = 1 - gap / params.tail;
      let dec = pow(params.decay, gap);
      let tb = lin * dec;
      if (tb > bright) { bright = tb; }
    }
  }

  // Vacated ring: render its frozen, shrinking tail.
  if (ring == prevRing && prevHas > 0) {
    let g0 = prevHead - xr;
    g0 = g0 - floor(g0 / ringW) * ringW;
    if (g0 <= prevLen && prevLen > 0) {
      let lin0 = 1 - g0 / params.tail;
      if (lin0 < 0) { lin0 = 0; }
      let tb0 = lin0 * pow(params.decay, g0);
      if (tb0 > bright) { bright = tb0; }
    }
  }

  if (bright > 1) { bright = 1; }
  if (bright < 0) { bright = 0; }

  // The C++ scales baseColor by brightness — emitBright does exactly that.
  emitBright(bright);
}
