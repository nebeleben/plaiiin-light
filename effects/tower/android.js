// Android — a single arc that grows and shrinks while it rotates around the
// loop, ported from the classic Android boot/progress spinner. The arc has
// smoothstep-feathered ends so it fades softly in at the tail and out at the
// head rather than snapping on.
//
// On the lamp: a bright crescent sweeps steadily around the wormhole ring
// (or the tower circumference), breathing longer and shorter as it goes.
// Every row sees the same arc at the same angle, so the whole construct
// reads as one rotating band of light.
//
// Position around the loop is x/w, so the arc stays in sync across all rows.
// `rotation` and the grow/shrink `growthPhase` are both derived from `time`,
// so it is fully stateless and fps-independent.
//
// `speed` is revolutions per second (the grow cycle runs at 0.75x of it, as
// in the original). `minArc` / `maxArc` bound the arc length as a fraction
// of the loop.

// @param speed 0.05..3 = 0.75 Revolutions per second
// @param minArc 0.01..0.5 = 0.05 Shortest arc length (fraction of the loop)
// @param maxArc 0.1..1 = 0.75 Longest arc length (fraction of the loop)

function shade(x, y, idx, frame, base, params) {
  let t = time * 0.001;

  // Rotation and growth phase, both wrapped into 0..1. Growth runs at 0.75x.
  let rotation = t * params.speed;
  rotation = rotation - floor(rotation);
  let growthPhase = t * params.speed * 0.75;
  growthPhase = growthPhase - floor(growthPhase);

  // Arc length breathes between minArc and maxArc on a sine.
  let hi = params.maxArc;
  if (hi < params.minArc) { hi = params.minArc; }
  let arcProgress = (sinLUT(6.28318 * growthPhase) + 1) * 0.5;
  let length = params.minArc + (hi - params.minArc) * arcProgress;
  length = clamp01(length);

  // Feather width: 25% of the arc, clamped to [0.02, 45% of the arc].
  let edgeFeather = length * 0.25;
  if (edgeFeather < 0.02) { edgeFeather = 0.02; }
  let maxFeather = length * 0.45;
  if (edgeFeather > maxFeather) { edgeFeather = maxFeather; }
  let featherRatio = 0;
  if (length > 0) { featherRatio = edgeFeather / length; }

  // This pixel's position around the loop, and its offset from the arc start.
  let position = x / w;
  let relative = position - rotation;
  if (relative < 0) { relative = relative + 1; }

  let bright = 0;
  if (length > 0 && relative <= length) {
    let n = relative / length;
    // tail: smoothstep(0, featherRatio, n)
    let tail = 1;
    if (featherRatio > 0) {
      let tt = n / featherRatio;
      tt = clamp01(tt);
      tail = tt * tt * (3 - 2 * tt);
    }
    // head: 1 - smoothstep(1 - featherRatio, 1, n)
    let head = 1;
    if (featherRatio > 0) {
      let ht = (n - (1 - featherRatio)) / featherRatio;
      ht = clamp01(ht);
      head = 1 - ht * ht * (3 - 2 * ht);
    }
    bright = clamp01(tail * head);
  }

  emitBright(bright);
}
