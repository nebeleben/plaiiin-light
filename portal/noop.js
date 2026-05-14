// Default "no-op" animation. Paints the entire grid in the current baseColor
// so the lamp behaves like a regular smart bulb when js mode is selected
// without picking a script.
//
// Phase 23 — DSL syntax. The compiler emits a single LOAD_BASE_R/G/B +
// EMIT_RGB per pixel; no loops, no allocations, no JS engine. Faster than
// even Frame.fillSolid was under JerryScript.

function shade(x, y, idx, frame, base, params) {
  emit(base.r, base.g, base.b);
}
