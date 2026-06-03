/* PlaiiinLight Bytecode (plbc) — Phase 23.
 *
 * Replaces the general-purpose JS engine with a stack VM tuned for per-pixel
 * shader-style scripts. Authors write a JS-like DSL; the compiler emits
 * bytecode; the device interprets it per pixel per frame.
 *
 * Why this exists: JerryScript's external-call overhead is ~184 µs/call on
 * ESP32 (~44k cycles for the bytecode→native→bytecode round-trip). For a
 * 256-LED panel with one setPixel per pixel, that alone burns 47 ms/frame
 * before any script work runs. A purpose-built VM with a tight dispatch
 * loop and indexed register access drops that to ~5–20 cycles per opcode.
 *
 * Lifecycle:
 *   1. Source PUT to /api/js/<name>
 *   2. plbc_compile() parses + validates + emits bytecode into a buffer
 *   3. Buffer is stored as <name>.bc alongside <name>.js
 *   4. /api/play loads <name>.bc via plbc_load() into a program struct
 *   5. Per frame: plbc_frame_setup() then plbc_run_pixel() per LED
 *
 * Threading: program state (locals, fstate, pstate) is owned by the player
 * task; no other task touches it. Compilation is a separate call and
 * doesn't share state with running programs.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define PLBC_MAGIC       "PLBC"
/* v2 (Phase 41): per-param `type` byte (range vs switch) + a program-level
 * `mode` byte in the header (declared wormhole render mode). v1 .bc lack both;
 * the boot recompile pass (js_storage_bc_current) rebuilds them on upgrade. */
#define PLBC_VERSION     2

#define PLBC_MAX_PARAMS         12
/* Bumped from 16 → 32 in Phase 23 so multi-slot effects (e.g. shootingstar
 * with 4 stars × 6 fields = 24) can keep their state in one program. The
 * `n_frame_state` wire field is u8 so 32 is well within format limits;
 * runtime cost is one float per slot per program. */
#define PLBC_MAX_FRAME_STATE    32
#define PLBC_MAX_PIXEL_STATE     8
#define PLBC_MAX_LOCALS         32
#define PLBC_STACK_DEPTH        32
#define PLBC_MAX_NAME           24
#define PLBC_MAX_DESC           96
#define PLBC_MAX_CODE         4096

/* Opcodes. Single-byte; some take inline operands (immediates / indices /
 * branch offsets) following the opcode byte. */
