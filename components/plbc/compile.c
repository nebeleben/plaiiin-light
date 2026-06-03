/* PLBC compiler — JS-subset → bytecode.
 *
 * Single-pass recursive descent: tokens stream straight into bytecode
 * emission, no AST. Symbol tables for params (indexed by @param decl
 * order), frame state, pixel state, and function-scoped locals (declared
 * via `let`). Branches use placeholder offsets fixed up at end-of-block.
 *
 * Grammar (rough):
 *   program        = annotation_or_comment* function_decl
 *   annotation     = '//' '@param' NAME MIN '..' MAX '=' DEF [DESC] EOL
 *                  | '//' '@state' NAME ':' NUM EOL
 *                  | '//' '@state.pixel' NAME ':' NUM EOL
 *   function_decl  = 'function' 'shade' '(' params ')' '{' stmt* '}'
 *   stmt           = let_stmt | assign_stmt | if_stmt | for_stmt | expr_stmt
 *   let_stmt       = 'let' NAME '=' expr ';'
 *   assign_stmt    = lvalue ('=' | '+=' | '-=' | '*=' | '/=') expr ';'
 *   if_stmt        = 'if' '(' expr ')' '{' stmt* '}' ('else' (if_stmt | '{' stmt* '}'))?
 *   for_stmt       = 'for' '(' 'let' NAME '=' expr ';' expr ';' assign_stmt ')' '{' stmt* '}'
 *   expr           = or_expr
 *   or_expr        = and_expr ('||' and_expr)*
 *   and_expr       = cmp_expr ('&&' cmp_expr)*
 *   cmp_expr       = sum (('<'|'<='|'>'|'>='|'=='|'!=') sum)*
 *   sum            = product (('+'|'-') product)*
 *   product        = unary (('*'|'/'|'%') unary)*
 *   unary          = ('-'|'!')? primary
 *   primary        = NUM | NAME [postfix] | NAME '(' args ')' | '(' expr ')'
 *   postfix        = '.' NAME             (params.X, base.r/g/b, name.pixel)
 *
 * Reserved input names inside function body: x, y, idx, frame, w, h, base.
 * Reserved built-in calls: sinLUT, cosLUT, floor, ceil, round, abs, sqrt,
 *   pow, min, max, clamp01, hash, random, emit, emitBright.
 */

#include "plbc.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- Symbol tables --------------------------------------------- */

typedef struct {
    char name[PLBC_MAX_NAME];
} local_t;

typedef struct {
    const char *src;
    size_t src_len;
    size_t pos;

    /* Last-token cache for peek/consume. */
    int tok_kind;
    double tok_num;
    char tok_str[PLBC_MAX_NAME];
    size_t tok_start;
    size_t tok_line;

    /* Emit state */
    plbc_program_t *prog;
    uint16_t cur_pc;

    /* Symbols */
    local_t locals[PLBC_MAX_LOCALS];
    int n_locals;

    /* Error reporting */
    char *err_buf;
    size_t err_buf_size;
    bool has_err;
} ctx_t;

enum {
    TOK_EOF = 0,
    TOK_NUM,
    TOK_IDENT,
    TOK_LPAREN, TOK_RPAREN,
    TOK_LBRACE, TOK_RBRACE,
    TOK_LBRACK, TOK_RBRACK,
    TOK_COMMA, TOK_SEMI, TOK_DOT,
    TOK_ASSIGN, TOK_PLUS_ASSIGN, TOK_MINUS_ASSIGN, TOK_MUL_ASSIGN, TOK_DIV_ASSIGN,
    TOK_PLUS, TOK_MINUS, TOK_STAR, TOK_SLASH, TOK_PERCENT,
    TOK_LT, TOK_LE, TOK_GT, TOK_GE, TOK_EQ, TOK_NE,
    TOK_AND, TOK_OR, TOK_NOT,
    TOK_PLUSPLUS,
    /* Keywords */
    TOK_LET, TOK_IF, TOK_ELSE, TOK_FOR, TOK_FUNCTION,
};

static void verr(ctx_t *c, const char *fmt, ...)
{
    if (c->has_err) return;
    c->has_err = true;
    if (!c->err_buf || c->err_buf_size == 0) return;
    int n = snprintf(c->err_buf, c->err_buf_size, "line %zu: ", c->tok_line);
    if (n < 0 || (size_t)n >= c->err_buf_size) return;
    va_list ap;
    __builtin_va_start(ap, fmt);
    vsnprintf(c->err_buf + n, c->err_buf_size - n, fmt, ap);
    __builtin_va_end(ap);
}

/* ---------- Lexer ----------------------------------------------------- */

static bool is_ident_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static bool is_ident_cont(char c)  { return is_ident_start(c) || (c >= '0' && c <= '9'); }

static void skip_ws_and_comments(ctx_t *c)
{
    while (c->pos < c->src_len) {
        char ch = c->src[c->pos];
        if (ch == '\n') { c->tok_line++; c->pos++; continue; }
        if (ch == ' ' || ch == '\t' || ch == '\r') { c->pos++; continue; }
        if (ch == '/' && c->pos + 1 < c->src_len && c->src[c->pos + 1] == '/') {
            /* Line comment — annotations are handled by a separate prepass. */
            while (c->pos < c->src_len && c->src[c->pos] != '\n') c->pos++;
            continue;
        }
        if (ch == '/' && c->pos + 1 < c->src_len && c->src[c->pos + 1] == '*') {
            c->pos += 2;
            while (c->pos + 1 < c->src_len &&
                   !(c->src[c->pos] == '*' && c->src[c->pos + 1] == '/')) {
                if (c->src[c->pos] == '\n') c->tok_line++;
                c->pos++;
            }
            if (c->pos + 1 < c->src_len) c->pos += 2;
            continue;
        }
        break;
    }
}

