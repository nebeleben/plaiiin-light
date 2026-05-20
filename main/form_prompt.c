#include "form_prompt.h"
#include "config_store.h"
#include "led_control.h"
#include "wormhole.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// Phase 26 — the per-form descriptor template. Two sources, in priority order:
//   1. /storage/_form_template.txt — flashed by `profile-burn.sh --full` from
//      lampos/form-template/<form>.txt. Lets the wording be reworded + re-burned
//      without a firmware release. Carries {placeholder} tokens for geometry.
//   2. A hardcoded fallback below, used when the device was never --full-burned.
#define FORM_TEMPLATE_PATH "/storage/_form_template.txt"

// Parse the leading panel dimension out of a lamp_type like "matrix8x8" or
// "matrix16x16". Returns 0 if the type is not a matrix.
static int panel_side(const char *lamp_type)
{
    const char *p = strstr(lamp_type, "matrix");
    if (!p) return 0;
    return atoi(p + 6);
}

// Hardcoded fallback descriptor — geometry interpolated directly. Used when no
// burned template file is present. `mode` is the wormhole render mode (0 =
// strip, 1 = mirror); it is ignored for every non-wormhole form.
static void build_hardcoded(char *out, size_t max_len,
                            const char *lamp_form, const char *lamp_type,
                            int w, int h, int count, int mode)
{
    if (strcmp(lamp_form, "tower") == 0) {
        snprintf(out, max_len,
            "PHYSICAL FORM: tower (cylinder). The %dx%d pixel grid is wrapped "
            "around a cylinder, so column %d sits physically next to column 0 "
            "— there is no left or right edge. Horizontal motion must be "
            "seamless across that seam: derive an angle from x as (x / %d) and "
            "drive it with sinLUT/cosLUT so the pattern meets itself. y is "
            "vertical (0 = top, %d = bottom) with hard top and bottom edges.",
            w, h, w - 1, w, h - 1);
    } else if (strcmp(lamp_form, "cube") == 0) {
        int n = panel_side(lamp_type);
        if (n <= 0) n = 8;
        int per = n * n;
        int faces = (per > 0) ? count / per : 5;
        snprintf(out, max_len,
            "PHYSICAL FORM: cube. %d square %dx%d panels (%d LEDs total) form a "
            "5-sided cube. Faces in wiring order: A=front, B=right, C=back, "
            "D=left, E=top. From the flat index: face = floor(idx / %d); within "
            "a face row = floor((idx mod %d) / %d) and col = idx mod %d. Faces "
            "A,B,C,D form a horizontal band around the cube (B,C,D continue on "
            "from A); E caps the top. Prefer idx-based math for effects that "
            "cross faces — the x/y grid is meaningful per-face only.",
            faces, n, n, count, per, per, n, n);
    } else if (strcmp(lamp_form, "wormhole") == 0) {
        int rings = wormhole_is_wormhole() ? wormhole_rings() : count / 24;
        if (rings < 1) rings = 1;
        if (mode == 1) {
            // Mirror mode — the script renders ONE 24-LED ring; firmware tiles
            // it onto every physical ring.
            snprintf(out, max_len,
                "PHYSICAL FORM: wormhole (mirror mode). Render ONE 24-LED ring "
                "only — the grid is 24x1, idx is 0..23 = position-on-ring "
                "(wraps seamlessly — derive an angle as idx / 24). The firmware "
                "tiles your single ring onto all %d physical rings, applying "
                "each ring's mount orientation automatically. Write a "
                "self-contained single-ring effect; do NOT think about ring "
                "index or depth — there is only one ring from the script's "
                "point of view.",
                rings);
        } else {
            // Strip mode — the script renders the whole construct.
            snprintf(out, max_len,
                "PHYSICAL FORM: wormhole (strip mode). %d stacked 24-LED rings "
                "(%d LEDs total) form a tunnel. The grid is one flat strip — "
                "use idx (0..%d); x = idx and y = 0. ring = floor(idx / 24); "
                "position-on-ring = idx mod 24 (0..23, wraps seamlessly — "
                "derive an angle as (idx mod 24) / 24). Rings alternate "
                "physical facing: even rings face one way, odd rings the "
                "opposite — mirror odd rings for symmetric tunnel effects. "
                "Depth runs along the ring axis from ring 0 outward.",
                rings, count, count - 1);
        }
    } else if (strcmp(lamp_form, "rocket") == 0) {
        snprintf(out, max_len,
            "PHYSICAL FORM: rocket. A single %d-LED strip wired as stacked "
            "segments from base to tip: booster (an 8-LED ring then a 24-LED "
            "ring), body (two 8x8 matrices rolled into a cylinder), head (an "
            "8-LED ring then a 16-LED ring). Rings are angular — derive an "
            "angle from (idx within the ring) / ringLength. Treat idx ranges "
            "as segments; if this build needs exact segment boundaries they "
            "are in the editable note below. Effects sweeping along idx read "
            "as launch/thrust motion.",
            count);
    } else if (strcmp(lamp_form, "display") == 0) {
        snprintf(out, max_len,
            "PHYSICAL FORM: display (flat wall). A flat rectangular %dx%d "
            "pixel matrix (%d pixels). x increases left-to-right (0..%d), y "
            "increases top-to-bottom (0..%d). All four edges are hard edges "
            "— nothing wraps around.",
            w, h, w * h, w - 1, h - 1);
    } else if (w > 1 && h > 1) {
        snprintf(out, max_len,
            "PHYSICAL FORM: %s. A %dx%d pixel matrix (%d pixels). No edge "
            "wraps unless noted; x is 0..%d, y is 0..%d.",
            lamp_form, w, h, w * h, w - 1, h - 1);
    } else {
        snprintf(out, max_len,
            "PHYSICAL FORM: %s. A single strip of %d LEDs addressed by idx "
            "(0..%d). x/y are not meaningful here — use idx for position.",
            lamp_form, count, count - 1);
    }
}

