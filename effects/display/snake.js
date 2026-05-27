// Snake — an auto-play snake wanders the panel, eats fruit, and grows.
// The head moves one cell every `moveInterval` ms (slowed by `speed`),
// steered by a simple greedy heuristic: prefer the axis with the larger
// gap to the fruit; if blocked by a wall, turn 90°. Self-collision is
// not avoided (the snake CAN eat itself) — when it does, it resets after
// a brief dead-flash.
//
// State plumbing:
//   - Frame state holds head position + direction + length + fruit + the
//     last-move timestamp. Only pixel idx=0 updates this each frame (it's
//     the controller pixel); subsequent pixels read it for rendering.
//   - Each pixel keeps an `age` in pixel state. When the head visits a
//     cell, age is set to `length`; every move tick, every pixel
//     decrements its age by 1. age > 0 → this pixel is part of the body
//     trail, brightness scales with age/length.
//
// byForm display effect.

// @param speed 0.5..5 = 2 Moves per second (head cell advances per second)
// @param initLength 3..14 = 5 Length the snake starts (and respawns) with
// @param eatGrowth 1..5 = 2 Cells added to length per fruit
// @param headHue 0..1 = 0.35 Head colour hue (0 = pure base, 1 = bright green)
// @param fruitPulse 0..1 = 0.7 Fruit blink depth (0 = solid red, 1 = full pulse)

// @state head_x : 8
// @state head_y : 8
// @state head_dx : 1
// @state head_dy : 0
// @state length : 5
// @state fruit_x : 3
// @state fruit_y : 3
// @state last_ms : 0
// @state moved : 0
// @state respawn_ms : 0

// @state.pixel age : 0

function shade(x, y, idx, frame, base, params) {
  /* -------------------- controller: idx == 0 only -------------------- */
  if (idx == 0) {
    let interval = 1000 / params.speed;
    moved = 0;
    if (respawn_ms > 0 && time >= respawn_ms) {
      /* End of death-flash: reset snake state. */
      head_x = floor(w * 0.5);
      head_y = floor(h * 0.5);
      head_dx = 1;
      head_dy = 0;
      length = params.initLength;
      fruit_x = floor(hash(time + 17) * w);
      fruit_y = floor(hash(time + 113) * h);
      last_ms = time;
      respawn_ms = 0;
    } else if (respawn_ms == 0 && time >= last_ms + interval) {
      moved = 1;
      /* Pick a new direction: prefer larger-gap axis toward fruit. */
      let gapx = fruit_x - head_x;
      let gapy = fruit_y - head_y;
      let new_dx = 0;
      let new_dy = 0;
      if (abs(gapx) >= abs(gapy) && gapx != 0) {
        if (gapx > 0) { new_dx = 1; } else { new_dx = -1; }
      } else if (gapy != 0) {
        if (gapy > 0) { new_dy = 1; } else { new_dy = -1; }
      } else {
        new_dx = head_dx; new_dy = head_dy;
      }
      /* Wall check on chosen direction; if blocked, fall back to current. */
      let nx = head_x + new_dx;
      let ny = head_y + new_dy;
      if (nx < 0 || nx >= w || ny < 0 || ny >= h) {
        new_dx = head_dx; new_dy = head_dy;
        nx = head_x + new_dx;
        ny = head_y + new_dy;
      }
      /* Still blocked (head was already cornered): turn 90° clockwise. */
      if (nx < 0 || nx >= w || ny < 0 || ny >= h) {
        let tdx = new_dy;
        let tdy = 0 - new_dx;
        new_dx = tdx; new_dy = tdy;
        nx = head_x + new_dx;
        ny = head_y + new_dy;
        if (nx < 0 || nx >= w || ny < 0 || ny >= h) {
          /* Still stuck — die and respawn. */
          respawn_ms = time + 600;
        }
      }
      if (respawn_ms == 0) {
        head_dx = new_dx;
        head_dy = new_dy;
        head_x = nx;
        head_y = ny;
        last_ms = time;
        /* Fruit eaten? */
        if (head_x == fruit_x && head_y == fruit_y) {
          length = length + params.eatGrowth;
          fruit_x = floor(hash(time + 17) * w);
          fruit_y = floor(hash(time + 113) * h);
        }
      }
    }
  }

  /* -------------------- render: every pixel -------------------- */
  let isHead = (x == head_x && y == head_y && respawn_ms == 0);
  if (isHead) { age.pixel = length; }
  /* Decrement age once per move tick (not per frame), so length controls
   * trail length in cells. */
  if (moved == 1 && age.pixel > 0 && !isHead) {
    age.pixel = age.pixel - 1;
  }
  let myAge = age.pixel;
  /* During death-flash, snap all body to a brief white blink. */
  if (respawn_ms > 0) {
    let flashOn = ((time - (respawn_ms - 600)) / 100);
    flashOn = floor(flashOn) - 2 * floor(flashOn / 2);
    if (myAge > 0 && flashOn == 0) {
      emit(255, 255, 255);
    } else {
      emit(0, 0, 0);
    }
  } else if (isHead) {
    /* Head: bright green tinted toward base. */
    let mix = params.headHue;
    let or_ = base.r * (1 - mix) + 60 * mix;
    let og = base.g * (1 - mix) + 255 * mix;
    let ob = base.b * (1 - mix) + 60 * mix;
    emit(or_, og, ob);
  } else if (myAge > 0) {
    /* Body: green, dimming toward the tail by age/length. */
    let f = myAge / max(1, length);
    let bmix = params.headHue * f;
    let br_ = base.r * (1 - bmix) + 30 * bmix;
    let bg_ = base.g * (1 - bmix) + 200 * bmix;
    let bb_ = base.b * (1 - bmix) + 30 * bmix;
    emit(br_, bg_, bb_);
  } else if (x == fruit_x && y == fruit_y) {
    /* Fruit: red, pulsing. */
    let p = 1 - params.fruitPulse * 0.5 * (1 + sinLUT(time * 0.003));
    emit(255 * p, 30 * p, 30 * p);
  } else {
    emit(0, 0, 0);
  }
}
