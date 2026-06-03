/* .bc file format (little-endian throughout):
 *
 *   offset  size  field
 *   0       4     magic 'PLBC'
 *   4       1     version
 *   5       1     flags (reserved)
 *   6       1     mode (v2: int8 — 0xFF none / 0 strip / 1 mirror; v1: reserved)
 *   7       1     mode_switch (v2: 1 = user may change mode; 0 / old v2 = locked)
 *   8       1     n_params
 *   9       1     n_frame_state
 *   10      1     n_pixel_state
 *   11      1     n_locals
 *   12      2     code_size
 *   14      code_size  bytecode
 *   ...     param schema:   for each: name_len(u8) + name + min(f32) + max(f32) + def(f32) + cur(f32) + desc_len(u8) + desc + type(u8, v2+)
 *   ...     frame state defs: for each: name_len(u8) + name + def(f32)
 *   ...     pixel state defs: for each: name_len(u8) + name + def(f32)
 *
 * Param `cur` lets us round-trip persisted overrides without touching the
 * source. The compiler initialises it to `def`; /api/js/<name>/params PUT
 * mutates it in-memory and re-serializes.
 */

#include "plbc.h"

#include <string.h>
#include <stdio.h>

#define WB(buf, off, max, val, n) do { \
    if ((off) + (n) > (max)) return -1; \
    memcpy((buf) + (off), (val), (n)); \
    (off) += (n); \
} while (0)

#define WU8(buf, off, max, v) do { \
    if ((off) + 1 > (max)) return -1; \
    (buf)[(off)++] = (uint8_t)(v); \
} while (0)

#define WU16(buf, off, max, v) do { \
    if ((off) + 2 > (max)) return -1; \
    (buf)[(off)++] = (uint8_t)((v) & 0xFF); \
    (buf)[(off)++] = (uint8_t)(((v) >> 8) & 0xFF); \
} while (0)

#define WF32(buf, off, max, v) do { \
    if ((off) + 4 > (max)) return -1; \
    union { float f; uint32_t u; } b; b.f = (v); \
    (buf)[(off)++] = (uint8_t)(b.u & 0xFF); \
    (buf)[(off)++] = (uint8_t)((b.u >> 8) & 0xFF); \
    (buf)[(off)++] = (uint8_t)((b.u >> 16) & 0xFF); \
    (buf)[(off)++] = (uint8_t)((b.u >> 24) & 0xFF); \
} while (0)

#define WSTR(buf, off, max, s, max_str) do { \
    size_t n = strlen(s); \
    if (n > (max_str)) n = (max_str); \
    WU8(buf, off, max, (uint8_t)n); \
    WB(buf, off, max, s, n); \
} while (0)

int plbc_serialize(const plbc_program_t *prog, uint8_t *buf, size_t buf_size)
{
    if (!prog || !buf) return -1;
    size_t off = 0;

    /* Header */
    WB(buf, off, buf_size, PLBC_MAGIC, 4);
    WU8(buf, off, buf_size, PLBC_VERSION);
    WU8(buf, off, buf_size, 0);  /* flags */
    WU8(buf, off, buf_size, (uint8_t)(int8_t)prog->mode);  /* v2: declared mode (0xFF=none) */
    WU8(buf, off, buf_size, prog->mode_switch);  /* v2 byte-7: mode-switchable flag (was reserved-0) */
    WU8(buf, off, buf_size, prog->n_params);
    WU8(buf, off, buf_size, prog->n_frame_state);
    WU8(buf, off, buf_size, prog->n_pixel_state);
    WU8(buf, off, buf_size, prog->n_locals);
    WU16(buf, off, buf_size, prog->code_size);

    /* Code */
    WB(buf, off, buf_size, prog->code, prog->code_size);

    /* Param schema */
    for (int i = 0; i < prog->n_params; i++) {
        const plbc_param_t *p = &prog->params[i];
        WSTR(buf, off, buf_size, p->name, PLBC_MAX_NAME - 1);
        WF32(buf, off, buf_size, p->min);
        WF32(buf, off, buf_size, p->max);
        WF32(buf, off, buf_size, p->def);
        WF32(buf, off, buf_size, p->value);
        WSTR(buf, off, buf_size, p->desc, PLBC_MAX_DESC - 1);
        WU8(buf, off, buf_size, p->type);  /* v2 */
    }

    /* Frame state defs */
    for (int i = 0; i < prog->n_frame_state; i++) {
        WSTR(buf, off, buf_size, prog->frame_state_defs[i].name, PLBC_MAX_NAME - 1);
        WF32(buf, off, buf_size, prog->frame_state_defs[i].def);
    }

    /* Pixel state defs */
    for (int i = 0; i < prog->n_pixel_state; i++) {
        WSTR(buf, off, buf_size, prog->pixel_state_defs[i].name, PLBC_MAX_NAME - 1);
        WF32(buf, off, buf_size, prog->pixel_state_defs[i].def);
    }

    return (int)off;
}