/* Match a fixed keyword. Returns 0 if not a keyword, else token kind. */
static int kw_lookup(const char *s, size_t len)
{
    if (len == 3 && memcmp(s, "let", 3) == 0) return TOK_LET;
    if (len == 2 && memcmp(s, "if", 2) == 0) return TOK_IF;
    if (len == 4 && memcmp(s, "else", 4) == 0) return TOK_ELSE;
    if (len == 3 && memcmp(s, "for", 3) == 0) return TOK_FOR;
    if (len == 8 && memcmp(s, "function", 8) == 0) return TOK_FUNCTION;
    return 0;
}

static void next_token(ctx_t *c)
{
    skip_ws_and_comments(c);
    c->tok_start = c->pos;
    if (c->pos >= c->src_len) { c->tok_kind = TOK_EOF; return; }
    char ch = c->src[c->pos];

    /* Number — must start with a digit. A bare leading `.` is the
     * member-access operator, not a number; the punctuation branch below
     * handles it. (Earlier we accepted `.5` here, but a bare `.` followed
     * by a non-digit (`base.r`) sent the loop into a zero-advance state
     * and returned TOK_NUM with the lex position unchanged.) */
    if (ch >= '0' && ch <= '9') {
        char buf[40] = {0};
        size_t bi = 0;
        bool had_dot = false;
        bool had_e = false;
        while (c->pos < c->src_len && bi < sizeof(buf) - 1) {
            char d = c->src[c->pos];
            if (d >= '0' && d <= '9') { buf[bi++] = d; c->pos++; }
            else if (d == '.' && !had_dot && !had_e) { had_dot = true; buf[bi++] = d; c->pos++; }
            else if ((d == 'e' || d == 'E') && !had_e) { had_e = true; buf[bi++] = d; c->pos++; }
            else if ((d == '-' || d == '+') && had_e && (buf[bi - 1] == 'e' || buf[bi - 1] == 'E')) { buf[bi++] = d; c->pos++; }
            else break;
        }
        c->tok_num = atof(buf);
        c->tok_kind = TOK_NUM;
        return;
    }

    /* Identifier / keyword */
    if (is_ident_start(ch)) {
        size_t s = c->pos;
        while (c->pos < c->src_len && is_ident_cont(c->src[c->pos])) c->pos++;
        size_t len = c->pos - s;
        int kw = kw_lookup(c->src + s, len);
        if (kw) { c->tok_kind = kw; return; }
        size_t take = len < PLBC_MAX_NAME - 1 ? len : PLBC_MAX_NAME - 1;
        memcpy(c->tok_str, c->src + s, take);
        c->tok_str[take] = 0;
        c->tok_kind = TOK_IDENT;
        return;
    }

    /* Punctuation / operators */
    c->pos++;
    switch (ch) {
        case '(': c->tok_kind = TOK_LPAREN; return;
        case ')': c->tok_kind = TOK_RPAREN; return;
        case '{': c->tok_kind = TOK_LBRACE; return;
        case '}': c->tok_kind = TOK_RBRACE; return;
        case '[': c->tok_kind = TOK_LBRACK; return;
        case ']': c->tok_kind = TOK_RBRACK; return;
        case ',': c->tok_kind = TOK_COMMA; return;
        case ';': c->tok_kind = TOK_SEMI; return;
        case '.': c->tok_kind = TOK_DOT; return;
        case '+':
            if (c->pos < c->src_len && c->src[c->pos] == '+') { c->pos++; c->tok_kind = TOK_PLUSPLUS; return; }
            if (c->pos < c->src_len && c->src[c->pos] == '=') { c->pos++; c->tok_kind = TOK_PLUS_ASSIGN; return; }
            c->tok_kind = TOK_PLUS; return;
        case '-':
            if (c->pos < c->src_len && c->src[c->pos] == '=') { c->pos++; c->tok_kind = TOK_MINUS_ASSIGN; return; }
            c->tok_kind = TOK_MINUS; return;
        case '*':
            if (c->pos < c->src_len && c->src[c->pos] == '=') { c->pos++; c->tok_kind = TOK_MUL_ASSIGN; return; }
            c->tok_kind = TOK_STAR; return;
        case '/':
            if (c->pos < c->src_len && c->src[c->pos] == '=') { c->pos++; c->tok_kind = TOK_DIV_ASSIGN; return; }
            c->tok_kind = TOK_SLASH; return;
        case '%': c->tok_kind = TOK_PERCENT; return;
        case '<':
            if (c->pos < c->src_len && c->src[c->pos] == '=') { c->pos++; c->tok_kind = TOK_LE; return; }
            c->tok_kind = TOK_LT; return;
        case '>':
            if (c->pos < c->src_len && c->src[c->pos] == '=') { c->pos++; c->tok_kind = TOK_GE; return; }
            c->tok_kind = TOK_GT; return;
        case '=':
            if (c->pos < c->src_len && c->src[c->pos] == '=') { c->pos++; c->tok_kind = TOK_EQ; return; }
            c->tok_kind = TOK_ASSIGN; return;
        case '!':
            if (c->pos < c->src_len && c->src[c->pos] == '=') { c->pos++; c->tok_kind = TOK_NE; return; }
            c->tok_kind = TOK_NOT; return;
        case '&':
            if (c->pos < c->src_len && c->src[c->pos] == '&') { c->pos++; c->tok_kind = TOK_AND; return; }
            verr(c, "unexpected '&'"); c->tok_kind = TOK_EOF; return;
        case '|':
            if (c->pos < c->src_len && c->src[c->pos] == '|') { c->pos++; c->tok_kind = TOK_OR; return; }
            verr(c, "unexpected '|'"); c->tok_kind = TOK_EOF; return;
        default:
            verr(c, "unexpected character '%c'", ch);
            c->tok_kind = TOK_EOF; return;
    }
}

