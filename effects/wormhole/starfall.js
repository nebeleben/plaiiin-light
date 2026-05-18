// Starfall — multiple stars orbiting the wormhole ring, each racing around
// for a few loops before burning out, then a fresh one spawns in its place
// after a random pause. Each star drags a pow()-shaped tail behind it.
//
// On the lamp: bright points chase around the ring at different speeds and
// directions, some clockwise, some counter, each fading out after its run
// and being replaced — a restless, sparkling orbit. The stars travel along
// x, so on a strip-mode grid every row carries the same swarm in sync.
//
// State model: 4 independent star "slots". Each tracks ring position, signed
// velocity (px/frame at a 30 fps baseline, sign = direction), loops
// remaining, and an active flag. The once-per-frame bookkeeping (advance,
// count down loops, retire, maybe respawn) runs inside `if (idx == 0)`.
// `lastTime` carries the previous frame's `time` so motion scales with real
// elapsed ms and stays fps-independent. Per pixel, the tail brightness of
// every active star is accumulated.
//
// `density` sets the per-frame respawn chance of an idle slot (higher = more
// stars in flight at once). `tailLen` is the tail length in pixels and
// `tailExp` shapes its falloff. `speed` scales how fast the stars orbit.

// @param density 0..1 = 0.5 Respawn likelihood for idle slots (more = busier)
// @param tailLen 1..16 = 8 Tail length in pixels
// @param tailExp 1..5 = 2 Tail falloff exponent (higher = sharper tail)
// @param speed 0.1..3 = 1 Orbit speed scale

// @state s0_x: 0
// @state s0_v: 0
// @state s0_loops: 0
// @state s0_active: 0
// @state s1_x: 0
// @state s1_v: 0
// @state s1_loops: 0
// @state s1_active: 0
// @state s2_x: 0
// @state s2_v: 0
// @state s2_loops: 0
// @state s2_active: 0
// @state s3_x: 0
// @state s3_v: 0
// @state s3_loops: 0
// @state s3_active: 0
// @state lastTime: 0

function shade(x, y, idx, frame, base, params) {

  // ---------- Per-frame slot pool update (runs once before pixel 0) -------
  if (idx == 0) {
    // Elapsed frames since last update, normalised to a 30 fps baseline so
    // the px/frame velocities behave consistently at any real fps.
    let dt = (time - lastTime) * 0.03;
    if (dt < 0) { dt = 0; }
    if (dt > 4) { dt = 4; }
    lastTime = time;

    // Speed range maps the C++ 12..28 px/s to px/frame at 30 fps (~0.4..0.93).
    // `sp` is a reused scratch local for the spawn dice.
    let spLo = 0.4 * params.speed;
    let spHi = 0.93 * params.speed;
    let sp = 0;

    // Slot 0
    if (s0_active > 0) {
      s0_x = s0_x + s0_v * dt;
      if (s0_x >= w) { s0_x = s0_x - w; s0_loops = s0_loops - 1; }
      if (s0_x < 0)  { s0_x = s0_x + w; s0_loops = s0_loops - 1; }
      if (s0_loops <= 0) { s0_active = 0; }
    } else {
      if (random() < params.density * 0.05) {
        s0_x = random() * w;
        sp = spLo + random() * (spHi - spLo);
        if (random() < 0.5) { sp = -sp; }
        s0_v = sp;
        s0_loops = 1 + floor(random() * 3);
        s0_active = 1;
      }
    }

    // Slot 1
    if (s1_active > 0) {
      s1_x = s1_x + s1_v * dt;
      if (s1_x >= w) { s1_x = s1_x - w; s1_loops = s1_loops - 1; }
      if (s1_x < 0)  { s1_x = s1_x + w; s1_loops = s1_loops - 1; }
      if (s1_loops <= 0) { s1_active = 0; }
    } else {
      if (random() < params.density * 0.05) {
        s1_x = random() * w;
        sp = spLo + random() * (spHi - spLo);
        if (random() < 0.5) { sp = -sp; }
        s1_v = sp;
        s1_loops = 1 + floor(random() * 3);
        s1_active = 1;
      }
    }

    // Slot 2
    if (s2_active > 0) {
      s2_x = s2_x + s2_v * dt;
      if (s2_x >= w) { s2_x = s2_x - w; s2_loops = s2_loops - 1; }
      if (s2_x < 0)  { s2_x = s2_x + w; s2_loops = s2_loops - 1; }
      if (s2_loops <= 0) { s2_active = 0; }
    } else {
      if (random() < params.density * 0.05) {
        s2_x = random() * w;
        sp = spLo + random() * (spHi - spLo);
        if (random() < 0.5) { sp = -sp; }
        s2_v = sp;
        s2_loops = 1 + floor(random() * 3);
        s2_active = 1;
      }
    }

    // Slot 3
    if (s3_active > 0) {
      s3_x = s3_x + s3_v * dt;
      if (s3_x >= w) { s3_x = s3_x - w; s3_loops = s3_loops - 1; }
      if (s3_x < 0)  { s3_x = s3_x + w; s3_loops = s3_loops - 1; }
      if (s3_loops <= 0) { s3_active = 0; }
    } else {
      if (random() < params.density * 0.05) {
        s3_x = random() * w;
        sp = spLo + random() * (spHi - spLo);
        if (random() < 0.5) { sp = -sp; }
        s3_v = sp;
        s3_loops = 1 + floor(random() * 3);
        s3_active = 1;
      }
    }
  }

  // ---------- Per-pixel: accumulate every active star's tail -------------
  // `behind` and `nrm` are reused scratch locals across the four slots.
  let bright = 0;
  let behind = 0;
  let nrm = 0;

  if (s0_active > 0) {
    if (s0_v >= 0) { behind = s0_x - x; } else { behind = x - s0_x; }
    behind = behind - floor(behind / w) * w;   // wrap into 0..w
    if (behind <= params.tailLen) {
      nrm = 1 - behind / (params.tailLen + 1);
      if (nrm > 0) { bright = bright + pow(nrm, params.tailExp); }
    }
  }
  if (s1_active > 0) {
    if (s1_v >= 0) { behind = s1_x - x; } else { behind = x - s1_x; }
    behind = behind - floor(behind / w) * w;
    if (behind <= params.tailLen) {
      nrm = 1 - behind / (params.tailLen + 1);
      if (nrm > 0) { bright = bright + pow(nrm, params.tailExp); }
    }
  }
  if (s2_active > 0) {
    if (s2_v >= 0) { behind = s2_x - x; } else { behind = x - s2_x; }
    behind = behind - floor(behind / w) * w;
    if (behind <= params.tailLen) {
      nrm = 1 - behind / (params.tailLen + 1);
      if (nrm > 0) { bright = bright + pow(nrm, params.tailExp); }
    }
  }
  if (s3_active > 0) {
    if (s3_v >= 0) { behind = s3_x - x; } else { behind = x - s3_x; }
    behind = behind - floor(behind / w) * w;
    if (behind <= params.tailLen) {
      nrm = 1 - behind / (params.tailLen + 1);
      if (nrm > 0) { bright = bright + pow(nrm, params.tailExp); }
    }
  }

  emitBright(clamp01(bright));
}
