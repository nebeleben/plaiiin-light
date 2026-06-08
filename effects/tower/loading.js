// Loading — a ring of evenly spaced dots chasing each other around the loop,
// the classic "loading spinner". Each dot has a soft pow() falloff so it
// reads as a glowing blob with a short trail rather than a hard pixel.
//
// On the lamp (tower): `dotCount` bright horizontal bands, equally spaced,
// chase together UP the full height of the tower at a steady rate, wrapping
// from the top back to the bottom — a vertical ring. A dim floor keeps the
// unlit pixels gently glowing so the whole tower stays present. Every column
// shows the same dots at the same heights.
//
// Position up the tower is y/h; the rotation is driven by `time`, so the
// effect is stateless and fps-independent.
//
// `rotSpeed` is revolutions per second. `dotCount` is how many dots circle
// the loop. `dotWidth` is each dot's half-extent as a fraction of the loop.
// `trail` shapes the falloff (higher = tighter, punchier dots). `floor` is
// the minimum brightness of the dark pixels.

// @param rotSpeed 0.05..3 = 0.83 Revolutions per second
// @param dotCount 1..8 = 5 Number of dots circling the loop
// @param dotWidth 0.02..0.3 = 0.12 Dot half-width (fraction of the loop)
// @param trail 1..5 = 2.5 Falloff exponent (higher = tighter dots)
// @param floor 0..0.5 = 0.15 Minimum brightness of the dark pixels
// @param axis 0..1 = 0 Travel: 0 = up/down the tower, 1 = around (left/right)

function shade(x, y, idx, frame, base, params) {
  let t = time * 0.001;

  // Rotation of the whole dot ring, wrapped into 0..1.
  let rotation = t * params.rotSpeed;
  rotation = rotation - floor(rotation);

  // This pixel's position along the travel axis. axis < 0.5 runs up the tower
  // (y/h); axis >= 0.5 runs around it (x/w).
  let position = y / h;
  if (params.axis >= 0.5) { position = x / w; }

  let count = floor(params.dotCount);
  if (count < 1) { count = 1; }

  let bright = params.floor;

  for (let d = 0; d < count; d++) {
    // Dot's position around the loop, wrapped into 0..1.
    let dotPos = rotation + d / count;
    dotPos = dotPos - floor(dotPos);

    // Shortest wrapped distance from this pixel to the dot.
    let dist = abs(position - dotPos);
    if (dist > 0.5) { dist = 1 - dist; }

    let n = dist / params.dotWidth;
    if (n < 1) {
      let intensity = pow(1 - n, params.trail);
      let dotBright = params.floor + (1 - params.floor) * intensity;
      if (dotBright > bright) { bright = dotBright; }
    }
  }

  emitBright(clamp01(bright));
}