static bool accept(ctx_t *c, int kind)
{
    if (c->tok_kind == kind) { next_token(c); return true; }
    return false;
}

static void expect(ctx_t *c, int kind, const char *what)
{
    if (c->tok_kind != kind) { verr(c, "expected %s", what); }
    else next_token(c);
}

/* ---------- Emit primitives ------------------------------------------ */

static void emit_u8(ctx_t *c, uint8_t v)
{
    if (c->cur_pc >= PLBC_MAX_CODE) { verr(c, "bytecode too large"); return; }
    c->prog->code[c->cur_pc++] = v;
}

static void emit_i16_at(ctx_t *c, uint16_t at, int16_t v)
{
    if (at + 2 > PLBC_MAX_CODE) { verr(c, "bytecode too large"); return; }
    c->prog->code[at] = (uint8_t)(v & 0xFF);
    c->prog->code[at + 1] = (uint8_t)((v >> 8) & 0xFF);
}

static uint16_t emit_i16_placeholder(ctx_t *c)
{
    if (c->cur_pc + 2 > PLBC_MAX_CODE) { verr(c, "bytecode too large"); return 0; }
    uint16_t at = c->cur_pc;
    c->cur_pc += 2;
    return at;
}

static void emit_f32(ctx_t *c, float f)
{
    if (c->cur_pc + 4 > PLBC_MAX_CODE) { verr(c, "bytecode too large"); return; }
    union { float f; uint32_t u; } b; b.f = f;
    c->prog->code[c->cur_pc++] = (uint8_t)(b.u & 0xFF);
    c->prog->code[c->cur_pc++] = (uint8_t)((b.u >> 8) & 0xFF);
    c->prog->code[c->cur_pc++] = (uint8_t)((b.u >> 16) & 0xFF);
    c->prog->code[c->cur_pc++] = (uint8_t)((b.u >> 24) & 0xFF);
}

static void emit_push_num(ctx_t *c, double v)
{
    if (v == 0.0)      emit_u8(c, PLBC_PUSH_ZERO);
    else if (v == 1.0) emit_u8(c, PLBC_PUSH_ONE);
    else if (v == floor(v) && v >= -128 && v <= 127) {
        emit_u8(c, PLBC_PUSH_I8);
        emit_u8(c, (uint8_t)(int8_t)v);
    } else {
        emit_u8(c, PLBC_PUSH_F32);
        emit_f32(c, (float)v);
    }
}

/* ---------- Symbol resolution --------------------------------------- */

static int find_local(ctx_t *c, const char *name)
{
    for (int i = 0; i < c->n_locals; i++) {
        if (strcmp(c->locals[i].name, name) == 0) return i;
    }
    return -1;
}

static int add_local(ctx_t *c, const char *name)
{
    if (c->n_locals >= PLBC_MAX_LOCALS) { verr(c, "too many locals"); return -1; }
    strncpy(c->locals[c->n_locals].name, name, PLBC_MAX_NAME - 1);
    c->locals[c->n_locals].name[PLBC_MAX_NAME - 1] = 0;
    return c->n_locals++;
}

static int find_param(plbc_program_t *prog, const char *name)
{
    for (int i = 0; i < prog->n_params; i++) {
        if (strcmp(prog->params[i].name, name) == 0) return i;
    }
    return -1;
}

static int find_fstate(plbc_program_t *prog, const char *name)
{
    for (int i = 0; i < prog->n_frame_state; i++) {
        if (strcmp(prog->frame_state_defs[i].name, name) == 0) return i;
    }
    return -1;
}

static int find_pstate(plbc_program_t *prog, const char *name)
{
    for (int i = 0; i < prog->n_pixel_state; i++) {
        if (strcmp(prog->pixel_state_defs[i].name, name) == 0) return i;
    }
    return -1;
}

/* Look up a name as a "pre-loadable" input. Returns the LOAD_* opcode or 0
 * if not a known input. */
static uint8_t input_opcode(const char *name)
{
    if (strcmp(name, "x") == 0) return PLBC_LOAD_X;
    if (strcmp(name, "y") == 0) return PLBC_LOAD_Y;
    if (strcmp(name, "idx") == 0) return PLBC_LOAD_IDX;
    if (strcmp(name, "w") == 0) return PLBC_LOAD_W;
    if (strcmp(name, "h") == 0) return PLBC_LOAD_H;
    if (strcmp(name, "frame") == 0) return PLBC_LOAD_FRAME;
    /* Phase 23 — per-playback seed (ms-since-boot at start). Different
     * every play so stateless scripts like fade can vary their pattern. */
    if (strcmp(name, "playStart") == 0) return PLBC_LOAD_PLAY_START;
    /* Phase 23 — ms since playback started, advances per frame. The right
     * input to drive animations now that fps is configurable; using `frame
     * * dt` with a hard-coded dt locks animations to whatever fps the
     * script's author assumed. */
    if (strcmp(name, "time") == 0) return PLBC_LOAD_TIME;
    return 0;
}

static uint8_t base_opcode(const char *member)
{
    if (strcmp(member, "r") == 0) return PLBC_LOAD_BASE_R;
    if (strcmp(member, "g") == 0) return PLBC_LOAD_BASE_G;
    if (strcmp(member, "b") == 0) return PLBC_LOAD_BASE_B;
    return 0;
}

