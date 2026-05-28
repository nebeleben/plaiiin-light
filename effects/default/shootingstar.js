// Shooting-star overlay. Every pixel normally sits at baseColor; meteors
// pass through and brighten the pixels they touch, with the brightness
// decaying back to baseColor over the next several frames (the tail).
//
// State model: 4 independent "slots", each one tracks one in-flight star
// (position, velocity, visual size, active flag). Per-pixel `fresh` holds
// the current overlay strength (0..1) and decays each frame; passing
// stars refresh it. The per-frame slot bookkeeping (advance + maybe
// respawn) runs inside `if (idx == 0)` — once per render, before the
// first pixel is shaded.
//
// Star characteristics:
//   * size 1..4 px (random per spawn)
//   * speed 0.4..1.0 px/frame (random), bigger stars trend slower from
//     the size-vs-speed correlation in the spawn dice
//   * angle uniformly random in [0, 2π) — stars come from any side
//   * decay tail length follows `decayRate` (higher = longer trail)
//
// Density controls the per-frame spawn probability of dormant slots, so
// at high density multiple stars are in flight simultaneously.

// @state.pixel fresh: 0

// @state s0_x: -10
// @state s0_y: -10
// @state s0_vx: 0
// @state s0_vy: 0
// @state s0_size: 2
// @state s0_active: 0
// @state s1_x: -10
// @state s1_y: -10
// @state s1_vx: 0
// @state s1_vy: 0
// @state s1_size: 2
// @state s1_active: 0
// @state s2_x: -10
// @state s2_y: -10
// @state s2_vx: 0
// @state s2_vy: 0
// @state s2_size: 2
// @state s2_active: 0
// @state s3_x: -10
// @state s3_y: -10
// @state s3_vx: 0
// @state s3_vy: 0
// @state s3_size: 2
// @state s3_active: 0

// @param density 0..1 = 0.5 Average number of stars in flight (higher = more simultaneous)
// @param contrast 0..1 = 0.7 How much brighter the stars are than baseColor
// @param decayRate 0.5..0.99 = 0.85 Tail fade-out rate per frame (higher = longer tail)

function shade(x, y, idx, frame, base, params) {

  // ---------- Per-frame slot pool update (runs once before pixel 0) ----------
  if (idx == 0) {
    let angle = 0;
    let speed = 0;

    // Slot 0
    if (s0_active > 0) {
      s0_x = s0_x + s0_vx;
      s0_y = s0_y + s0_vy;
      if (s0_x < -3 || s0_x > w + 3 || s0_y < -3 || s0_y > h + 3) {
        s0_active = 0;
      }
    } else {
      if (random() < params.density * 0.05) {
        angle = random() * 6.28318;
        speed = 0.4 + random() * 0.6;
        s0_vx = cosLUT(angle) * speed;
        s0_vy = sinLUT(angle) * speed;
        // Start ~30 frames "behind" panel centre along trajectory so the
        // star enters from off-panel and traverses cleanly.
        s0_x = w * 0.5 - s0_vx * 30;
        s0_y = h * 0.5 - s0_vy * 30;
        s0_size = 1 + random() * 3;
        s0_active = 1;
      }
    }

    // Slot 1
    if (s1_active > 0) {
      s1_x = s1_x + s1_vx;
      s1_y = s1_y + s1_vy;
      if (s1_x < -3 || s1_x > w + 3 || s1_y < -3 || s1_y > h + 3) {
        s1_active = 0;
      }
    } else {
      if (random() < params.density * 0.05) {
        angle = random() * 6.28318;
        speed = 0.4 + random() * 0.6;
        s1_vx = cosLUT(angle) * speed;
        s1_vy = sinLUT(angle) * speed;
        s1_x = w * 0.5 - s1_vx * 30;
        s1_y = h * 0.5 - s1_vy * 30;
        s1_size = 1 + random() * 3;
        s1_active = 1;
      }
    }

    // Slot 2
    if (s2_active > 0) {
      s2_x = s2_x + s2_vx;
      s2_y = s2_y + s2_vy;
      if (s2_x < -3 || s2_x > w + 3 || s2_y < -3 || s2_y > h + 3) {
        s2_active = 0;
      }
    } else {
      if (random() < params.density * 0.05) {
        angle = random() * 6.28318;
        speed = 0.4 + random() * 0.6;
        s2_vx = cosLUT(angle) * speed;
        s2_vy = sinLUT(angle) * speed;
        s2_x = w * 0.5 - s2_vx * 30;
        s2_y = h * 0.5 - s2_vy * 30;
        s2_size = 1 + random() * 3;
        s2_active = 1;
      }
    }

    // Slot 3
    if (s3_active > 0) {
      s3_x = s3_x + s3_vx;
      s3_y = s3_y + s3_vy;
      if (s3_x < -3 || s3_x > w + 3 || s3_y < -3 || s3_y > h + 3) {
        s3_active = 0;
      }
    } else {
      if (random() < params.density * 0.05) {
        angle = random() * 6.28318;
        speed = 0.4 + random() * 0.6;
        s3_vx = cosLUT(angle) * speed;
        s3_vy = sinLUT(angle) * speed;
        s3_x = w * 0.5 - s3_vx * 30;
        s3_y = h * 0.5 - s3_vy * 30;
        s3_size = 1 + random() * 3;
        s3_active = 1;
      }
    }
  }

  // ---------- Per-pixel overlay -----------------------------------------
  // Decay last frame's overlay, then bump back up if a star is nearby.
  let bright = fresh.pixel * params.decayRate;

  if (s0_active > 0) {
    let d0 = max(abs(x - s0_x), abs(y - s0_y));
    if (d0 < s0_size) {
      let c0 = 1 - d0 / s0_size;
      if (c0 > bright) { bright = c0; }
    }
  }
  if (s1_active > 0) {
    let d1 = max(abs(x - s1_x), abs(y - s1_y));
    if (d1 < s1_size) {
      let c1 = 1 - d1 / s1_size;
      if (c1 > bright) { bright = c1; }
    }
  }
  if (s2_active > 0) {
    let d2 = max(abs(x - s2_x), abs(y - s2_y));
    if (d2 < s2_size) {
      let c2 = 1 - d2 / s2_size;
      if (c2 > bright) { bright = c2; }
    }
  }
  if (s3_active > 0) {
    let d3 = max(abs(x - s3_x), abs(y - s3_y));
    if (d3 < s3_size) {
      let c3 = 1 - d3 / s3_size;
      if (c3 > bright) { bright = c3; }
    }
  }

  fresh.pixel = bright;

  // Overlay blend: `bright * contrast` interpolates from baseColor (bright=0)
  // toward white (bright=1, contrast=1).
  let cval = bright * params.contrast;
  emit(
    base.r + (255 - base.r) * cval,
    base.g + (255 - base.g) * cval,
    base.b + (255 - base.b) * cval
  );
}
