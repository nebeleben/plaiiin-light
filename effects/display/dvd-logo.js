// DVD logo — small filled rectangle bounces around the panel, colour rotates
// on every wall hit. Closed-form position: per-axis triangle wave from
// fmod(t * vx, 2 * travel) folded onto [0, travel]. Stateless — the bounce
// count is just floor(t / traverseTime), so the colour cycle is deterministic
// without needing per-frame state.
//
// byForm display effect.

// @param speed 1..12 = 4 Logo speed (pixels per second on the faster axis)
// @param logoW 2..6 = 4 Logo width in pixels
// @param logoH 2..6 = 3 Logo height in pixels
// @param ratio 0.4..1 = 0.7 Vertical/horizontal speed ratio (1 = 45°, lower = flatter trajectory)
// @param brightness 0.2..1 = 1 Logo brightness scale

function shade(x, y, idx, frame, base, params) {
  let t = time * 0.001;
  let lw = floor(params.logoW);
  let lh = floor(params.logoH);

  /* Travel distance on each axis = panel - logo (the top-left corner of the
   * logo moves over [0, travel]). Guard against logo larger than panel. */
  let travelX = w - lw; if (travelX < 1) { travelX = 1; }
  let travelY = h - lh; if (travelY < 1) { travelY = 1; }

  let vx = params.speed;
  let vy = params.speed * params.ratio;

  /* Triangle-wave position on each axis. fmod via t - period*floor(t/period).
   * px ∈ [0, 2*travelX); fold the upper half back. */
  let periodX = 2 * travelX / vx;
  let periodY = 2 * travelY / vy;
  let px = t - periodX * floor(t / periodX);
  let py = t - periodY * floor(t / periodY);
  px = px * vx;  // distance into the period (0..2*travelX)
  py = py * vy;
  if (px > travelX) { px = 2 * travelX - px; }
  if (py > travelY) { py = 2 * travelY - py; }
  /* Snap to integer cell, otherwise sub-pixel positions like py=0.3 make
   * `y >= py` exclude row 0 (0 < 0.3) and the logo never visits the top/left
   * edge — appears to bounce from row/col 1. */
  px = floor(px);
  py = floor(py);

  /* Inside the logo rectangle? */
  if (x >= px && x < px + lw && y >= py && y < py + lh) {
    /* Bounce count = total wall hits across both axes. Each axis bounces
     * twice per period (top+bottom or left+right). */
    let bounces = floor(2 * t / periodX) + floor(2 * t / periodY);
    /* 6 colour hue stops (R, Y, G, C, B, M) cycled by bounce count. Pure
     * primaries — much more DVD-screensaver than smooth hue rotation. */
    let stop = bounces - 6 * floor(bounces / 6);
    let r = 0;
    let g = 0;
    let bC = 0;
    if (stop == 0) { r = 255; }
    else if (stop == 1) { r = 255; g = 255; }
    else if (stop == 2) { g = 255; }
    else if (stop == 3) { g = 255; bC = 255; }
    else if (stop == 4) { bC = 255; }
    else                { r = 255; bC = 255; }
    emit(r * params.brightness, g * params.brightness, bC * params.brightness);
  } else {
    emit(0, 0, 0);
  }
}
