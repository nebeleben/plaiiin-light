// @mode mirror
// @modeSwitch

// Travelling sine wave along the LED index. Each LED's brightness is a sine
// of its position, and the whole wave scrolls over time. Pure shader (no
// state) — a faithful port of the C++ SinWavePattern.
//
// brightness = offset + amplitude * sin(positionPhase + phase), where
// positionPhase = idx / wavelength * 2π and phase advances with `time`.
// `direction` is the C++ contract: 0 = forward, anything else = reverse.

// @param amplitude 0..1 = 0.5 Sine swing around the offset
// @param offset 0..1 = 0.5 Mid-point brightness the wave oscillates about
// @param wavelength 1..64 = 6 LEDs per full wave (larger = longer wave)
// @param speed 0..5 = 0.5 Scroll speed in wave-cycles per second
// @param direction 0..1 = 0 0 = forward, 1 = reverse

function shade(x, y, idx, frame, base, params) {
  // `time` is wall-clock ms since /api/play. Driving `phase` from it keeps
  // the scroll speed in real seconds regardless of render fps — replaces
  // the C++ per-frame delta accumulation.
  let twoPi = 6.2831853;

  // direction param: 0 -> +1 (forward), else -> -1, matching the C++
  // `direction = (d == 0) ? 1 : -1`.
  let dir = 1;
  if (params.direction != 0) {
    dir = -1;
  }

  // phase = direction * speed * 2π * (time / 1000).
  let phase = dir * params.speed * twoPi * (time / 1000);

  // positionPhase = idx / wavelength * 2π.
  let positionPhase = idx / params.wavelength * twoPi;

  let bright = params.offset + params.amplitude * sinLUT(positionPhase + phase);

  // C++ clamps brightness to [0,1] before scaling baseColor.
  bright = clamp01(bright);
  emitBright(bright);
}