/* ---------- Annotation prepass --------------------------------------- */

/* Walk the source line by line, extract @param / @state / @state.pixel into
 * the program's schema arrays. Comments outside these forms are ignored. */
static void parse_annotations(ctx_t *c)
{
    plbc_program_t *prog = c->prog;
    size_t i = 0;
    while (i < c->src_len) {
        /* Skip whitespace */
        while (i < c->src_len && (c->src[i] == ' ' || c->src[i] == '\t' || c->src[i] == '\r')) i++;
        if (i + 2 >= c->src_len || c->src[i] != '/' || c->src[i + 1] != '/') {
            /* Not a comment — advance to next line */
            while (i < c->src_len && c->src[i] != '\n') i++;
            if (i < c->src_len) i++;
            continue;
        }
        /* Consume // */
        i += 2;
        while (i < c->src_len && (c->src[i] == ' ' || c->src[i] == '\t')) i++;

        const char *line = c->src + i;
        /* Find end of line for parsing */
        size_t le = i;
        while (le < c->src_len && c->src[le] != '\n') le++;
        size_t line_len = le - i;

        if (line_len > 7 && memcmp(line, "@param ", 7) == 0) {
            /* @param NAME MIN..MAX = DEF [DESC] */
            const char *p = line + 7;
            const char *end = line + line_len;
            char name[PLBC_MAX_NAME] = {0};
            int ni = 0;
            while (p < end && (*p == ' ' || *p == '\t')) p++;
            while (p < end && ni < (int)sizeof(name) - 1 && is_ident_cont(*p)) name[ni++] = *p++;
            while (p < end && (*p == ' ' || *p == '\t')) p++;
            char range[40] = {0}; int ri = 0;
            while (p < end && ri < (int)sizeof(range) - 1 && *p != '=' && *p != ' ') range[ri++] = *p++;
            float lo = 0, hi = 1;
            uint8_t ptype = PLBC_PARAM_RANGE;
            char *dd = strstr(range, "..");
            if (dd) {
                *dd = 0; lo = (float)atof(range); hi = (float)atof(dd + 2);
            } else if (strcmp(range, "switch") == 0) {
                /* `@param N switch = 0` — a 0/1 toggle. The VM still sees a
                 * float in [0,1]; `type` only changes how clients render it. */
                ptype = PLBC_PARAM_SWITCH; lo = 0; hi = 1;
            }
            while (p < end && (*p == ' ' || *p == '\t' || *p == '=')) p++;
            char defbuf[32] = {0}; int di = 0;
            while (p < end && di < (int)sizeof(defbuf) - 1
                   && (*p == '-' || *p == '.' || (*p >= '0' && *p <= '9') || *p == 'e' || *p == 'E' || *p == '+'))
                defbuf[di++] = *p++;
            float def = (float)atof(defbuf);
            while (p < end && (*p == ' ' || *p == '\t')) p++;
            char desc[PLBC_MAX_DESC] = {0};
            size_t dn = end - p;
            if (dn > sizeof(desc) - 1) dn = sizeof(desc) - 1;
            memcpy(desc, p, dn);

            if (prog->n_params >= PLBC_MAX_PARAMS) { verr(c, "too many @param"); }
            else {
                plbc_param_t *param = &prog->params[prog->n_params++];
                strncpy(param->name, name, sizeof(param->name) - 1);
                param->min = lo; param->max = hi; param->def = def; param->value = def;
                param->type = ptype;
                strncpy(param->desc, desc, sizeof(param->desc) - 1);
            }
        } else if (line_len > 13 && memcmp(line, "@state.pixel ", 13) == 0) {
            const char *p = line + 13;
            const char *end = line + line_len;
            char name[PLBC_MAX_NAME] = {0}; int ni = 0;
            while (p < end && (*p == ' ' || *p == '\t')) p++;
            while (p < end && ni < (int)sizeof(name) - 1 && is_ident_cont(*p)) name[ni++] = *p++;
            while (p < end && (*p == ' ' || *p == '\t' || *p == ':')) p++;
            float def = (float)atof(p);
            if (prog->n_pixel_state >= PLBC_MAX_PIXEL_STATE) { verr(c, "too many @state.pixel"); }
            else {
                plbc_state_def_t *s = &prog->pixel_state_defs[prog->n_pixel_state++];
                strncpy(s->name, name, sizeof(s->name) - 1);
                s->def = def;
            }
        } else if (line_len > 7 && memcmp(line, "@state ", 7) == 0) {
            const char *p = line + 7;
            const char *end = line + line_len;
            char name[PLBC_MAX_NAME] = {0}; int ni = 0;
            while (p < end && (*p == ' ' || *p == '\t')) p++;
            while (p < end && ni < (int)sizeof(name) - 1 && is_ident_cont(*p)) name[ni++] = *p++;
            while (p < end && (*p == ' ' || *p == '\t' || *p == ':')) p++;
            float def = (float)atof(p);
            if (prog->n_frame_state >= PLBC_MAX_FRAME_STATE) { verr(c, "too many @state"); }
            else {
                plbc_state_def_t *s = &prog->frame_state_defs[prog->n_frame_state++];
                strncpy(s->name, name, sizeof(s->name) - 1);
                s->def = def;
            }
        } else if (line_len > 6 && memcmp(line, "@mode ", 6) == 0) {
            /* @mode strip|mirror — Phase 41. Declares the wormhole render mode
             * this effect is authored for; js_api_play() auto-switches the
             * device to it. Unrecognised values leave mode = -1 (no hint). */
            const char *p = line + 6;
            const char *end = line + line_len;
            while (p < end && (*p == ' ' || *p == '\t')) p++;
            if (end - p >= 6 && memcmp(p, "mirror", 6) == 0)     prog->mode = 1;
            else if (end - p >= 5 && memcmp(p, "strip", 5) == 0) prog->mode = 0;
        } else if (line_len >= 11 && memcmp(line, "@modeSwitch", 11) == 0) {
            /* @modeSwitch — Phase 41. Marks the effect as nice in both render
             * modes, so clients allow the user to change it. Absent (default)
             * means the effect is locked to its declared @mode. */
            prog->mode_switch = 1;
        }
        /* Other // ... comments are ignored. */
        i = le;
        if (i < c->src_len) i++;
    }
}

