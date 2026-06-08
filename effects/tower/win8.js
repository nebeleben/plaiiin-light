// Win8 — the Windows 8 boot spinner: a string of dots that race around the
// loop with an easeInOutCubic motion, so they accelerate out of the start,
// coast, then decelerate before looping. The lead dot is brightest and each
// following dot is dimmer, giving the cluster a clear head and tail.
//
// On the lamp (tower): `dotCount` horizontal bands chase UP the full height
// of the tower, bunching up and stretching out as the easing speeds them up
// and slows them down, wrapping from the top back to the bottom — a vertical
// ring. The whole pattern's start point drifts slowly so it never settles
// into one fixed groove. Every column shows the same dots.
//
// Position up the tower is y/h; both the per-dot phase and the drifting
// start offset come from `time`, so the effect is stateless.
//
// `cycleSpeed` is cycles per second of the dot motion. `dotCount` is how
// many dots. `dotRadius` is each dot's half-width (fraction of the loop).
// `trail` shapes the falloff. `tailBright` is how bright the last dot is
// relative to the lead dot. `floor` is the minimum background brightness.
// `drift` is how fast the whole pattern's start point creeps around.

// @param cycleSpeed 0.05..3 = 0.56 Cycles per second of the dot motion
// @param dotCount 1..8 = 5 Number of dots in the chase
// @param dotRadius 0.02..0.3 = 0.09 Dot half-width (fraction of the loop)
// @param trail 1..5 = 2.4 Falloff exponent (higher = tighter dots)
// @param tailBright 0..1 = 0.35 Brightness of the last dot vs the lead dot
// @param floor 0..0.5 = 0.05 Minimum background brightness
// @param drift 0..0.5 = 0.05 Speed the pattern start point creeps around
// @param axis 0..1 = 0 Travel: 0 = up/down the tower, 1 = around (left/right)

function shade(x, y, idx, frame, base, params) {
  let t = time * 0.001;

  // Master phase of the chase and the slowly drifting start offset.
  let phase = t * params.cycleSpeed;
  phase = phase - floor(phase);
  let startOffset = t * params.drift;
  startOffset = startOffset - floor(startOffset);

  // axis < 0.5 runs up the tower (y/h); axis >= 0.5 runs around it (x/w).
  let position = y / h;
  if (params.axis >= 0.5) { position = x / w; }

  let count = floor(params.dotCount);
  if (count < 1) { count = 1; }

  let bright = params.floor;

  for (let d = 0; d < count; d++) {
    // Each dot is offset along the phase, then wrapped into 0..1.
    let dotProgress = phase + d / count;
    dotProgress = dotProgress - floor(dotProgress);

    // easeInOutCubic(dotProgress).
    let eased = 0;
    if (dotProgress < 0.5) {
      eased = 4 * dotProgress * dotProgress * dotProgress;
    } else {
      let f = -2 * dotProgress + 2;
      eased = 1 - (f * f * f) / 2;
    }

    // Dot's position around the loop with the drifting offset applied.
    let dotPos = eased + startOffset;
    dotPos = dotPos - floor(dotPos);

    // Shortest wrapped distance from this pixel to the dot.
    let dist = abs(position - dotPos);
    if (dist > 0.5) { dist = 1 - dist; }

    let n = dist / params.dotRadius;
    if (n < 1) {
      let intensity = pow(1 - n, params.trail);

      // Per-dot strength: lead dot full, last dot at tailBright.
      let dotStrength = 1;
      if (count > 1) {
        let fraction = d / (count - 1);
        dotStrength = params.tailBright + (1 - params.tailBright) * (1 - fraction);
      }

      let dotBright = params.floor + (1 - params.floor) * intensity * dotStrength;
      if (dotBright > bright) { bright = dotBright; }
    }
  }

  emitBright(clamp01(bright));
}