/* Read counterparts. Each returns the new offset or -1 on overflow. */
static int read_bytes(const uint8_t *buf, size_t buf_len, size_t off, void *dst, size_t n)
{
    if (off + n > buf_len) return -1;
    memcpy(dst, buf + off, n);
    return (int)(off + n);
}

static int read_u8(const uint8_t *buf, size_t buf_len, size_t off, uint8_t *out)
{
    if (off + 1 > buf_len) return -1;
    *out = buf[off];
    return (int)(off + 1);
}

static int read_u16(const uint8_t *buf, size_t buf_len, size_t off, uint16_t *out)
{
    if (off + 2 > buf_len) return -1;
    *out = (uint16_t)(buf[off] | (buf[off + 1] << 8));
    return (int)(off + 2);
}

static int read_f32(const uint8_t *buf, size_t buf_len, size_t off, float *out)
{
    if (off + 4 > buf_len) return -1;
    union { uint32_t u; float f; } b;
    b.u = (uint32_t)buf[off]
        | ((uint32_t)buf[off + 1] << 8)
        | ((uint32_t)buf[off + 2] << 16)
        | ((uint32_t)buf[off + 3] << 24);
    *out = b.f;
    return (int)(off + 4);
}

static int read_str(const uint8_t *buf, size_t buf_len, size_t off,
                    char *out, size_t out_size)
{
    uint8_t n;
    int o = read_u8(buf, buf_len, off, &n);
    if (o < 0) return -1;
    size_t take = (size_t)n < out_size - 1 ? (size_t)n : out_size - 1;
    if ((size_t)o + n > buf_len) return -1;
    memcpy(out, buf + o, take);
    out[take] = 0;
    return o + n;
}

