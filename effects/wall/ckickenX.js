// @param speed 0.05..2.0 = 0.6 Dance speed
// @param flap 0.1..2.0 = 1.0 Wing flap intensity
// @param cadence 0.5..6.0 = 3.0 Hop cadence
function shade(x, y, idx, frame, base, params) {
  let t = frame * params.speed * 0.05;

  // Side-to-side dance + hop. The bird is 12px tall (comb at cy-7 .. feet at
  // cy+5), so on a 16-row panel cy can only travel 7..10 without clipping.
  // We overshoot the floor (amplitude 3.8 from a 10.8 base) and clamp cy to 10
  // so the feet plant solidly on the bottom row (15) for a brief dwell at each
  // landing instead of only grazing it for a single frame.
  let sway = sinLUT(t * 2.0) * 2.5;
  let bounce = abs(sinLUT(t * params.cadence)) * 3.8;

  // Chicken center
  let cx = 8 + sway;
  let cy = 10.8 - bounce;
  cy = min(cy, 10.0);   // can't sink below the floor — feet stay on row 15

  let dx = x - cx;
  let dy = y - cy;

  // Body (egg-shaped blob)
  let body = (dx * dx) / 9.0 + (dy * dy) / 12.0;

  // Head sits up and forward
  let hx = x - (cx + sway * 0.4);
  let hy = y - (cy - 5.0);
  let head = (hx * hx) / 3.0 + (hy * hy) / 3.0;

  // Wings flap out from the sides
  let wig = (sinLUT(t * 6.0) * 0.5 + 0.5) * params.flap * 2.0;
  let wlx = x - (cx - 3.0 - wig);
  let wrx = x - (cx + 3.0 + wig);
  let wy = y - (cy + 0.5);
  let wingL = (wlx * wlx) / 2.0 + (wy * wy) / 4.0;
  let wingR = (wrx * wrx) / 2.0 + (wy * wy) / 4.0;

  // Beak (orange triangle in front of head)
  let bx = x - (cx + sway * 0.4 + 2.0);
  let by = y - (cy - 5.0);
  let beak = abs(bx) + abs(by);

  // Legs
  let leg = 0;
  if (y >= cy + 3.0 && y <= cy + 5.0) {
    if (abs(x - (cx - 1.0)) < 0.6 || abs(x - (cx + 1.0)) < 0.6) {
      leg = 1;
    }
  }

  // Comb (red bump on top of head)
  let combx = x - (cx + sway * 0.4);
  let comby = y - (cy - 7.0);
  let comb = (combx * combx) / 2.0 + (comby * comby) / 1.5;

  let r = 0;
  let g = 0;
  let b = 0;

  if (head < 1.0 || body < 1.0) {
    // White feathers tinted with base
    r = 230 + base.r * 0.1;
    g = 230 + base.g * 0.1;
    b = 230 + base.b * 0.1;
  }
  if (wingL < 1.0 || wingR < 1.0) {
    r = 200;
    g = 200;
    b = 210;
  }
  if (comb < 1.0) {
    r = 220; g = 30; b = 40;
  }
  if (beak < 1.2 && bx > -0.2) {
    r = 250; g = 160; b = 20;
  }
  if (leg == 1) {
    r = 250; g = 150; b = 20;
  }

  // Eye
  if (abs(x - (cx + sway * 0.4 + 0.6)) < 0.6 && abs(y - (cy - 5.5)) < 0.6) {
    r = 10; g = 10; b = 10;
  }

  emit(clamp01(r / 255) * 255, clamp01(g / 255) * 255, clamp01(b / 255) * 255);
}
