// Default "no-op" animation. Paints the entire grid in the current baseColor
// (set via /api/color, typically by Home Assistant) so the lamp behaves like
// a regular smart bulb when js mode is selected without picking a script.
function render(frame, width, height, baseColor) {
  let pixels = [];
  let total = width * height;
  let r = baseColor[0];
  let g = baseColor[1];
  let b = baseColor[2];
  for (let i = 0; i < total; i++) {
    pixels.push([r, g, b]);
  }
  return pixels;
}