/* ---------- Parser: expressions -------------------------------------- */

static void parse_expr(ctx_t *c);

/* Built-in function call (sinLUT, floor, emit, ...). Returns true if name
 * matched a built-in and was emitted; caller still consumes args / closing
 * paren. */
static bool emit_builtin_call(ctx_t *c, const char *name, int argc)
{
    /* Single-arg unary functions */
    struct { const char *n; uint8_t op; int argc; } unary[] = {
        {"sinLUT", PLBC_SIN_LUT, 1},
        {"cosLUT", PLBC_COS_LUT, 1},
        {"floor",  PLBC_FLOOR,   1},
        {"ceil",   PLBC_CEIL,    1},
        {"round",  PLBC_ROUND,   1},
        {"abs",    PLBC_ABS,     1},
        {"sqrt",   PLBC_SQRT,    1},
        {"clamp01",PLBC_CLAMP01, 1},
        {"hash",   PLBC_HASH_I,  1},
    };
    for (size_t i = 0; i < sizeof(unary) / sizeof(unary[0]); i++) {
        if (strcmp(name, unary[i].n) == 0) {
            if (argc != unary[i].argc) { verr(c, "%s expects %d arg", name, unary[i].argc); return true; }
            emit_u8(c, unary[i].op);
            return true;
        }
    }
    if (strcmp(name, "min") == 0) {
        if (argc != 2) { verr(c, "min expects 2 args"); return true; }
        emit_u8(c, PLBC_MIN); return true;
    }
    if (strcmp(name, "max") == 0) {
        if (argc != 2) { verr(c, "max expects 2 args"); return true; }
        emit_u8(c, PLBC_MAX); return true;
    }
    if (strcmp(name, "pow") == 0) {
        if (argc != 2) { verr(c, "pow expects 2 args"); return true; }
        emit_u8(c, PLBC_POW); return true;
    }
    if (strcmp(name, "random") == 0) {
        if (argc != 0) { verr(c, "random takes no args"); return true; }
        emit_u8(c, PLBC_RANDOM); return true;
    }
    if (strcmp(name, "emit") == 0) {
        if (argc != 3) { verr(c, "emit expects 3 args (r, g, b)"); return true; }
        emit_u8(c, PLBC_EMIT_RGB); return true;
    }
    if (strcmp(name, "emitBright") == 0) {
        if (argc != 1) { verr(c, "emitBright expects 1 arg"); return true; }
        emit_u8(c, PLBC_EMIT_BRIGHT); return true;
    }
    return false;
}

/* Parse arglist and return argc. Arglist tokens consumed up to and
 * including the ')'. */
static int parse_arglist(ctx_t *c)
{
    expect(c, TOK_LPAREN, "'('");
    int argc = 0;
    if (c->tok_kind != TOK_RPAREN) {
        for (;;) {
            parse_expr(c);
            argc++;
            if (!accept(c, TOK_COMMA)) break;
        }
    }
    expect(c, TOK_RPAREN, "')'");
    return argc;
}

static void parse_primary(ctx_t *c)
{
    if (c->tok_kind == TOK_NUM) {
        emit_push_num(c, c->tok_num);
        next_token(c);
        return;
    }
    if (c->tok_kind == TOK_LPAREN) {
        next_token(c);
        parse_expr(c);
        expect(c, TOK_RPAREN, "')'");
        return;
    }
    if (c->tok_kind == TOK_MINUS) {
        /* Unary minus handled by unary, but allow here too */
        next_token(c);
        parse_primary(c);
        emit_u8(c, PLBC_NEG);
        return;
    }
    if (c->tok_kind == TOK_NOT) {
        next_token(c);
        parse_primary(c);
        emit_u8(c, PLBC_NOT);
        return;
    }
    if (c->tok_kind == TOK_IDENT) {
        char name[PLBC_MAX_NAME];
        strncpy(name, c->tok_str, sizeof(name));
        name[sizeof(name) - 1] = 0;
        next_token(c);

        /* Postfix: "name.member" */
        if (c->tok_kind == TOK_DOT) {
            next_token(c);
            if (c->tok_kind != TOK_IDENT) { verr(c, "expected member after '.'"); return; }
            char member[PLBC_MAX_NAME];
            strncpy(member, c->tok_str, sizeof(member));
            member[sizeof(member) - 1] = 0;
            next_token(c);

            if (strcmp(name, "params") == 0) {
                int idx = find_param(c->prog, member);
                if (idx < 0) { verr(c, "unknown param '%s'", member); return; }
                emit_u8(c, PLBC_LOAD_PARAM);
                emit_u8(c, (uint8_t)idx);
                return;
            }
            if (strcmp(name, "base") == 0) {
                uint8_t op = base_opcode(member);
                if (!op) { verr(c, "base.%s is not r/g/b", member); return; }
                emit_u8(c, op);
                return;
            }
            if (strcmp(member, "pixel") == 0) {
                int idx = find_pstate(c->prog, name);
                if (idx < 0) { verr(c, "undeclared @state.pixel '%s'", name); return; }
                emit_u8(c, PLBC_LOAD_PSTATE);
                emit_u8(c, (uint8_t)idx);
                return;
            }
            verr(c, "unknown member access %s.%s", name, member);
            return;
        }

        /* Function call */
        if (c->tok_kind == TOK_LPAREN) {
            int argc = parse_arglist(c);
            if (!emit_builtin_call(c, name, argc)) verr(c, "unknown function '%s'", name);
            return;
        }

        /* Bare identifier — input, local, or frame-state. */
        uint8_t input_op = input_opcode(name);
        if (input_op) { emit_u8(c, input_op); return; }
        int li = find_local(c, name);
        if (li >= 0) {
            emit_u8(c, PLBC_LOAD_LOCAL);
            emit_u8(c, (uint8_t)li);
            return;
        }
        int fi = find_fstate(c->prog, name);
        if (fi >= 0) {
            emit_u8(c, PLBC_LOAD_FSTATE);
            emit_u8(c, (uint8_t)fi);
            return;
        }
        verr(c, "unknown identifier '%s'", name);
        return;
    }
    verr(c, "expected expression");
}