esp_err_t plbc_load(const uint8_t *buf, size_t buf_len,
                    plbc_program_t *out_prog,
                    char *err_buf, size_t err_buf_size)
{
    if (!buf || !out_prog) return ESP_ERR_INVALID_ARG;
    memset(out_prog, 0, sizeof(*out_prog));
    if (buf_len < 14) {
        if (err_buf) snprintf(err_buf, err_buf_size, "bytecode truncated");
        return ESP_FAIL;
    }
    if (memcmp(buf, PLBC_MAGIC, 4) != 0) {
        if (err_buf) snprintf(err_buf, err_buf_size, "bad magic");
        return ESP_FAIL;
    }
    int off = 4;
    uint8_t version, flags, mode_b, modesw_b, np, nf, ns, nl;
    uint16_t code_size;
    off = read_u8(buf, buf_len, off, &version); if (off < 0) goto truncated;
    if (version != PLBC_VERSION) {
        if (err_buf) snprintf(err_buf, err_buf_size, "version %u, expected %u", version, PLBC_VERSION);
        return ESP_FAIL;
    }
    off = read_u8(buf, buf_len, off, &flags); if (off < 0) goto truncated;
    off = read_u8(buf, buf_len, off, &mode_b); if (off < 0) goto truncated;
    off = read_u8(buf, buf_len, off, &modesw_b); if (off < 0) goto truncated;
    off = read_u8(buf, buf_len, off, &np); if (off < 0) goto truncated;
    off = read_u8(buf, buf_len, off, &nf); if (off < 0) goto truncated;
    off = read_u8(buf, buf_len, off, &ns); if (off < 0) goto truncated;
    off = read_u8(buf, buf_len, off, &nl); if (off < 0) goto truncated;
    off = read_u16(buf, buf_len, off, &code_size); if (off < 0) goto truncated;

    if (np > PLBC_MAX_PARAMS || nf > PLBC_MAX_FRAME_STATE
        || ns > PLBC_MAX_PIXEL_STATE || nl > PLBC_MAX_LOCALS
        || code_size > PLBC_MAX_CODE) {
        if (err_buf) snprintf(err_buf, err_buf_size, "schema exceeds limits");
        return ESP_FAIL;
    }
    out_prog->n_params = np;
    out_prog->n_frame_state = nf;
    out_prog->n_pixel_state = ns;
    out_prog->n_locals = nl;
    out_prog->code_size = code_size;
    out_prog->mode = (int8_t)mode_b;  /* 0xFF -> -1 (none) */
    out_prog->mode_switch = modesw_b ? 1 : 0;

    off = read_bytes(buf, buf_len, off, out_prog->code, code_size); if (off < 0) goto truncated;

    for (int i = 0; i < np; i++) {
        plbc_param_t *p = &out_prog->params[i];
        off = read_str(buf, buf_len, off, p->name, sizeof(p->name)); if (off < 0) goto truncated;
        off = read_f32(buf, buf_len, off, &p->min);                 if (off < 0) goto truncated;
        off = read_f32(buf, buf_len, off, &p->max);                 if (off < 0) goto truncated;
        off = read_f32(buf, buf_len, off, &p->def);                 if (off < 0) goto truncated;
        off = read_f32(buf, buf_len, off, &p->value);               if (off < 0) goto truncated;
        off = read_str(buf, buf_len, off, p->desc, sizeof(p->desc)); if (off < 0) goto truncated;
        off = read_u8(buf, buf_len, off, &p->type);                 if (off < 0) goto truncated;
    }
    for (int i = 0; i < nf; i++) {
        off = read_str(buf, buf_len, off, out_prog->frame_state_defs[i].name,
                       sizeof(out_prog->frame_state_defs[i].name));
        if (off < 0) goto truncated;
        off = read_f32(buf, buf_len, off, &out_prog->frame_state_defs[i].def);
        if (off < 0) goto truncated;
    }
    for (int i = 0; i < ns; i++) {
        off = read_str(buf, buf_len, off, out_prog->pixel_state_defs[i].name,
                       sizeof(out_prog->pixel_state_defs[i].name));
        if (off < 0) goto truncated;
        off = read_f32(buf, buf_len, off, &out_prog->pixel_state_defs[i].def);
        if (off < 0) goto truncated;
    }

    (void)flags;
    return ESP_OK;

truncated:
    if (err_buf) snprintf(err_buf, err_buf_size, "bytecode truncated");
    return ESP_FAIL;
}

/* Params <-> JSON for the existing /api/js/<name>/params surface. We keep
 * the same field names the old js_params code emitted so the macOS knob UI
 * works without changes. */
int plbc_params_to_json(const plbc_program_t *prog, char *out, size_t max)
{
    return plbc_params_to_json_ex(prog, out, max, true);
}

