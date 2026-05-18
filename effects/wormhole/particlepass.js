// @mode strip
//
// Particle Pass — density-driven wormhole shimmer. LED positions around the
// rings spontaneously ignite, swell to a peak, then fade out. Each "particle"
// is one LED: it runs an approach -> peak -> fade envelope with a delayed,
// weaker secondary tail, so a bloom rises sharply then settles in two stages.
//
// On the lamp: random points around the wormhole bloom and die. Outer rings
// (higher y) lag a little behind the mouth, so a wave of blooms reads as a
// pass washing down the construct from one side to the other.
//
// Strip mode: x = position-on-ring (0..23), y = ring index. This is a
// per-pixel-state effect — every LED carries its own envelope, so a particle
// lives in the pixel it ignites on. With h == 1 it still runs (every pixel is
// just ring 0).
//
// Ported from WormholeParticlePassPatternV1.cpp. The approach/peak/fade
// envelope and the upper/secondary intensity formulas follow the C++ exactly.
// PLBC has no neighbour-pixel access, so the C++'s explicit upper/lower-ring
// particle pairing cannot be tracked across rows; instead each pixel renders
// its OWN two-stage envelope and a per-ring time delay (later y ignites with
// a head start consumed, later y fades later) recreates the cross-ring wash.
// See report for exactly what was approximated.
//
// `density` is the average number of live particles; `speedMin`/`speedMax`
// set the approach rate; `sizeMin`/`sizeMax` slow the fade for bigger
// particles; `fade` is an overall fade-rate multiplier.

// @param density 0.02..0.6 = 0.18 Fraction of LEDs alight on average
// @param speedMin 0.1..1 = 0.35 Slowest particle speed (drives approach rate)
// @param speedMax 0.3..2 = 0.85 Fastest particle speed
// @param sizeMin 0.5..6 = 1.5 Smallest particle size (bigger = slower fade)
// @param sizeMax 1..10 = 4 Largest particle size
// @param fade 0.3..3 = 1 Overall fade-rate multiplier

// @state inited: 0
// @state lastTime: 0

// Per-pixel envelope. phase: 0 idle, 1 approaching, 2 fading.
// @state.pixel phase: 0
// @state.pixel approach: 0
// @state.pixel fade: 0
// @state.pixel secFade: 0
// @state.pixel started: 0
// @state.pixel pspeed: 0
// @state.pixel psize: 0

function shade(x, y, idx, frame, base, params) {

  // ---------- Per-frame clock (runs once before pixel 0) ----------------
  if (idx == 0) {
    if (inited < 1) {
      inited = 1;
      lastTime = time;
    }
  }

  // Delta time in seconds, from the per-frame-snapshotted shared clock so
  // every pixel this frame sees the same dt. Advance the clock after the
  // last pixel has been shaded.
  let dt = (time - lastTime) * 0.001;
  if (dt < 0) { dt = 0; }
  if (dt > 0.25) { dt = 0.25; }
  if (idx == w * h - 1) {
    lastTime = time;
  }

  // Ring-depth lag: outer rings advance their envelopes slightly slower so a
  // simultaneous burst reads as a pass washing y0 -> y(h-1). This approximates
  // the C++ upper-then-lower secondary-fade choreography across the rings.
  let depthLag = 1;
  if (h > 1) {
    depthLag = 1 - y / h * 0.45;
  }
  let edt = dt * depthLag;

  let ph = phase.pixel;

  // ---------- Spawn: an idle pixel may ignite --------------------------
  // Per-frame spawn probability is `density` per LED (matches particles.js's
  // random() < spawnRate), so on average density*totalPixels are alight.
  if (ph < 1) {
    if (random() < params.density) {
      ph = 1;
      approach.pixel = 0;
      fade.pixel = 0;
      secFade.pixel = 0;
      started.pixel = 0;
      pspeed.pixel = params.speedMin + random() * (params.speedMax - params.speedMin);
      psize.pixel = params.sizeMin + random() * (params.sizeMax - params.sizeMin);
    }
  }

  // ---------- Envelope advance + intensity -----------------------------
  // val   = primary intensity (the C++ upperIntensity).
  // secVal = the delayed, weaker secondary glow (the C++ lowerIntensity).
  let val = 0;
  let secVal = 0;

  if (ph == 1) {
    // Approach: approachProgress rises at speed*2 (min 0.35), clamped to 1.
    let approachRate = pspeed.pixel * 2;
    if (approachRate < 0.35) { approachRate = 0.35; }
    let ap = approach.pixel + approachRate * edt;

    let intensity = ap;
    if (intensity > 1) { intensity = 1; }
    val = intensity;

    // secondaryIntensity: 0 until intensity hits 0.5, then (i-0.5)*2.
    if (intensity >= 0.5) {
      secVal = (intensity - 0.5) * 2;
      if (secVal > 1) { secVal = 1; }
    }

    if (ap >= 1) {
      // Reached peak — both stages snap to full, switch to fade phase.
      ap = 1;
      ph = 2;
      fade.pixel = 0;
      secFade.pixel = 0;
      started.pixel = 0;
      val = 1;
      secVal = 1;
    }
    approach.pixel = ap;
  }

  if (ph == 2) {
    // Fade: fadeRate = speed*0.5 / size, floored at 0.1, scaled by `fade`.
    let fadeRate = pspeed.pixel * 0.5;
    if (psize.pixel > 0) { fadeRate = fadeRate / psize.pixel; }
    if (fadeRate < 0.1) { fadeRate = 0.1; }
    fadeRate = fadeRate * params.fade;

    let fp = fade.pixel + fadeRate * edt;
    let primary = 1 - fp;
    if (primary < 0) { primary = 0; }
    val = primary;

    // Secondary fade starts once the primary has fallen to <= 0.5; until then
    // the secondary stage holds at the full peak.
    let sf = secFade.pixel;
    let st = started.pixel;
    if (st < 1) {
      if (primary <= 0.5) {
        st = 1;
        sf = 0;
      } else {
        secVal = 1;
      }
    }
    if (st >= 1) {
      sf = sf + fadeRate * edt;
      let secondary = 1 - sf;
      if (secondary < 0) { secondary = 0; }
      secVal = secondary;
    }

    fade.pixel = fp;
    secFade.pixel = sf;
    started.pixel = st;

    // Particle dies once both stages have faded out.
    if (val <= 0.004 && secVal <= 0.004) {
      ph = 0;
    }
  }

  phase.pixel = ph;

  // The visible brightness is the stronger of the two envelope stages — the
  // primary swell plus its lingering secondary tail, both on this LED.
  let bright = val;
  if (secVal > bright) { bright = secVal; }
  if (bright < 0) { bright = 0; }
  if (bright > 1) { bright = 1; }

  // The C++ lerps baseColor toward white by the intensity.
  emit(
    base.r + (255 - base.r) * bright,
    base.g + (255 - base.g) * bright,
    base.b + (255 - base.b) * bright
  );
}
