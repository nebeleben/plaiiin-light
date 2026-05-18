// @mode mirror
//
// Comet — a single bright head chases its own fading tail around one ring.
// In mirror mode the firmware tiles this one 24-LED ring onto every physical
// ring of the wormhole, so the whole construct shows the same comet spinning
// in sync (per-ring offsets/brightness still applied by the creative config).
//
// On the lamp: one bright dot races around the circle, dragging a comet tail
// that fades behind it. Clean and hypnotic — the wormhole looks like it is
// slowly rotating.
//
// Mirror mode: the script only ever sees a 24×1 grid — x = 0..23 around the
// ring, y always 0. The head position and every pixel angle are derived from
// x/24, so the comet wraps seamlessly across the x 0/23 seam.
//
// `speed` is revolutions per second; `tail` is how far the tail stretches
// behind the head (in ring fractions); `hue` tints the comet.

// @param speed 0.05..2 = 0.4 Revolutions per second
// @param tail 0.05..0.9 = 0.4 Tail length as a fraction of the ring
// @param hue 0..1 = 0.55 Comet colour, 0 = magenta, 1 = cyan

function shade(x, y, idx, frame, base, params) {
  let t = time * 0.001;

  // Head position around the ring as a 0..1 fraction.
  let head = (t * params.speed) % 1;

  // This pixel's position around the ring, 0..1.
  let pos = x / w;

  // Signed gap from head BACK to this pixel, wrapped into 0..1. A pixel just
  // behind the head has a small gap; the tail occupies gap < `tail`.
  let gap = head - pos;
  gap = gap - floor(gap);

  let bright = 0;
  if (gap < params.tail) {
    // Brightest at the head (gap ~0), fading to 0 at the tail end.
    bright = 1 - gap / params.tail;
    bright = bright * bright * bright;   // cubic → punchy head, soft tail
  }

  // Colour lerp magenta -> cyan, scaled by brightness.
  let r = (255 - params.hue * 255) * bright;
  let g = (params.hue * 220) * bright;
  let b = (255 - params.hue * 40) * bright;
  emit(r, g, b);
}