static void parse_unary(ctx_t *c)
{
    if (accept(c, TOK_MINUS))    { parse_unary(c); emit_u8(c, PLBC_NEG); return; }
    if (accept(c, TOK_NOT))      { parse_unary(c); emit_u8(c, PLBC_NOT); return; }
    parse_primary(c);
}

static void parse_product(ctx_t *c)
{
    parse_unary(c);
    while (c->tok_kind == TOK_STAR || c->tok_kind == TOK_SLASH || c->tok_kind == TOK_PERCENT) {
        int op = c->tok_kind;
        next_token(c);
        parse_unary(c);
        emit_u8(c, op == TOK_STAR ? PLBC_MUL : op == TOK_SLASH ? PLBC_DIV : PLBC_MOD);
    }
}

static void parse_sum(ctx_t *c)
{
    parse_product(c);
    while (c->tok_kind == TOK_PLUS || c->tok_kind == TOK_MINUS) {
        int op = c->tok_kind;
        next_token(c);
        parse_product(c);
        emit_u8(c, op == TOK_PLUS ? PLBC_ADD : PLBC_SUB);
    }
}

static void parse_cmp(ctx_t *c)
{
    parse_sum(c);
    while (c->tok_kind == TOK_LT || c->tok_kind == TOK_LE
        || c->tok_kind == TOK_GT || c->tok_kind == TOK_GE
        || c->tok_kind == TOK_EQ || c->tok_kind == TOK_NE) {
        int op = c->tok_kind;
        next_token(c);
        parse_sum(c);
        uint8_t opc;
        switch (op) {
            case TOK_LT: opc = PLBC_LT; break;
            case TOK_LE: opc = PLBC_LE; break;
            case TOK_GT: opc = PLBC_GT; break;
            case TOK_GE: opc = PLBC_GE; break;
            case TOK_EQ: opc = PLBC_EQ; break;
            default:     opc = PLBC_NE; break;
        }
        emit_u8(c, opc);
    }
}

static void parse_and_expr(ctx_t *c)
{
    parse_cmp(c);
    while (c->tok_kind == TOK_AND) { next_token(c); parse_cmp(c); emit_u8(c, PLBC_AND); }
}

static void parse_expr(ctx_t *c)
{
    parse_and_expr(c);
    while (c->tok_kind == TOK_OR) { next_token(c); parse_and_expr(c); emit_u8(c, PLBC_OR); }
}

/* ---------- Parser: statements --------------------------------------- */

static void parse_block(ctx_t *c);

/* Emit a store instruction for a previously-parsed lvalue. lvalue is encoded
 * as `kind` + index. kind: 0=local, 1=fstate, 2=pstate. */
static void emit_store(ctx_t *c, int kind, int idx)
{
    switch (kind) {
        case 0: emit_u8(c, PLBC_STORE_LOCAL);  emit_u8(c, (uint8_t)idx); break;
        case 1: emit_u8(c, PLBC_STORE_FSTATE); emit_u8(c, (uint8_t)idx); break;
        case 2: emit_u8(c, PLBC_STORE_PSTATE); emit_u8(c, (uint8_t)idx); break;
        default: verr(c, "internal: bad lvalue"); break;
    }
}
static void emit_load(ctx_t *c, int kind, int idx)
{
    switch (kind) {
        case 0: emit_u8(c, PLBC_LOAD_LOCAL);  emit_u8(c, (uint8_t)idx); break;
        case 1: emit_u8(c, PLBC_LOAD_FSTATE); emit_u8(c, (uint8_t)idx); break;
        case 2: emit_u8(c, PLBC_LOAD_PSTATE); emit_u8(c, (uint8_t)idx); break;
    }
}

/* Resolve an lvalue identifier (or NAME.pixel). Sets *kind, *idx. */
static bool resolve_lvalue(ctx_t *c, const char *name, bool dot_pixel, int *kind, int *idx)
{
    if (dot_pixel) {
        int i = find_pstate(c->prog, name);
        if (i < 0) { verr(c, "undeclared @state.pixel '%s'", name); return false; }
        *kind = 2; *idx = i; return true;
    }
    int li = find_local(c, name);
    if (li >= 0) { *kind = 0; *idx = li; return true; }
    int fi = find_fstate(c->prog, name);
    if (fi >= 0) { *kind = 1; *idx = fi; return true; }
    verr(c, "cannot assign to '%s' (not a let/state)", name);
    return false;
}

