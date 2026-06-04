// @param speed 0.05..2.0 = 0.6 Dance speed
// @param flap 0.1..2.0 = 1.0 Wing flap intensity
// @param cadence 0.5..6.0 = 3.0 Hop cadence
// @param grassy 0.0..1.0 = 0.6 Grass height and wildness
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

  // Body (egg-shaped blob) — dx/dy inlined to stay under the 32-local cap
  let body = ((x - cx) * (x - cx)) / 9.0 + ((y - cy) * (y - cy)) / 12.0;

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

  // Comb (red bump on top of head) — combx/comby inlined to save locals
  let comb = ((x - (cx + sway * 0.4)) * (x - (cx + sway * 0.4))) / 2.0 + ((y - (cy - 7.0)) * (y - (cy - 7.0))) / 1.5;

  // Grass: a continuous green floor on the bottom row (15) plus ragged blades
  // that rise a row or two and sway in a light breeze. Drawn as background, so
  // the chicken and its feet render in front of it.
  // grassy scales both the blade height variation and the breeze: at 0 it
  // collapses to a flat, stiff single-row floor; toward 1 the blades grow
  // taller, more uneven, and sway faster.
  // (gWild inlined — the VM caps a script at 32 locals and this effect is near it)
  let grassH = 1.0 + params.grassy * (1.0 + (sinLUT(x * 0.9) * 0.9 + sinLUT(t * (1.0 + params.grassy * 1.4) + x * 0.6) * 0.7) * 1.3);
  let grassTop = 16.0 - grassH;
  let isGrass = 0;
  if (y == 15) { isGrass = 1; }
  if (y >= grassTop) { isGrass = 1; }

  // --- Faint background scene (painted first; grass + chicken draw over it).
  // Sky: gentle vertical gradient, a touch brighter and bluer toward the top.
  let r = 4 + (15.0 - y) * 0.3;
  let g = 9 + (15.0 - y) * 0.5;
  let b = 18 + (15.0 - y) * 0.9;

  // Rolling hills on the horizon — a dim muted-green band behind the grass.
  // Tall now: peaks reach ~row 7 (a pixel above the panel's midline), rolling
  // down to ~row 11 in the valleys.
  if (y >= 11.0 - (sinLUT(x * 0.5) + 1.0) * 2.0) {
    r = 10; g = 26; b = 20;
  }

  // Stable behind the chicken: two posts + two rails of faint brown branch.
  if ((x == 2 || x == 11) && y >= 5 && y <= 13) {
    r = 58; g = 36; b = 16;
  }
  if (y == 6 || y == 10) {
    r = 58; g = 36; b = 16;
  }

  // 3-pixel sun tucked into the upper-right corner — soft warm glow.
  if ((x == 15 && y == 0) || (x == 14 && y == 0) || (x == 15 && y == 1)) {
    r = 115; g = 85; b = 30;
  }

  if (isGrass == 1) {
    // green blade — brighter/yellower toward the tip. grassy also drives the
    // hue: short stiff grass (low) reads dry and olive (more red, less green),
    // tall wild grass (high) reads lush and saturated.
    let blade = clamp01((16.0 - y) / 3.0);
    r = (55 - params.grassy * 35) + blade * 40;
    g = (70 + params.grassy * 75) + sinLUT(x * 1.7) * 25 + blade * 55;
    b = (15 + params.grassy * 22) + blade * 12;
  }

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
