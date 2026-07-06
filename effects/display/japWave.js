// @param speed 0.05..2.0 = 0.5 Wave motion speed
// @param scale 0.1..1.0 = 0.35 Arc density
// @param foam 0..1 = 0.6 White foam amount
function shade(x, y, idx, frame, base, params) {
    let t = frame * params.speed * 0.05;
    let s = 5.0 / max(params.scale, 0.05);
    let cx = 0;
    let cy = 0;
    let dx = 0;
    let dy = 0;
    let dist = 0;
    let ring = 0;
    let v = 0;
    let crest = 0;
    let cell = 0;
    let best = 0;

    best = 999.0;
    crest = 0;

    // sample a few overlapping arc centers (seigaiha grid)
    for (let gj = -1; gj <= 1; gj++) {
        for (let gi = -1; gi <= 1; gi++) {
            cx = (floor(x / s) + gi) * s + s * 0.5;
            cy = (floor(y / s) + gj) * s + s * 0.5;
            dx = x - cx;
            dy = y - cy;
            dist = sqrt(dx * dx + dy * dy);
            // animated ripple radius
            ring = abs(((dist / s) + t) % 1.0 - 0.5) * 2.0;
            if (dist < best) {
                best = dist;
                crest = ring;
            }
        }
    }

    // wave body brightness from ripple
    v = 0.35 + crest * 0.65;

    // foam highlight near crest peaks
    cell = clamp01((crest - 0.7) * 3.0) * params.foam;

    let r = 0;
    let g = 0;
    let b = 0;
    r = base.r * v + 255 * cell;
    g = base.g * v + 255 * cell;
    b = base.b * v + 255 * cell;

    emit(min(r, 255), min(g, 255), min(b, 255));
}