static void parse_stmt(ctx_t *c)
{
    if (c->has_err) return;

    /* let NAME = expr ; */
    if (accept(c, TOK_LET)) {
        if (c->tok_kind != TOK_IDENT) { verr(c, "expected name after 'let'"); return; }
        char name[PLBC_MAX_NAME];
        strncpy(name, c->tok_str, sizeof(name));
        name[sizeof(name) - 1] = 0;
        next_token(c);
        if (find_local(c, name) >= 0) { verr(c, "duplicate let '%s'", name); return; }
        int idx = add_local(c, name);
        expect(c, TOK_ASSIGN, "'='");
        parse_expr(c);
        expect(c, TOK_SEMI, "';'");
        emit_u8(c, PLBC_STORE_LOCAL);
        emit_u8(c, (uint8_t)idx);
        return;
    }

    /* if ( expr ) { ... } else? */
    if (accept(c, TOK_IF)) {
        expect(c, TOK_LPAREN, "'('");
        parse_expr(c);
        expect(c, TOK_RPAREN, "')'");
        emit_u8(c, PLBC_JMP_IF_FALSE);
        uint16_t fixup_else = emit_i16_placeholder(c);
        uint16_t after_jif = c->cur_pc;
        parse_block(c);
        if (accept(c, TOK_ELSE)) {
            emit_u8(c, PLBC_JMP);
            uint16_t fixup_end = emit_i16_placeholder(c);
            uint16_t after_jmp = c->cur_pc;
            emit_i16_at(c, fixup_else, (int16_t)((int)c->cur_pc - (int)after_jif));
            if (c->tok_kind == TOK_IF) parse_stmt(c);  /* else if */
            else                        parse_block(c);
            emit_i16_at(c, fixup_end, (int16_t)((int)c->cur_pc - (int)after_jmp));
        } else {
            emit_i16_at(c, fixup_else, (int16_t)((int)c->cur_pc - (int)after_jif));
        }
        return;
    }

    /* for ( let i = N ; i < M ; i++ ) { ... }  (bounded only) */
    if (accept(c, TOK_FOR)) {
        expect(c, TOK_LPAREN, "'('");
        expect(c, TOK_LET, "'let'");
        if (c->tok_kind != TOK_IDENT) { verr(c, "expected loop var"); return; }
        char iname[PLBC_MAX_NAME];
        strncpy(iname, c->tok_str, sizeof(iname));
        iname[sizeof(iname) - 1] = 0;
        next_token(c);
        expect(c, TOK_ASSIGN, "'='");
        parse_expr(c);
        expect(c, TOK_SEMI, "';'");
        int li = find_local(c, iname);
        if (li < 0) li = add_local(c, iname);
        emit_u8(c, PLBC_STORE_LOCAL); emit_u8(c, (uint8_t)li);

        uint16_t loop_top = c->cur_pc;
        /* Condition */
        parse_expr(c);
        expect(c, TOK_SEMI, "';'");
        emit_u8(c, PLBC_JMP_IF_FALSE);
        uint16_t fixup_exit = emit_i16_placeholder(c);
        uint16_t after_jif = c->cur_pc;

        /* Capture the increment text to replay at end-of-body. Simpler:
         * compile increment into a small buffer, paste later. Tiny-scope
         * approach: only support `i++` or `i += N`. */
        size_t inc_start_pos = c->pos - strlen(c->tok_str) + (c->tok_kind == TOK_IDENT ? 0 : 0);
        /* Skip the increment for now — re-tokenize at body end. */
        /* Easier: parse the increment into bytecode now, but store its
         * bytecode separately and emit after body. */
        uint16_t inc_buf_start = c->cur_pc;
        if (c->tok_kind == TOK_IDENT) {
            char nm[PLBC_MAX_NAME]; strncpy(nm, c->tok_str, sizeof(nm)); nm[sizeof(nm) - 1] = 0;
            next_token(c);
            if (accept(c, TOK_PLUSPLUS)) {
                int lj = find_local(c, nm); if (lj < 0) { verr(c, "unknown loop var"); return; }
                emit_u8(c, PLBC_LOAD_LOCAL); emit_u8(c, (uint8_t)lj);
                emit_u8(c, PLBC_PUSH_ONE);
                emit_u8(c, PLBC_ADD);
                emit_u8(c, PLBC_STORE_LOCAL); emit_u8(c, (uint8_t)lj);
            } else if (accept(c, TOK_PLUS_ASSIGN)) {
                int lj = find_local(c, nm); if (lj < 0) { verr(c, "unknown loop var"); return; }
                emit_u8(c, PLBC_LOAD_LOCAL); emit_u8(c, (uint8_t)lj);
                parse_expr(c);
                emit_u8(c, PLBC_ADD);
                emit_u8(c, PLBC_STORE_LOCAL); emit_u8(c, (uint8_t)lj);
            } else {
                verr(c, "for-loop step must be 'i++' or 'i += N'"); return;
            }
        } else {
            verr(c, "for-loop step missing"); return;
        }
        uint16_t inc_buf_end = c->cur_pc;
        /* Move increment bytecode out of the stream so we can put it after
         * the body. Simple approach: copy to a temp, splice later. */
        uint8_t inc_save[16];
        uint16_t inc_size = inc_buf_end - inc_buf_start;
        if (inc_size > sizeof(inc_save)) { verr(c, "for-step too large"); return; }
        memcpy(inc_save, c->prog->code + inc_buf_start, inc_size);
        c->cur_pc = inc_buf_start;  /* rewind */

        expect(c, TOK_RPAREN, "')'");

        /* Body */
        parse_block(c);

        /* Emit increment, then jump back to top. */
        if (c->cur_pc + inc_size > PLBC_MAX_CODE) { verr(c, "bytecode too large"); return; }
        memcpy(c->prog->code + c->cur_pc, inc_save, inc_size);
        c->cur_pc += inc_size;

        emit_u8(c, PLBC_JMP);
        uint16_t fixup_back = emit_i16_placeholder(c);
        emit_i16_at(c, fixup_back, (int16_t)((int)loop_top - (int)c->cur_pc));

        emit_i16_at(c, fixup_exit, (int16_t)((int)c->cur_pc - (int)after_jif));
        (void)inc_start_pos;
        return;
    }

    /* ident . pixel? (= | += | etc) expr ;   OR   ident(args) ;   */
    if (c->tok_kind == TOK_IDENT) {
        char name[PLBC_MAX_NAME];
        strncpy(name, c->tok_str, sizeof(name));
        name[sizeof(name) - 1] = 0;
        next_token(c);

        bool dot_pixel = false;
        if (accept(c, TOK_DOT)) {
            if (c->tok_kind != TOK_IDENT || strcmp(c->tok_str, "pixel") != 0) {
                verr(c, "expected .pixel"); return;
            }
            dot_pixel = true;
            next_token(c);
        }

        if (c->tok_kind == TOK_LPAREN) {
            /* expression statement: function call discarded */
            int argc = parse_arglist(c);
            if (!emit_builtin_call(c, name, argc)) verr(c, "unknown function '%s'", name);
            expect(c, TOK_SEMI, "';'");
            return;
        }

        int kind = 0, idx = 0;
        if (!resolve_lvalue(c, name, dot_pixel, &kind, &idx)) return;

        if (accept(c, TOK_ASSIGN)) {
            parse_expr(c);
            expect(c, TOK_SEMI, "';'");
            emit_store(c, kind, idx);
            return;
        }
        if (accept(c, TOK_PLUS_ASSIGN) || accept(c, TOK_MINUS_ASSIGN)
            || accept(c, TOK_MUL_ASSIGN) || accept(c, TOK_DIV_ASSIGN)) {
            /* Compound op: load current, parse expr, op, store. Need to
             * remember which op we just consumed. */
            /* We already consumed; previous tok_kind is now the next one.
             * Easier: handle each case explicitly above. */
            verr(c, "compound assignment not supported yet"); return;
        }
        /* Manual handling because accept() advances past the token. */
        verr(c, "expected '=' after lvalue"); return;
    }

    verr(c, "unexpected statement");
}

