#include "ai_access.h"
#include <string.h>

static const struct { uint32_t bit; const char *name; } k_names[] = {
    {AI_PERM_CONTROL, "control"}, {AI_PERM_EFFECTS, "effects"}, {AI_PERM_IMAGES, "images"},
    {AI_PERM_SCENES, "scenes"},   {AI_PERM_STREAM, "stream"},   {AI_PERM_OTA, "ota"},
    {AI_PERM_RENAME, "rename"},
};
#define N_NAMES (sizeof k_names / sizeof k_names[0])

const char *ai_perm_name(uint32_t single_bit)
{
    for (unsigned i = 0; i < N_NAMES; i++)
        if (k_names[i].bit == single_bit) return k_names[i].name;
    return NULL;
}

uint32_t ai_perm_from_name(const char *name)
{
    if (!name) return 0;
    for (unsigned i = 0; i < N_NAMES; i++)
        if (strcmp(k_names[i].name, name) == 0) return k_names[i].bit;
    return 0;
}

typedef struct { ai_method_t m; const char *pattern; uint32_t perm; } route_t;

static const route_t k_routes[] = {
    {AI_M_POST,   "/api/power",                   AI_PERM_CONTROL},
    {AI_M_POST,   "/api/color",                   AI_PERM_CONTROL},
    {AI_M_POST,   "/api/brightness",              AI_PERM_CONTROL},
    {AI_M_POST,   "/api/play",                    AI_PERM_CONTROL},
    {AI_M_POST,   "/api/play/next",               AI_PERM_CONTROL},
    {AI_M_POST,   "/api/play/prev",               AI_PERM_CONTROL},
    {AI_M_POST,   "/api/stop",                    AI_PERM_CONTROL},
    {AI_M_POST,   "/api/fade",                    AI_PERM_CONTROL},
    {AI_M_PUT,    "/api/mode",                    AI_PERM_CONTROL},
    {AI_M_POST,   "/api/mode",                    AI_PERM_CONTROL},
    {AI_M_POST,   "/api/scenes/*/activate",       AI_PERM_CONTROL},
    {AI_M_POST,   "/api/js/validate",             AI_PERM_EFFECTS},
    {AI_M_POST,   "/api/js/compile",              AI_PERM_EFFECTS},
    {AI_M_PUT,    "/api/js/*",                    AI_PERM_EFFECTS},
    {AI_M_DELETE, "/api/js/*",                    AI_PERM_EFFECTS},
    {AI_M_PUT,    "/api/js/*/params",             AI_PERM_EFFECTS},
    {AI_M_POST,   "/api/frame",                   AI_PERM_IMAGES},
    {AI_M_POST,   "/api/images",                  AI_PERM_IMAGES},
    {AI_M_POST,   "/api/images/*/show",           AI_PERM_IMAGES},
    {AI_M_POST,   "/api/images/*/label",          AI_PERM_IMAGES},
    {AI_M_DELETE, "/api/images/*",                AI_PERM_IMAGES},
    {AI_M_PUT,    "/api/scenes/*",                AI_PERM_SCENES},
    {AI_M_DELETE, "/api/scenes/*",                AI_PERM_SCENES},
    {AI_M_POST,   "/api/scenes/*/capture",        AI_PERM_SCENES},
    {AI_M_GET,    "/ws",                          AI_PERM_STREAM},
    {AI_M_POST,   "/api/ota",                     AI_PERM_OTA},
    {AI_M_POST,   "/api/name",                    AI_PERM_RENAME},
};

/* path (length n, query already stripped) against pattern; '*' = one non-empty segment */
static bool match(const char *pat, const char *p, size_t n)
{
    const char *end = p + n;
    while (*pat) {
        if (p >= end || *p != '/' || *pat != '/') return false;
        pat++; p++;
        const char *seg = p;
        while (p < end && *p != '/') p++;
        size_t sl = (size_t)(p - seg);
        const char *pe = pat;
        while (*pe && *pe != '/') pe++;
        size_t pl = (size_t)(pe - pat);
        if (pl == 1 && *pat == '*') {
            if (sl == 0) return false;
        } else if (pl != sl || memcmp(pat, seg, sl) != 0) {
            return false;
        }
        pat = pe;
    }
    return p == end;
}

uint32_t ai_route_perm(ai_method_t m, const char *uri)
{
    if (!uri) return m == AI_M_GET ? AI_ROUTE_READ : AI_ROUTE_DENIED;
    size_t n = strcspn(uri, "?");
    if (n > 1 && uri[n - 1] == '/') n--;
    for (unsigned i = 0; i < sizeof k_routes / sizeof k_routes[0]; i++)
        if (k_routes[i].m == m && match(k_routes[i].pattern, uri, n)) return k_routes[i].perm;
    return m == AI_M_GET ? AI_ROUTE_READ : AI_ROUTE_DENIED;
}
