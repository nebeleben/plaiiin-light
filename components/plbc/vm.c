/* PLBC stack-machine VM. Hot path: plbc_run_pixel. Called once per LED per
 * frame, so the dispatch loop is everything — keep it small, branch-predictable,
 * and free of allocations. */

#include "plbc.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_random.h"

#define SIN_LUT_SIZE 256
static float s_sin_lut[SIN_LUT_SIZE];
static bool  s_sin_lut_ready = false;
static const float SIN_LUT_SCALE = (float)SIN_LUT_SIZE / (2.0f * (float)M_PI);

static void sin_lut_init(void)
{
    if (s_sin_lut_ready) return;
    for (int i = 0; i < SIN_LUT_SIZE; i++) {
        s_sin_lut[i] = sinf(2.0f * (float)M_PI * (float)i / (float)SIN_LUT_SIZE);
    }
    s_sin_lut_ready = true;
}

static inline float sin_lut_lookup(float x)
{
    /* Two's-complement wraparound on the cast gives the correct mod-2π
     * bucket for both signs of x — no fmodf, no floor() call. */
    int32_t idx = (int32_t)(x * SIN_LUT_SCALE);
    return s_sin_lut[(uint32_t)idx & (SIN_LUT_SIZE - 1)];
}

/* Knuth multiplicative hash → uniform [0,1). Deterministic per input, so
 * scripts get reproducible "random" patterns frame-over-frame. */
static inline float hash01(float v)
{
    uint32_t x = (uint32_t)((int32_t)v);
    x = x * 2654435761u;
    return (float)x * (1.0f / 4294967296.0f);
}

esp_err_t plbc_runtime_init(plbc_runtime_t *rt, const plbc_program_t *prog,
                            int w, int h)
{
    if (!rt || !prog || w <= 0 || h <= 0) return ESP_ERR_INVALID_ARG;
    sin_lut_init();
    memset(rt, 0, sizeof(*rt));
    rt->w = w;
    rt->h = h;
    rt->total_pixels = w * h;
    rt->base_r = 128;
    rt->base_g = 128;
    rt->base_b = 128;
    if (prog->n_pixel_state > 0) {
        size_t bytes = (size_t)prog->n_pixel_state
                       * (size_t)rt->total_pixels * sizeof(float);
        rt->pixel_state = (float *)calloc(1, bytes);
        if (!rt->pixel_state) return ESP_ERR_NO_MEM;
    }
    plbc_runtime_reset(rt, prog);
    return ESP_OK;
}

void plbc_runtime_free(plbc_runtime_t *rt)
{
    if (!rt) return;
    free(rt->pixel_state);
    rt->pixel_state = NULL;
    rt->total_pixels = 0;
}

void plbc_runtime_reset(plbc_runtime_t *rt, const plbc_program_t *prog)
{
    if (!rt || !prog) return;
    /* Frame state: load declared defaults. */
    for (int i = 0; i < prog->n_frame_state; i++) {
        rt->frame_state[i] = prog->frame_state_defs[i].def;
    }
    /* Per-pixel state: same default applied to every pixel. */
    if (rt->pixel_state) {
        for (int p = 0; p < rt->total_pixels; p++) {
            for (int s = 0; s < prog->n_pixel_state; s++) {
                rt->pixel_state[p * prog->n_pixel_state + s] =
                    prog->pixel_state_defs[s].def;
            }
        }
    }
}

/* Read a 16-bit little-endian operand from `code` at offset `off`, advance off. */
static inline int16_t read_i16(const uint8_t *code, uint16_t *off)
{
    int16_t v = (int16_t)(code[*off] | (code[*off + 1] << 8));
    *off += 2;
    return v;
}

static inline float read_f32(const uint8_t *code, uint16_t *off)
{
    union { uint32_t u; float f; } b;
    b.u = (uint32_t)code[*off]
        | ((uint32_t)code[*off + 1] << 8)
        | ((uint32_t)code[*off + 2] << 16)
        | ((uint32_t)code[*off + 3] << 24);
    *off += 4;
    return b.f;
}