// Read the burned template file from SPIFFS into `buf`. Returns true on a
// non-empty read. Trailing whitespace/newlines are trimmed.
static bool read_template_file(char *buf, size_t max)
{
    FILE *f = fopen(FORM_TEMPLATE_PATH, "r");
    if (!f) return false;
    size_t n = fread(buf, 1, max - 1, f);
    fclose(f);
    buf[n] = '\0';
    while (n > 0) {
        char c = buf[n - 1];
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t' || c == '\0') {
            buf[--n] = '\0';
        } else {
            break;
        }
    }
    return n > 0;
}

// Phase 29 — a two-mode template (wormhole) splits its content with the
// line-leading markers `@@strip` and `@@mirror`. Extract the section for the
// requested mode (0 = strip, 1 = mirror) out of `tpl` into `out`.
//
// Rule: if `tpl` contains a `@@strip` / `@@mirror` marker at the start of a
// line, the section running from the requested marker up to the next marker
// (or EOF) is copied. If there are no markers the whole template is copied —
// so non-wormhole templates are unaffected. Returns true if a section was
// produced (always true here, just for symmetry with read_template_file).
static bool template_section(const char *tpl, int mode, char *out, size_t max)
{
    const char *want = (mode == 1) ? "@@mirror" : "@@strip";
    size_t want_len = strlen(want);

    // Is this a marker-bearing template at all? A marker counts only when it
    // sits at the start of the buffer or right after a newline.
    bool has_markers = false;
    for (const char *p = tpl; *p; p++) {
        if ((p == tpl || p[-1] == '\n') &&
            (strncmp(p, "@@strip", 7) == 0 || strncmp(p, "@@mirror", 8) == 0)) {
            has_markers = true;
            break;
        }
    }
    if (!has_markers) {
        snprintf(out, max, "%s", tpl);
        return true;
    }

    // Find the requested marker at a line start.
    const char *start = NULL;
    for (const char *p = tpl; *p; p++) {
        if ((p == tpl || p[-1] == '\n') && strncmp(p, want, want_len) == 0) {
            start = p + want_len;
            break;
        }
    }
    if (!start) {
        // Requested mode's section is missing — fall back to the whole file
        // minus any markers would be messy; safest is an empty section so
        // build_hardcoded is used by the caller. Signal "nothing".
        out[0] = '\0';
        return false;
    }
    // Skip the rest of the marker line (whitespace + the newline).
    while (*start == ' ' || *start == '\t' || *start == '\r') start++;
    if (*start == '\n') start++;

    // The section ends at the next line-leading marker, or EOF.
    const char *end = start;
    for (const char *p = start; *p; p++) {
        if (p[-1] == '\n' &&
            (strncmp(p, "@@strip", 7) == 0 || strncmp(p, "@@mirror", 8) == 0)) {
            end = p;
            break;
        }
        end = p + 1;
    }
    size_t n = (size_t)(end - start);
    if (n >= max) n = max - 1;
    memcpy(out, start, n);
    out[n] = '\0';
    // Trim trailing whitespace/newlines, matching read_template_file().
    while (n > 0) {
        char c = out[n - 1];
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') out[--n] = '\0';
        else break;
    }
    return n > 0;
}