static void parse_block(ctx_t *c)
{
    expect(c, TOK_LBRACE, "'{'");
    while (c->tok_kind != TOK_RBRACE && c->tok_kind != TOK_EOF && !c->has_err) {
        parse_stmt(c);
    }
    expect(c, TOK_RBRACE, "'}'");
}

/* ---------- Top-level ------------------------------------------------ */

esp_err_t plbc_compile(const char *source, size_t source_len,
                       plbc_program_t *out_prog,
                       char *err_buf, size_t err_buf_size)
{
    if (!source || !out_prog) return ESP_ERR_INVALID_ARG;
    memset(out_prog, 0, sizeof(*out_prog));
    out_prog->mode = -1;  /* no @mode hint unless the source declares one */
    if (err_buf && err_buf_size) err_buf[0] = 0;

    /* Tolerate trailing NUL terminators. EMBED_TXTFILES appends one to every
     * embedded built-in, and a SPIFFS round-trip can carry it along; without
     * this the tokenizer would hit the '\0' and report "unexpected
     * character" at the final line. */
    while (source_len > 0 && source[source_len - 1] == '\0') source_len--;

    ctx_t c = {0};
    c.src = source;
    c.src_len = source_len;
    c.prog = out_prog;
    c.err_buf = err_buf;
    c.err_buf_size = err_buf_size;
    c.tok_line = 1;

    /* Pass 1: scan annotations into the schema. */
    parse_annotations(&c);
    if (c.has_err) return ESP_FAIL;

    /* Pass 2: tokenize + emit. */
    c.pos = 0; c.tok_line = 1;
    next_token(&c);

    /* Skip until 'function' (we don't run any code outside it). */
    while (c.tok_kind != TOK_EOF && c.tok_kind != TOK_FUNCTION) next_token(&c);
    if (c.tok_kind != TOK_FUNCTION) { verr(&c, "no function shade() found"); return ESP_FAIL; }
    next_token(&c);

    if (c.tok_kind != TOK_IDENT || strcmp(c.tok_str, "shade") != 0) {
        verr(&c, "function must be named 'shade'");
        return ESP_FAIL;
    }
    next_token(&c);
    expect(&c, TOK_LPAREN, "'('");
    /* Skip parameter list — they're positional and reserved (x, y, idx,
     * frame, base, params). The compiler doesn't care about the names the
     * author wrote; it generates LOAD_X / LOAD_Y / etc on bare references. */
    while (c.tok_kind != TOK_RPAREN && c.tok_kind != TOK_EOF) next_token(&c);
    expect(&c, TOK_RPAREN, "')'");

    parse_block(&c);

    if (c.has_err) return ESP_FAIL;

    emit_u8(&c, PLBC_HALT);
    out_prog->code_size = c.cur_pc;
    out_prog->n_locals = (uint8_t)c.n_locals;
    return ESP_OK;
}
