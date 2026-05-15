// Heartbeat — two pulses per cycle in a "lub-dub" shape, then quiet until
// the next beat. The pulses are short half-sine arches at fixed fractions
// of the cycle (15% wide for each), so the rhythm holds its character at
// any BPM:
//
//   |^^|--|^^^|-------|        (^ = S1 lub, ^^ = S2 dub, - = quiet)
//   0 .15 .20  .35       1.0
//
// S1 ("lub", mitral/tricuspid closure) peaks at 0.7; S2 ("dub", aortic/
// pulmonic closure) peaks at 1.0 — matches the relative loudness of a
// real auscultation.

// @param bpm 30..180 = 70 Beats per minute
// @param floor 0..0.5 = 0.0 Minimum brightness (keeps the panel glowing between beats)

function shade(x, y, idx, frame, base, params) {
  let cycleMs = 60000 / params.bpm;
  let t = time % cycleMs;
  let phase = t / cycleMs;       // 0..1 in this beat

  // S1: lub, 0..15% of cycle, peak 0.7
  let s1 = 0;
  if (phase < 0.15) {
    s1 = sinLUT(phase / 0.15 * 3.14159) * 0.7;
  }
  // S2: dub, 20..35% of cycle, peak 1.0
  let s2 = 0;
  if (phase >= 0.20 && phase < 0.35) {
    s2 = sinLUT((phase - 0.20) / 0.15 * 3.14159);
  }

  let bright = max(s1, s2);
  bright = params.floor + bright * (1 - params.floor);
  emitBright(bright);
}