int plbc_params_to_json_ex(const plbc_program_t *prog, char *out, size_t max,
                           bool include_desc)
{
    if (!prog || !out || max < 16) return 0;
    int off = snprintf(out, max, "{\"items\":[");
    for (int i = 0; i < prog->n_params; i++) {
        const plbc_param_t *p = &prog->params[i];
        const char *sep = (i == 0) ? "" : ",";
        /* `type` is additive — older clients ignore it and render a slider.
         * `description` is omitted over BLE to stay under the GATT read cap. */
        int n;
        if (include_desc) {
            n = snprintf(out + off, max - off,
                "%s{\"name\":\"%s\",\"min\":%g,\"max\":%g,\"default\":%g,\"value\":%g,\"description\":\"%s\",\"type\":\"%s\"}",
                sep, p->name, p->min, p->max, p->def, p->value, p->desc,
                p->type == PLBC_PARAM_SWITCH ? "switch" : "range");
        } else {
            /* Keep an empty "description" so clients whose model has a
             * non-optional description field still decode — just shed the
             * (long) text that blows past the BLE read cap. */
            n = snprintf(out + off, max - off,
                "%s{\"name\":\"%s\",\"min\":%g,\"max\":%g,\"default\":%g,\"value\":%g,\"description\":\"\",\"type\":\"%s\"}",
                sep, p->name, p->min, p->max, p->def, p->value,
                p->type == PLBC_PARAM_SWITCH ? "switch" : "range");
        }
        if (n < 0 || (size_t)(off + n) >= max) return 0;
        off += n;
    }
    /* Top-level `mode` carries the effect's declared wormhole render mode so a
     * client can seed the toggle; `modeSwitch` says whether the user may change
     * it (false = locked to `mode`, so clients hide the toggle). Both absent of
     * meaning when mode is null (a form-agnostic effect). */
    int n;
    if (prog->mode == 0 || prog->mode == 1) {
        n = snprintf(out + off, max - off, "],\"mode\":\"%s\",\"modeSwitch\":%s}",
                     prog->mode == 1 ? "mirror" : "strip",
                     prog->mode_switch ? "true" : "false");
    } else {
        n = snprintf(out + off, max - off, "],\"mode\":null,\"modeSwitch\":false}");
    }
    if (n < 0 || (size_t)(off + n) >= max) return 0;
    off += n;
    return off;
}

/* Tiny "find a key:value number in the JSON body" — same brittle parser as
 * the old js_params code, intentionally. Good enough for the small payloads
 * the UI sends; if we ever need real JSON, swap to cJSON. */
static const char *find_key(const char *json, size_t len, const char *key)
{
    size_t key_len = strlen(key);
    for (size_t i = 0; i + key_len + 2 < len; i++) {
        if (json[i] == '"' && memcmp(json + i + 1, key, key_len) == 0
            && json[i + 1 + key_len] == '"') {
            return json + i + 1 + key_len + 1;
        }
    }
    return NULL;
}

int plbc_apply_params_json(plbc_program_t *prog, const char *json, size_t len)
{
    if (!prog || !json) return 0;
    int updated = 0;
    for (int i = 0; i < prog->n_params; i++) {
        plbc_param_t *p = &prog->params[i];
        const char *colon = find_key(json, len, p->name);
        if (!colon) continue;
        while (*colon && *colon != ':' && (colon - json) < (long)len) colon++;
        if (*colon != ':') continue;
        colon++;
        while (*colon == ' ' && (colon - json) < (long)len) colon++;
        char buf[32] = {0};
        size_t bi = 0;
        while ((colon - json) < (long)len && bi < sizeof(buf) - 1
               && (*colon == '-' || *colon == '.' || *colon == 'e' || *colon == 'E'
                   || *colon == '+' || (*colon >= '0' && *colon <= '9'))) {
            buf[bi++] = *colon++;
        }
        if (bi == 0) continue;
        float v = (float)atof(buf);
        if (v < p->min) v = p->min;
        if (v > p->max) v = p->max;
        if (p->value != v) { p->value = v; updated++; }
    }
    return updated;
}