typedef enum {
    PLBC_NOP            = 0x00,

    /* Stack push */
    PLBC_PUSH_F32       = 0x01,  /* + f32 little-endian */
    PLBC_PUSH_I8        = 0x02,  /* + i8, sign-extended to float */
    PLBC_PUSH_ZERO      = 0x03,
    PLBC_PUSH_ONE       = 0x04,
    PLBC_DUP            = 0x05,
    PLBC_DROP           = 0x06,
    PLBC_SWAP           = 0x07,

    /* Locals (per-call temporaries) */
    PLBC_LOAD_LOCAL     = 0x10,  /* + u8 idx */
    PLBC_STORE_LOCAL    = 0x11,

    /* Per-pixel inputs (no operand) */
    PLBC_LOAD_X         = 0x20,
    PLBC_LOAD_Y         = 0x21,
    PLBC_LOAD_IDX       = 0x22,
    PLBC_LOAD_W         = 0x23,
    PLBC_LOAD_H         = 0x24,
    PLBC_LOAD_FRAME     = 0x25,
    PLBC_LOAD_BASE_R    = 0x26,
    PLBC_LOAD_BASE_G    = 0x27,
    PLBC_LOAD_BASE_B    = 0x28,
    PLBC_LOAD_PLAY_START= 0x29,  /* ms-since-boot at js_player_start; per-run seed */
    PLBC_LOAD_TIME      = 0x2A,  /* ms since this playback started; advances per frame */

    /* Params (read-only at runtime) */
    PLBC_LOAD_PARAM     = 0x30,  /* + u8 idx */

    /* State */
    PLBC_LOAD_FSTATE    = 0x40,  /* + u8 idx */
    PLBC_STORE_FSTATE   = 0x41,
    PLBC_LOAD_PSTATE    = 0x42,
    PLBC_STORE_PSTATE   = 0x43,

    /* Arithmetic */
    PLBC_ADD            = 0x50,
    PLBC_SUB            = 0x51,
    PLBC_MUL            = 0x52,
    PLBC_DIV            = 0x53,
    PLBC_MOD            = 0x54,
    PLBC_NEG            = 0x55,
    PLBC_ABS            = 0x56,
    PLBC_FLOOR          = 0x57,
    PLBC_CEIL           = 0x58,
    PLBC_ROUND          = 0x59,
    PLBC_MIN            = 0x5A,
    PLBC_MAX            = 0x5B,
    PLBC_SQRT           = 0x5C,
    PLBC_POW            = 0x5D,
    PLBC_CLAMP01        = 0x5E,  /* clamp top to [0,1] — fast path */

    /* Trig */
    PLBC_SIN_LUT        = 0x60,
    PLBC_COS_LUT        = 0x61,

    /* Hash / random */
    PLBC_HASH_I         = 0x68,  /* Knuth hash on top */
    PLBC_RANDOM         = 0x69,  /* push uniform [0,1) from esp_random */

    /* Compare (push 1.0 / 0.0) */
    PLBC_LT             = 0x70,
    PLBC_LE             = 0x71,
    PLBC_GT             = 0x72,
    PLBC_GE             = 0x73,
    PLBC_EQ             = 0x74,
    PLBC_NE             = 0x75,
    PLBC_NOT            = 0x76,
    PLBC_AND            = 0x77,  /* logical: 0 vs non-0 → 0/1 */
    PLBC_OR             = 0x78,

    /* Branch (i16 LE relative to byte after operand) */
    PLBC_JMP            = 0x80,
    PLBC_JMP_IF_FALSE   = 0x81,  /* pops */
    PLBC_JMP_IF_TRUE    = 0x82,

    /* Emit (write current pixel) */
    PLBC_EMIT_RGB       = 0x90,  /* pop b, g, r → [r,g,b] */
    PLBC_EMIT_BRIGHT    = 0x91,  /* pop bright → baseColor × bright */

    PLBC_HALT           = 0xFF,
} plbc_op_t;

/* Param UI hint. The VM always treats a param as a float in [min,max]; this
 * only tells clients how to render it. PLBC_PARAM_SWITCH is a 0..1 toggle. */
typedef enum {
    PLBC_PARAM_RANGE  = 0,  /* slider/knob over [min,max] (default) */
    PLBC_PARAM_SWITCH = 1,  /* 0/1 toggle — declared as `@param N switch = 0` */
} plbc_param_type_t;

/* In-memory representation of a compiled program. Layout matches the .bc
 * file exactly so we can load by mmap or by memcpy without parsing. */
typedef struct {
    char  name[PLBC_MAX_NAME];
    float min, max, def, value;
    char  desc[PLBC_MAX_DESC];
    uint8_t type;  /* plbc_param_type_t — Phase 41 */
} plbc_param_t;

typedef struct {
    char  name[PLBC_MAX_NAME];
    float def;
} plbc_state_def_t;

typedef struct {
    /* Schema */
    uint8_t n_params;
    uint8_t n_frame_state;
    uint8_t n_pixel_state;
    uint8_t n_locals;
    uint16_t code_size;

    /* Phase 41 — declared wormhole render mode, parsed from `// @mode
     * strip|mirror`. -1 = none (effect carries no mode hint), 0 = strip,
     * 1 = mirror. Drives the auto-switch in js_api_play() and is surfaced to
     * clients via plbc_params_to_json so the tile can show the mode toggle. */
    int8_t mode;
    /* Phase 41 — whether the user may change the render mode away from `mode`.
     * Parsed from `// @modeSwitch` (presence = 1). 0 (default / absent) means
     * the effect only makes sense in its declared `mode`, so clients hide the
     * toggle; 1 means it works in both, so the toggle is shown. Stored in the
     * v2 header's byte-7 slot (was reserved-0), so old .bc read back as 0. */
    uint8_t mode_switch;

    plbc_param_t     params[PLBC_MAX_PARAMS];
    plbc_state_def_t frame_state_defs[PLBC_MAX_FRAME_STATE];
    plbc_state_def_t pixel_state_defs[PLBC_MAX_PIXEL_STATE];

    /* Bytecode buffer — owned by the program. */
    uint8_t code[PLBC_MAX_CODE];
} plbc_program_t;

