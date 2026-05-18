#pragma once

#include <stddef.h>

/**
 * Phase 26 — per-lamp physical-form descriptor.
 *
 * The `shade()` contract only exposes a flat logical grid (w, h, idx) to
 * scripts, which is not enough for an AI to write form-correct effects: a
 * tower wraps into a cylinder, a cube has five faces, a wormhole stacks
 * rings, a rocket chains mixed segments. This module produces a short
 * human-readable paragraph describing the lamp's real physical arrangement,
 * which the apps and the on-device /compose page append to the AI system
 * prompt.
 *
 * The default descriptor comes from the burned template file
 * `/storage/_form_template.txt` (flashed by `profile-burn.sh --full` from
 * lampos/form-template/<form>.txt), with its {placeholder} tokens filled from
 * live geometry. If no file is present a hardcoded fallback is used. A user
 * can override the result per-lamp via PUT /api/form-prompt (stored in NVS
 * under CONFIG_KEY_FORM_PROMPT) — useful for custom builds whose segment
 * layout the generic template cannot know.
 */

/** Build the firmware default form descriptor for the current lamp form,
 *  interpolating live geometry from led_control + config. NUL-terminates.
 *  For a wormhole lamp this returns the descriptor for the *current* wh_mode. */
void form_prompt_build_default(char *out, size_t max_len);

/** Return the effective descriptor: the NVS override if one is set and
 *  non-empty, otherwise the firmware default. NUL-terminates. */
void form_prompt_get_effective(char *out, size_t max_len);

/** Phase 29 — wormhole mode-aware descriptor. `mode` is 0 = strip, 1 = mirror
 *  (matching wormhole_mode_t). For a wormhole lamp this produces the strip-
 *  vs mirror-specific descriptor; for every other form `mode` is ignored and
 *  the result is identical to form_prompt_build_default(). NUL-terminates. */
void form_prompt_build_for_mode(int mode, char *out, size_t max_len);
