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
#define PLBC_VERSION     1

#define PLBC_MAX_PARAMS         12
#define PLBC_MAX_FRAME_STATE    16
#define PLBC_MAX_PIXEL_STATE     8
#define PLBC_MAX_LOCALS         16
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

/* In-memory representation of a compiled program. Layout matches the .bc
 * file exactly so we can load by mmap or by memcpy without parsing. */
typedef struct {
    char  name[PLBC_MAX_NAME];
    float min, max, def, value;
    char  desc[PLBC_MAX_DESC];
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