typedef struct { const char *key; char val[24]; } subst_t;

// Expand {token} placeholders in `tpl` against `tbl`, writing into `out`.
// Unknown {tokens} are copied through verbatim.
static void apply_subst(const char *tpl, char *out, size_t max,
                        const subst_t *tbl, int tbl_n)
{
    size_t o = 0;
    for (const char *p = tpl; *p && o + 1 < max; ) {
        if (*p == '{') {
            const char *close = strchr(p, '}');
            if (close) {
                size_t klen = (size_t)(close - p - 1);
                for (int i = 0; i < tbl_n; i++) {
                    if (strlen(tbl[i].key) == klen &&
                        strncmp(p + 1, tbl[i].key, klen) == 0) {
                        int wrote = snprintf(out + o, max - o, "%s", tbl[i].val);
                        if (wrote > 0) o += (size_t)wrote;
                        if (o >= max) o = max - 1;
                        p = close + 1;
                        goto next;
                    }
                }
            }
        }
        out[o++] = *p++;
    next:;
    }
    out[o] = '\0';
}

void form_prompt_build_for_mode(int mode, char *out, size_t max_len)
{
    char lamp_type[32], lamp_form[32];
    config_get_str_or(CONFIG_KEY_LAMP_TYPE, lamp_type, sizeof(lamp_type), CONFIG_PLAIIIN_LAMP_TYPE);
    config_get_str_or(CONFIG_KEY_LAMP_FORM, lamp_form, sizeof(lamp_form), CONFIG_PLAIIIN_FORM);

    int count = led_control_get_count();
    int w = led_control_get_logical_w();
    int h = led_control_get_logical_h();

    // For a wormhole lamp the ring count comes from wormhole config (explicit
    // wh_rings, not just count/24). Other forms ignore `mode` entirely.
    int rings = wormhole_is_wormhole() ? wormhole_rings()
                                       : (count / 24 > 0 ? count / 24 : 1);

    // Prefer the burned template file; fall back to the hardcoded descriptor.
    char tpl[1024];
    if (read_template_file(tpl, sizeof(tpl))) {
        // Phase 29 — pick the @@strip / @@mirror section for the requested
        // mode. Markerless templates yield the whole file (non-wormhole forms
        // are unaffected). A missing section falls through to build_hardcoded.
        char section[1024];
        if (template_section(tpl, mode, section, sizeof(section))) {
            int n = panel_side(lamp_type);
            if (n <= 0) n = 8;
            int per = n * n;
            subst_t tbl[] = {
                { "w", {0} }, { "h", {0} }, { "wmax", {0} }, { "hmax", {0} },
                { "wh", {0} }, { "count", {0} }, { "panel", {0} },
                { "panelsq", {0} }, { "faces", {0} }, { "rings", {0} },
            };
            snprintf(tbl[0].val, sizeof(tbl[0].val), "%d", w);
            snprintf(tbl[1].val, sizeof(tbl[1].val), "%d", h);
            snprintf(tbl[2].val, sizeof(tbl[2].val), "%d", w - 1);
            snprintf(tbl[3].val, sizeof(tbl[3].val), "%d", h - 1);
            snprintf(tbl[4].val, sizeof(tbl[4].val), "%d", w * h);
            snprintf(tbl[5].val, sizeof(tbl[5].val), "%d", count);
            snprintf(tbl[6].val, sizeof(tbl[6].val), "%d", n);
            snprintf(tbl[7].val, sizeof(tbl[7].val), "%d", per);
            snprintf(tbl[8].val, sizeof(tbl[8].val), "%d", per > 0 ? count / per : 5);
            snprintf(tbl[9].val, sizeof(tbl[9].val), "%d", rings);
            apply_subst(section, out, max_len, tbl, (int)(sizeof(tbl) / sizeof(tbl[0])));
            return;
        }
    }

    build_hardcoded(out, max_len, lamp_form, lamp_type, w, h, count, mode);
}

void form_prompt_build_default(char *out, size_t max_len)
{
    // The "default" descriptor reflects the current wormhole render mode.
    // Non-wormhole lamps ignore the mode argument.
    int mode = (wormhole_is_wormhole() && wormhole_mode() == WORMHOLE_MODE_MIRROR)
                   ? 1 : 0;
    form_prompt_build_for_mode(mode, out, max_len);
}

void form_prompt_get_effective(char *out, size_t max_len)
{
    if (config_store_get_str(CONFIG_KEY_FORM_PROMPT, out, max_len) == ESP_OK && out[0]) {
        return;
    }
    form_prompt_build_default(out, max_len);
}
