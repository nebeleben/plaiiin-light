#include "js_params.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// Mirror js_storage's BASE_PATH. Keep them in sync if you ever rename it.
static const char *BASE_PATH = "/storage";
#define SIDECAR_MAX_BYTES 1024

// ---------------------------------------------------------------------------
// Magic-comment parsing
// ---------------------------------------------------------------------------

static const char *skip_ws(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

/// Copy a single token (run of non-whitespace) into `dst` (NUL-terminated).
/// Returns pointer to the first char *after* the token, or NULL on overflow.
static const char *take_token(const char *s, char *dst, size_t cap) {
    s = skip_ws(s);
    size_t i = 0;
    while (*s && *s != ' ' && *s != '\t' && *s != '\n' && *s != '\r') {
        if (i + 1 >= cap) return NULL;
        dst[i++] = *s++;
    }
    dst[i] = '\0';
    return s;
}

/// Parse a single `// @param NAME MIN..MAX = DEFAULT DESCRIPTION` line.
/// `line` points at the first char after `// @param`. Returns true on
/// success, false on any malformed component (line is then skipped).
static bool parse_one(const char *line, js_param_t *out) {
    char name[JS_PARAM_NAME_MAX];
    char range[64];
    const char *p = take_token(line, name, sizeof(name));
    if (!p || !name[0]) return false;
    p = take_token(p, range, sizeof(range));
    if (!p) return false;

    // range = "MIN..MAX"
    char *dotdot = strstr(range, "..");
    if (!dotdot) return false;
    *dotdot = '\0';
    char *endp;
    float lo = strtof(range, &endp); if (endp == range) return false;
    float hi = strtof(dotdot + 2, &endp); if (endp == dotdot + 2) return false;
    if (!(hi > lo)) return false;

    p = skip_ws(p);
    if (*p != '=') return false;
    p = skip_ws(p + 1);
    float def = strtof(p, &endp);
    if (endp == p) return false;
    p = skip_ws(endp);

    // Description = everything up to end-of-line. Earlier we ran strlen(p)
    // which captured the rest of the source (multi-line bleed).
    const char *eol = p;
    while (*eol && *eol != '\n' && *eol != '\r') eol++;
    size_t dlen = (size_t)(eol - p);
    while (dlen && (p[dlen-1] == ' ' || p[dlen-1] == '\t')) dlen--;
    size_t copy = dlen >= JS_PARAM_DESC_MAX ? JS_PARAM_DESC_MAX - 1 : dlen;
    memcpy(out->desc, p, copy);
    out->desc[copy] = '\0';

    snprintf(out->name, sizeof(out->name), "%s", name);
    out->min   = lo;
    out->max   = hi;
    out->def   = def < lo ? lo : (def > hi ? hi : def);
    out->value = out->def;
    return true;
}

int js_params_parse(const char *source, js_params_schema_t *out) {
    if (!source || !out) return 0;
    out->count = 0;
    static const char *MARKER = "// @param";
    const size_t mlen = strlen(MARKER);

    const char *p = source;
    while (*p && out->count < JS_PARAMS_MAX) {
        // Match `// @param` at start of line (allow leading whitespace).
        const char *line_start = p;
        const char *q = skip_ws(p);
        if (strncmp(q, MARKER, mlen) == 0) {
            const char *body = q + mlen;
            if (parse_one(body, &out->items[out->count])) {
                out->count++;
            }
        }
        // Advance to the next line.
        const char *nl = strchr(line_start, '\n');
        if (!nl) break;
        p = nl + 1;
    }
    return out->count;
}

// ---------------------------------------------------------------------------
// Sidecar persistence
// ---------------------------------------------------------------------------

static void sidecar_path(const char *name, char *out, size_t max_len) {
    snprintf(out, max_len, "%s/%s.params.json", BASE_PATH, name);
}

esp_err_t js_params_load_overrides(const char *name, js_params_schema_t *schema) {
    if (!name || !schema) return ESP_ERR_INVALID_ARG;
    if (schema->count == 0) return ESP_OK;
    char path[128];
    sidecar_path(name, path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return errno == ENOENT ? ESP_OK : ESP_FAIL;
    char buf[SIDECAR_MAX_BYTES];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    js_params_apply_json(schema, buf, n);
    return ESP_OK;
}

esp_err_t js_params_save_overrides(const char *name, const js_params_schema_t *schema) {
    if (!name || !schema) return ESP_ERR_INVALID_ARG;
    char path[128];
    sidecar_path(name, path, sizeof(path));
    // Build minimal JSON of overrides only (skip params at default).
    char buf[SIDECAR_MAX_BYTES];
    int off = 0;
    off += snprintf(buf + off, sizeof(buf) - off, "{");
    int written = 0;
    for (int i = 0; i < schema->count && off < (int)sizeof(buf) - 32; i++) {
        const js_param_t *p = &schema->items[i];
        if (p->value == p->def) continue;
        off += snprintf(buf + off, sizeof(buf) - off, "%s\"%s\":%g",
                        written ? "," : "", p->name, (double)p->value);
        written++;
    }
    off += snprintf(buf + off, sizeof(buf) - off, "}");
    if (written == 0) {
        // No overrides — leave no sidecar (unlink any old one).
        unlink(path);
        return ESP_OK;
    }
    FILE *f = fopen(path, "w");
    if (!f) return ESP_FAIL;
    size_t w = fwrite(buf, 1, off, f);
    fclose(f);
    return w == (size_t)off ? ESP_OK : ESP_FAIL;
}

void js_params_drop(const char *name) {
    if (!name) return;
    char path[128];
    sidecar_path(name, path, sizeof(path));
    unlink(path);
}

// ---------------------------------------------------------------------------
// JSON merge — used by both load_overrides and PUT handler. Tiny hand-rolled
// scanner to avoid pulling in cJSON for one shape.
// ---------------------------------------------------------------------------

// Find `"key"`: return pointer to first char of the value (whitespace
// skipped), or NULL if not found.
static const char *find_value(const char *json, const char *key) {
    char needle[JS_PARAM_NAME_MAX + 4];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return NULL;
    p += strlen(needle);
    while (*p == ' ' || *p == '\t' || *p == ':' || *p == '\n' || *p == '\r') p++;
    return p;
}

int js_params_apply_json(js_params_schema_t *schema, const char *json, size_t len) {
    (void)len;
    if (!schema || !json) return 0;
    int updated = 0;
    for (int i = 0; i < schema->count; i++) {
        js_param_t *p = &schema->items[i];
        const char *v = find_value(json, p->name);
        if (!v) continue;
        char *endp;
        float val = strtof(v, &endp);
        if (endp == v) continue;
        if (val < p->min) val = p->min;
        if (val > p->max) val = p->max;
        if (val != p->value) {
            p->value = val;
            updated++;
        }
    }
    return updated;
}

// ---------------------------------------------------------------------------
// Output: schema → JSON for clients
// ---------------------------------------------------------------------------

int js_params_to_json(const js_params_schema_t *schema, char *out, size_t max_len) {
    if (!schema || !out) return -1;
    int off = 0;
    off += snprintf(out + off, max_len - off, "{\"items\":[");
    for (int i = 0; i < schema->count && off < (int)max_len - 64; i++) {
        const js_param_t *p = &schema->items[i];
        // Escape only `"` and `\` in the description — that's enough for
        // human-typed text. Reject anything funkier silently.
        char desc[JS_PARAM_DESC_MAX * 2];
        size_t d = 0;
        for (size_t j = 0; p->desc[j] && d + 2 < sizeof(desc); j++) {
            char c = p->desc[j];
            if (c == '"' || c == '\\') desc[d++] = '\\';
            desc[d++] = c;
        }
        desc[d] = '\0';
        off += snprintf(out + off, max_len - off,
                        "%s{\"name\":\"%s\",\"min\":%g,\"max\":%g,\"default\":%g,"
                        "\"value\":%g,\"description\":\"%s\"}",
                        i ? "," : "",
                        p->name, (double)p->min, (double)p->max,
                        (double)p->def, (double)p->value, desc);
    }
    off += snprintf(out + off, max_len - off, "]}");
    return off;
}

// ---------------------------------------------------------------------------
// mjs object construction
// ---------------------------------------------------------------------------

mjs_val_t js_params_to_mjs(struct mjs *mjs, const js_params_schema_t *schema) {
    mjs_val_t obj = mjs_mk_object(mjs);
    if (!schema) return obj;
    for (int i = 0; i < schema->count; i++) {
        const js_param_t *p = &schema->items[i];
        mjs_set(mjs, obj, p->name, ~0, mjs_mk_number(mjs, p->value));
    }
    return obj;
}