/* Per-execution state — owned by the caller (player_task / validate). The
 * bytecode is in `prog`; everything that mutates lives here. */
typedef struct {
    float frame_state[PLBC_MAX_FRAME_STATE];
    float *pixel_state;          /* malloc'd: n_pixel_state * total_pixels */
    int total_pixels;
    int w, h;

    /* Pre-loaded per-frame constants */
    uint32_t frame_idx;
    uint8_t base_r, base_g, base_b;
    /* ms-since-boot at the moment js_player_start fired. Constant during
     * a playback. Used as a seed input by scripts that want a different
     * per-LED pattern every run, e.g. fade does `hash(idx + playStart)`.
     * Masked to 24 bits so it stays exact in float32. */
    uint32_t play_start_ms;
    /* Full-precision ms-since-boot at start of playback. NOT exposed to
     * scripts directly — used by the player to compute `now_ms = current -
     * play_start_full_ms` per frame. */
    uint32_t play_start_full_ms;
    /* Ms since play started, snapshotted at frame setup so all pixels in
     * one frame see the same value. Exposed to scripts as `time` — the
     * right way to drive animations now that fps is variable (the old
     * `frame * dt` pattern was implicitly assuming 10 fps). */
    uint32_t now_ms;
} plbc_runtime_t;

/* Compile JS-subset source to a program. On error, returns ESP_FAIL and
 * writes a human-readable message to err_buf (NUL-terminated). */
esp_err_t plbc_compile(const char *source, size_t source_len,
                       plbc_program_t *out_prog,
                       char *err_buf, size_t err_buf_size);

/* Load a previously-compiled bytecode buffer into a program. Validates
 * magic / version / sizes. */
esp_err_t plbc_load(const uint8_t *buf, size_t buf_len,
                    plbc_program_t *out_prog,
                    char *err_buf, size_t err_buf_size);

/* Serialize a program into a buffer (the .bc file format). Returns bytes
 * written or -1 on overflow. */
int plbc_serialize(const plbc_program_t *prog, uint8_t *buf, size_t buf_size);

/* Initialise a runtime against a program + LED grid. Allocates pixel_state.
 * Caller must free via plbc_runtime_free. */
esp_err_t plbc_runtime_init(plbc_runtime_t *rt, const plbc_program_t *prog,
                            int w, int h);

void plbc_runtime_free(plbc_runtime_t *rt);

/* Reset frame & pixel state to the program's declared defaults. Call this
 * on every /api/play so a previous run's state doesn't leak in. */
void plbc_runtime_reset(plbc_runtime_t *rt, const plbc_program_t *prog);

/* Run one pixel's shader. Returns ESP_OK on success, ESP_FAIL on VM trap
 * (stack overflow, divide-by-zero, etc.). Writes the emitted RGB into
 * out_r/g/b if the script called emit(); leaves them zero otherwise. */
esp_err_t plbc_run_pixel(const plbc_program_t *prog,
                         plbc_runtime_t *rt,
                         int pixel_idx,
                         uint8_t *out_r, uint8_t *out_g, uint8_t *out_b);

/* Apply a JSON param patch — {"name": value, ...}. Returns count updated.
 * Used by /api/js/<name>/params PUT. */
int plbc_apply_params_json(plbc_program_t *prog, const char *json, size_t len);

/* Serialize param schema as JSON for /api/js/<name>/params GET. */
int plbc_params_to_json(const plbc_program_t *prog, char *out, size_t max);

/* Like plbc_params_to_json but `include_desc` toggles the per-param
 * "description" field. The BLE script-params characteristic omits descriptions
 * to keep the payload under the GATT long-read ceiling (~1 KB); HTTP keeps
 * them. plbc_params_to_json() is the include_desc=true wrapper. */
int plbc_params_to_json_ex(const plbc_program_t *prog, char *out, size_t max,
                           bool include_desc);