esp_err_t plbc_run_pixel(const plbc_program_t *prog,
                         plbc_runtime_t *rt,
                         int pixel_idx,
                         uint8_t *out_r, uint8_t *out_g, uint8_t *out_b)
{
    if (!prog || !rt) return ESP_ERR_INVALID_ARG;
    if (pixel_idx < 0 || pixel_idx >= rt->total_pixels) return ESP_ERR_INVALID_ARG;

    float stack[PLBC_STACK_DEPTH];
    int sp = 0;
    float locals[PLBC_MAX_LOCALS] = {0};

    const int x = pixel_idx % rt->w;
    const int y = pixel_idx / rt->w;
    float *pstate = rt->pixel_state ?
        &rt->pixel_state[pixel_idx * prog->n_pixel_state] : NULL;

    uint16_t pc = 0;
    const uint8_t *code = prog->code;
    const uint16_t code_size = prog->code_size;

    bool emitted = false;
    if (out_r) *out_r = 0;
    if (out_g) *out_g = 0;
    if (out_b) *out_b = 0;

    /* Macro helpers for the dispatch loop. */
/* Sequence-point-safe push — evaluate `v` (which may read sp) first, then
 * store, then bump sp. Letting the compiler interleave the read of sp in
 * `v` with the post-increment in `stack[sp++] = v` triggers an
 * unsequenced-modification warning that ESP-IDF treats as an error. */
#define PUSH(v) do { \
    if (sp >= PLBC_STACK_DEPTH) goto trap; \
    float __pv = (v); \
    stack[sp] = __pv; \
    sp++; \
} while (0)
#define NEED(n) do { if (sp < (n)) goto trap; } while (0)

    while (pc < code_size) {
        uint8_t op = code[pc++];
        switch (op) {
            case PLBC_NOP:        break;
            case PLBC_PUSH_F32:   PUSH(read_f32(code, &pc)); break;
            case PLBC_PUSH_I8: {
                int8_t v = (int8_t)code[pc++];
                PUSH((float)v);
                break;
            }
            case PLBC_PUSH_ZERO:  PUSH(0.0f); break;
            case PLBC_PUSH_ONE:   PUSH(1.0f); break;
            case PLBC_DUP:        NEED(1); PUSH(stack[sp - 1]); break;
            case PLBC_DROP:       NEED(1); sp--; break;
            case PLBC_SWAP: {
                NEED(2);
                float a = stack[sp - 1]; stack[sp - 1] = stack[sp - 2]; stack[sp - 2] = a;
                break;
            }

            case PLBC_LOAD_LOCAL: {
                uint8_t i = code[pc++];
                if (i >= PLBC_MAX_LOCALS) goto trap;
                PUSH(locals[i]);
                break;
            }
            case PLBC_STORE_LOCAL: {
                uint8_t i = code[pc++];
                if (i >= PLBC_MAX_LOCALS) goto trap;
                NEED(1);
                locals[i] = stack[--sp];
                break;
            }

            case PLBC_LOAD_X:      PUSH((float)x); break;
            case PLBC_LOAD_Y:      PUSH((float)y); break;
            case PLBC_LOAD_IDX:    PUSH((float)pixel_idx); break;
            case PLBC_LOAD_W:      PUSH((float)rt->w); break;
            case PLBC_LOAD_H:      PUSH((float)rt->h); break;
            case PLBC_LOAD_FRAME:  PUSH((float)rt->frame_idx); break;
            case PLBC_LOAD_BASE_R: PUSH((float)rt->base_r); break;
            case PLBC_LOAD_BASE_G: PUSH((float)rt->base_g); break;
            case PLBC_LOAD_BASE_B: PUSH((float)rt->base_b); break;

            case PLBC_LOAD_PARAM: {
                uint8_t i = code[pc++];
                if (i >= prog->n_params) goto trap;
                PUSH(prog->params[i].value);
                break;
            }

            case PLBC_LOAD_FSTATE: {
                uint8_t i = code[pc++];
                if (i >= prog->n_frame_state) goto trap;
                PUSH(rt->frame_state[i]);
                break;
            }
            case PLBC_STORE_FSTATE: {
                uint8_t i = code[pc++];
                if (i >= prog->n_frame_state) goto trap;
                NEED(1);
                rt->frame_state[i] = stack[--sp];
                break;
            }
            case PLBC_LOAD_PSTATE: {
                uint8_t i = code[pc++];
                if (!pstate || i >= prog->n_pixel_state) goto trap;
                PUSH(pstate[i]);
                break;
            }
            case PLBC_STORE_PSTATE: {
                uint8_t i = code[pc++];
                if (!pstate || i >= prog->n_pixel_state) goto trap;
                NEED(1);
                pstate[i] = stack[--sp];
                break;
            }

            case PLBC_ADD: { NEED(2); sp--; stack[sp - 1] = stack[sp - 1] + stack[sp]; break; }
            case PLBC_SUB: { NEED(2); sp--; stack[sp - 1] = stack[sp - 1] - stack[sp]; break; }
            case PLBC_MUL: { NEED(2); sp--; stack[sp - 1] = stack[sp - 1] * stack[sp]; break; }
            case PLBC_DIV: {
                NEED(2); sp--;
                float b = stack[sp];
                stack[sp - 1] = (b == 0.0f) ? 0.0f : (stack[sp - 1] / b);
                break;
            }
            case PLBC_MOD: {
                NEED(2); sp--;
                float b = stack[sp];
                stack[sp - 1] = (b == 0.0f) ? 0.0f : fmodf(stack[sp - 1], b);
                break;
            }
            case PLBC_NEG:   NEED(1); stack[sp - 1] = -stack[sp - 1]; break;
            case PLBC_ABS:   NEED(1); stack[sp - 1] = fabsf(stack[sp - 1]); break;
            case PLBC_FLOOR: NEED(1); stack[sp - 1] = floorf(stack[sp - 1]); break;
            case PLBC_CEIL:  NEED(1); stack[sp - 1] = ceilf(stack[sp - 1]); break;
            case PLBC_ROUND: NEED(1); stack[sp - 1] = roundf(stack[sp - 1]); break;
            case PLBC_MIN: {
                NEED(2); float b = stack[--sp];
                if (b < stack[sp - 1]) stack[sp - 1] = b;
                break;
            }
            case PLBC_MAX: {
                NEED(2); float b = stack[--sp];
                if (b > stack[sp - 1]) stack[sp - 1] = b;
                break;
            }
            case PLBC_SQRT: NEED(1); stack[sp - 1] = sqrtf(stack[sp - 1]); break;
            case PLBC_POW: {
                NEED(2); float b = stack[--sp];
                stack[sp - 1] = powf(stack[sp - 1], b);
                break;
            }
            case PLBC_CLAMP01: {
                NEED(1);
                float v = stack[sp - 1];
                if (v < 0.0f) stack[sp - 1] = 0.0f;
                else if (v > 1.0f) stack[sp - 1] = 1.0f;
                break;
            }

            case PLBC_SIN_LUT: NEED(1); stack[sp - 1] = sin_lut_lookup(stack[sp - 1]); break;
            case PLBC_COS_LUT: NEED(1); stack[sp - 1] = sin_lut_lookup(stack[sp - 1] + (float)M_PI_2); break;

            case PLBC_HASH_I:  NEED(1); stack[sp - 1] = hash01(stack[sp - 1]); break;
            case PLBC_RANDOM: {
                uint32_t r = esp_random();
                PUSH((float)r * (1.0f / 4294967296.0f));
                break;
            }

            case PLBC_LT: { NEED(2); float b = stack[--sp]; stack[sp - 1] = stack[sp - 1] <  b ? 1.0f : 0.0f; break; }
            case PLBC_LE: { NEED(2); float b = stack[--sp]; stack[sp - 1] = stack[sp - 1] <= b ? 1.0f : 0.0f; break; }
            case PLBC_GT: { NEED(2); float b = stack[--sp]; stack[sp - 1] = stack[sp - 1] >  b ? 1.0f : 0.0f; break; }
            case PLBC_GE: { NEED(2); float b = stack[--sp]; stack[sp - 1] = stack[sp - 1] >= b ? 1.0f : 0.0f; break; }
            case PLBC_EQ: { NEED(2); float b = stack[--sp]; stack[sp - 1] = stack[sp - 1] == b ? 1.0f : 0.0f; break; }
            case PLBC_NE: { NEED(2); float b = stack[--sp]; stack[sp - 1] = stack[sp - 1] != b ? 1.0f : 0.0f; break; }
            case PLBC_NOT: NEED(1); stack[sp - 1] = stack[sp - 1] == 0.0f ? 1.0f : 0.0f; break;
            case PLBC_AND: { NEED(2); float b = stack[--sp]; stack[sp - 1] = (stack[sp - 1] != 0.0f && b != 0.0f) ? 1.0f : 0.0f; break; }
            case PLBC_OR:  { NEED(2); float b = stack[--sp]; stack[sp - 1] = (stack[sp - 1] != 0.0f || b != 0.0f) ? 1.0f : 0.0f; break; }

            case PLBC_JMP: {
                int16_t off = read_i16(code, &pc);
                pc = (uint16_t)((int)pc + off);
                if (pc > code_size) goto trap;
                break;
            }
            case PLBC_JMP_IF_FALSE: {
                int16_t off = read_i16(code, &pc);
                NEED(1);
                float v = stack[--sp];
                if (v == 0.0f) {
                    pc = (uint16_t)((int)pc + off);
                    if (pc > code_size) goto trap;
                }
                break;
            }
            case PLBC_JMP_IF_TRUE: {
                int16_t off = read_i16(code, &pc);
                NEED(1);
                float v = stack[--sp];
                if (v != 0.0f) {
                    pc = (uint16_t)((int)pc + off);
                    if (pc > code_size) goto trap;
                }
                break;
            }

            case PLBC_EMIT_RGB: {
                NEED(3);
                float b = stack[--sp];
                float g = stack[--sp];
                float r = stack[--sp];
                int ir = (int)r, ig = (int)g, ib = (int)b;
                if (ir < 0) ir = 0; else if (ir > 255) ir = 255;
                if (ig < 0) ig = 0; else if (ig > 255) ig = 255;
                if (ib < 0) ib = 0; else if (ib > 255) ib = 255;
                if (out_r) *out_r = (uint8_t)ir;
                if (out_g) *out_g = (uint8_t)ig;
                if (out_b) *out_b = (uint8_t)ib;
                emitted = true;
                break;
            }
            case PLBC_EMIT_BRIGHT: {
                NEED(1);
                float bright = stack[--sp];
                if (bright < 0.0f) bright = 0.0f;
                else if (bright > 1.0f) bright = 1.0f;
                if (out_r) *out_r = (uint8_t)((float)rt->base_r * bright);
                if (out_g) *out_g = (uint8_t)((float)rt->base_g * bright);
                if (out_b) *out_b = (uint8_t)((float)rt->base_b * bright);
                emitted = true;
                break;
            }

            case PLBC_HALT: pc = code_size; break;

            default: goto trap;
        }
    }

    (void)emitted;
    return ESP_OK;

trap:
    return ESP_FAIL;
#undef PUSH
#undef NEED
}
