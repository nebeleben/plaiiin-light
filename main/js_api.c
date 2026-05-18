#include "js_api.h"
#include "js_storage.h"
#include "js_player.h"
#include "plbc.h"
#include "config_store.h"
#include "pairing.h"
#include <stdbool.h>

#include "esp_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "js_api";

#define MAX_BC_BYTES (16 * 1024)

#define MAX_UPLOAD_BYTES (48 * 1024)

/** Extract name from "/api/js/<name>" or "/api/js/<name>/params". Writes
 *  the name into `out` (NUL-terminated), and returns true if the URI ends
 *  with "/params" (so the caller dispatches to the params handler instead
 *  of the source handler). */
static bool split_uri(const char *uri, char *out, size_t out_cap, bool *is_params)
{
    *is_params = false;
    if (out_cap == 0) return false;
    out[0] = '\0';
    const char *prefix = "/api/js/";
    if (strncmp(uri, prefix, strlen(prefix)) != 0) return false;
    const char *name = uri + strlen(prefix);
    if (*name == 0) return false;
    // Strip trailing "/params" if present.
    size_t len = strlen(name);
    const char *suffix = "/params";
    size_t slen = strlen(suffix);
    if (len > slen && strcmp(name + len - slen, suffix) == 0) {
        *is_params = true;
        len -= slen;
    }
    if (len + 1 > out_cap) return false;
    memcpy(out, name, len);
    out[len] = '\0';
    return out[0] != '\0';
}

/** Back-compat wrapper for the existing source handlers. Returns NULL when
 *  the URI is the params sub-resource so callers naturally fall through to
 *  the dispatcher. */
static const char *name_from_uri(const char *uri)
{
    static char buf[64];
    bool is_params = false;
    if (!split_uri(uri, buf, sizeof(buf), &is_params)) return NULL;
    if (is_params) return NULL;
    return buf;
}

static esp_err_t send_err_json(httpd_req_t *req, int status, const char *msg)
{
    httpd_resp_set_status(req, status == 404 ? "404 Not Found"
                              : status == 400 ? "400 Bad Request"
                              : "500 Internal Server Error");
    httpd_resp_set_type(req, "application/json");
    char resp[192];
    snprintf(resp, sizeof(resp), "{\"status\":\"error\",\"message\":\"%s\"}", msg);
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

// GET /api/js -> {"scripts":[...],"playing":"name"|null}
static esp_err_t list_handler(httpd_req_t *req)
{
    if (pairing_http_check(req, PL_ROLE_USER) != ESP_OK) return ESP_FAIL;
    char scripts[768];
    esp_err_t err = js_storage_list(scripts, sizeof(scripts));
    if (err != ESP_OK) return send_err_json(req, 500, "list failed");

    const char *playing = js_player_current_name();
    char resp[900];
    if (playing) {
        snprintf(resp, sizeof(resp), "{\"scripts\":%s,\"playing\":\"%s\"}", scripts, playing);
    } else {
        snprintf(resp, sizeof(resp), "{\"scripts\":%s,\"playing\":null}", scripts);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

// GET /api/js/<name>            -> script source as text/javascript
// GET /api/js/<name>/params     -> {"items":[{"name":..,"min":..,...}]}
static esp_err_t read_handler(httpd_req_t *req)
{
    if (pairing_http_check(req, PL_ROLE_USER) != ESP_OK) return ESP_FAIL;
    char name[64];
    bool is_params = false;
    if (!split_uri(req->uri, name, sizeof(name), &is_params)) {
        return send_err_json(req, 400, "missing name");
    }
    if (is_params) {
        /* Phase 23 — schema lives in the compiled .bc. Load it, emit JSON
         * via PLBC. Works for any script in storage, not just the running
         * one (the player keeps its own copy in memory). */
        void *bc = NULL; size_t bc_len = 0;
        esp_err_t err = js_storage_read_bc(name, &bc, &bc_len);
        if (err == ESP_ERR_NOT_FOUND) return send_err_json(req, 404, "not found");
        if (err != ESP_OK) return send_err_json(req, 500, "read failed");
        plbc_program_t *prog = (plbc_program_t *)calloc(1, sizeof(*prog));
        if (!prog) { free(bc); return send_err_json(req, 500, "oom"); }
        char perr[64] = {0};
        if (plbc_load(bc, bc_len, prog, perr, sizeof(perr)) != ESP_OK) {
            free(bc); free(prog);
            return send_err_json(req, 500, perr[0] ? perr : "load failed");
        }
        free(bc);
        char buf[2048];
        int n = plbc_params_to_json(prog, buf, sizeof(buf));
        free(prog);
        if (n <= 0) return send_err_json(req, 500, "params encode failed");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, buf, n);
        return ESP_OK;
    }
    /* Phase 23 — GET /api/js/<name>.bc returns the raw bytecode. We strip
     * the `.bc` suffix in split_uri's output if present here, but for the
     * regular .js case the name has no extension and we serve the source. */
    size_t nlen = strlen(name);
    if (nlen > 3 && strcmp(name + nlen - 3, ".bc") == 0) {
        char base[64]; size_t bl = nlen - 3;
        if (bl >= sizeof(base)) return send_err_json(req, 400, "name too long");
        memcpy(base, name, bl); base[bl] = 0;
        void *bc = NULL; size_t bc_len = 0;
        esp_err_t err = js_storage_read_bc(base, &bc, &bc_len);
        if (err == ESP_ERR_NOT_FOUND) return send_err_json(req, 404, "not found");
        if (err != ESP_OK) return send_err_json(req, 500, "read failed");
        httpd_resp_set_type(req, "application/octet-stream");
        httpd_resp_send(req, bc, bc_len);
        free(bc);
        return ESP_OK;
    }
    char *source = NULL;
    size_t len = 0;
    esp_err_t err = js_storage_read(name, &source, &len);
    if (err == ESP_ERR_NOT_FOUND) return send_err_json(req, 404, "not found");
    if (err != ESP_OK) return send_err_json(req, 500, "read failed");
    httpd_resp_set_type(req, "text/javascript");
    httpd_resp_send(req, source, len);
    free(source);
    return ESP_OK;
}

static esp_err_t receive_body(httpd_req_t *req, char **out, size_t *len_out)
{
    int content_len = req->content_len;
    if (content_len <= 0 || content_len > MAX_UPLOAD_BYTES) return ESP_ERR_INVALID_SIZE;
    char *buf = (char *)malloc(content_len + 1);
    if (!buf) return ESP_ERR_NO_MEM;
    int received = 0;
    while (received < content_len) {
        int ret = httpd_req_recv(req, buf + received, content_len - received);
        if (ret <= 0) { free(buf); return ESP_FAIL; }
        received += ret;
    }
    buf[content_len] = 0;
    *out = buf;
    *len_out = content_len;
    return ESP_OK;
}

int js_api_write_script(const char *name, const char *body, size_t len,
                        char *err_buf, size_t err_len)
{
    /* Phase 23 — compile to bytecode in place. Store both .js (source of
     * truth for editing) and .bc (what the runtime player loads). If the
     * compile fails we DON'T write the .js — keeping disk consistent. */
    plbc_program_t *prog = (plbc_program_t *)calloc(1, sizeof(*prog));
    if (!prog) {
        if (err_buf && err_len) snprintf(err_buf, err_len, "oom");
        return 0;
    }
    char cerr[128] = {0};
    esp_err_t err = plbc_compile(body, len, prog, cerr, sizeof(cerr));
    if (err != ESP_OK) {
        free(prog);
        if (err_buf && err_len) snprintf(err_buf, err_len, "compile: %s", cerr[0] ? cerr : "?");
        return 0;
    }
    /* Heap, not stack — MAX_BC_BYTES is 16 KB; HTTP task stack is ~4 KB. */
    uint8_t *bc = (uint8_t *)malloc(MAX_BC_BYTES);
    if (!bc) { free(prog); if (err_buf && err_len) snprintf(err_buf, err_len, "oom"); return 0; }
    int n_bc = plbc_serialize(prog, bc, MAX_BC_BYTES);
    free(prog);
    if (n_bc <= 0) {
        free(bc);
        if (err_buf && err_len) snprintf(err_buf, err_len, "bytecode too large");
        return 0;
    }
    err = js_storage_write(name, body, len);
    if (err != ESP_OK) {
        free(bc);
        if (err_buf && err_len) snprintf(err_buf, err_len, "write .js failed");
        return 0;
    }
    err = js_storage_write_bc(name, bc, (size_t)n_bc);
    free(bc);
    if (err != ESP_OK) {
        if (err_buf && err_len) snprintf(err_buf, err_len, "write .bc failed");
        return 0;
    }
    return 1;
}

// PUT /api/js/<name>            body: raw JS source
// PUT /api/js/<name>/params     body: {"name":value,...} — partial OK
static esp_err_t write_handler(httpd_req_t *req)
{
    if (pairing_http_check(req, PL_ROLE_CREATOR) != ESP_OK) return ESP_FAIL;
    char name[64];
    bool is_params = false;
    if (!split_uri(req->uri, name, sizeof(name), &is_params)) {
        return send_err_json(req, 400, "missing name");
    }
    char *body = NULL; size_t body_len = 0;
    esp_err_t err = receive_body(req, &body, &body_len);
    if (err != ESP_OK) return send_err_json(req, 400, "bad body");

    if (is_params) {
        /* Phase 23 — load .bc, apply JSON patch, re-serialize. */
        void *bc = NULL; size_t bc_len = 0;
        esp_err_t rerr = js_storage_read_bc(name, &bc, &bc_len);
        if (rerr == ESP_ERR_NOT_FOUND) { free(body); return send_err_json(req, 404, "not found"); }
        if (rerr != ESP_OK)            { free(body); return send_err_json(req, 500, "read failed"); }
        plbc_program_t *prog = (plbc_program_t *)calloc(1, sizeof(*prog));
        if (!prog) { free(bc); free(body); return send_err_json(req, 500, "oom"); }
        char perr[64] = {0};
        if (plbc_load(bc, bc_len, prog, perr, sizeof(perr)) != ESP_OK) {
            free(bc); free(prog); free(body);
            return send_err_json(req, 500, perr[0] ? perr : "load failed");
        }
        free(bc);
        plbc_apply_params_json(prog, body, body_len);
        /* Heap, not stack — httpd task stack is ~4 KB; MAX_BC_BYTES is 16 KB. */
        uint8_t *out_bc = (uint8_t *)malloc(MAX_BC_BYTES);
        if (!out_bc) { free(prog); free(body); return send_err_json(req, 500, "oom"); }
        int n_bc = plbc_serialize(prog, out_bc, MAX_BC_BYTES);
        if (n_bc > 0) js_storage_write_bc(name, out_bc, (size_t)n_bc);
        free(out_bc);
        /* Push live values into the running player if it's playing this script. */
        const char *playing = js_player_current_name();
        if (playing && strcmp(playing, name) == 0) {
            js_player_apply_params_json(body, body_len);
        }
        free(body);
        char buf[2048];
        int n = plbc_params_to_json(prog, buf, sizeof(buf));
        free(prog);
        if (n <= 0) return send_err_json(req, 500, "params encode failed");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, buf, n);
        return ESP_OK;
    }

    char errbuf[160] = {0};
    int ok = js_api_write_script(name, body, body_len, errbuf, sizeof(errbuf));
    free(body);
    if (!ok) {
        bool is_validation = strncmp(errbuf, "compile", 7) == 0 || strncmp(errbuf, "validation", 10) == 0;
        return send_err_json(req, is_validation ? 400 : 500, errbuf);
    }
    httpd_resp_set_type(req, "application/json");
    char resp[96];
    snprintf(resp, sizeof(resp), "{\"status\":\"ok\",\"name\":\"%s\"}", name);
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

/* Phase 23 — POST /api/js/compile  body: raw JS source
 *
 * Compiles without saving. Returns the bytecode as application/octet-stream
 * on success, or a JSON error body on failure. macOS preview uses this to
 * run new scripts in its Swift VM without round-tripping a save. */
static esp_err_t compile_handler(httpd_req_t *req)
{
    if (pairing_http_check(req, PL_ROLE_CREATOR) != ESP_OK) return ESP_FAIL;
    char *body = NULL; size_t body_len = 0;
    esp_err_t err = receive_body(req, &body, &body_len);
    if (err != ESP_OK) return send_err_json(req, 400, "bad body");
    plbc_program_t *prog = (plbc_program_t *)calloc(1, sizeof(*prog));
    if (!prog) { free(body); return send_err_json(req, 500, "oom"); }
    char cerr[128] = {0};
    err = plbc_compile(body, body_len, prog, cerr, sizeof(cerr));
    free(body);
    if (err != ESP_OK) {
        free(prog);
        return send_err_json(req, 400, cerr[0] ? cerr : "compile failed");
    }
    uint8_t *bc = (uint8_t *)malloc(MAX_BC_BYTES);
    if (!bc) { free(prog); return send_err_json(req, 500, "oom"); }
    int n = plbc_serialize(prog, bc, MAX_BC_BYTES);
    free(prog);
    if (n <= 0) { free(bc); return send_err_json(req, 500, "bytecode too large"); }
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_send(req, (const char *)bc, n);
    free(bc);
    return ESP_OK;
}

// POST /api/js/validate    body: raw JS source
//   200  {"status":"ok"}
//   400  {"status":"error","message":"<reason>"}
//
// Lets clients pre-flight a script through the same `js_player_validate`
// pass that gatekeeps PUT /api/js/<name>, without committing it to NVS.
// Phase 21 — the "device is master" handshake: if validate rejects it,
// no client should save it or run it locally.
static esp_err_t validate_handler(httpd_req_t *req)
{
    if (pairing_http_check(req, PL_ROLE_CREATOR) != ESP_OK) return ESP_FAIL;
    char *body = NULL; size_t body_len = 0;
    esp_err_t err = receive_body(req, &body, &body_len);
    if (err != ESP_OK) return send_err_json(req, 400, "bad body");
    const char *verr = NULL;
    esp_err_t code = js_player_validate(body, &verr);
    free(body);
    if (code != ESP_OK) {
        // js_player_validate writes a short, ASCII-only reason; safe to embed
        // directly. Keep the buffer modest — we're only mirroring one phrase.
        char msg[160];
        snprintf(msg, sizeof(msg), "%s", verr ? verr : "validation failed");
        return send_err_json(req, 400, msg);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

// DELETE /api/js/<name>
static esp_err_t delete_handler(httpd_req_t *req)
{
    if (pairing_http_check(req, PL_ROLE_CREATOR) != ESP_OK) return ESP_FAIL;
    const char *name = name_from_uri(req->uri);
    if (!name) return send_err_json(req, 400, "missing name");
    esp_err_t err = js_storage_remove(name);
    if (err == ESP_ERR_NOT_FOUND) return send_err_json(req, 404, "not found");
    if (err != ESP_OK) return send_err_json(req, 500, "delete failed");
    /* Drop the .bc alongside the .js. Best-effort — missing .bc is fine. */
    js_storage_remove_bc(name);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

esp_err_t js_api_play(const char *name, int fps)
{
    if (!name || !name[0]) return ESP_ERR_INVALID_ARG;
    if (fps <= 0) fps = JS_DEFAULT_FPS;
    /* Phase 23 — verify the .bc exists before starting. Player reads .bc
     * directly so the source body isn't needed here. */
    if (!js_storage_exists(name)) return ESP_ERR_NOT_FOUND;
    void *probe = NULL; size_t probe_len = 0;
    esp_err_t err = js_storage_read_bc(name, &probe, &probe_len);
    if (err == ESP_ERR_NOT_FOUND) return ESP_ERR_NOT_FOUND;
    if (err != ESP_OK) return err;
    free(probe);
    /* Order matters — set_current_name must run before start so the player
     * task sees the right name when it loads the .bc. */
    js_player_set_current_name(name);
    err = js_player_start(NULL, fps);
    if (err != ESP_OK) return err;
    config_store_set_str(CONFIG_KEY_CURRENT_JS, name);
    return ESP_OK;
}

// POST /api/play  body: {"file":"sparkle","fps":10}
static esp_err_t play_handler(httpd_req_t *req)
{
    if (pairing_http_check(req, PL_ROLE_USER) != ESP_OK) return ESP_FAIL;
    char buf[192] = {0};
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) return send_err_json(req, 400, "no body");

    // Parse "file":"name"
    const char *p = strstr(buf, "\"file\"");
    if (!p) return send_err_json(req, 400, "file missing");
    p = strchr(p, ':');
    if (!p) return send_err_json(req, 400, "file missing");
    p = strchr(p, '"');
    if (!p) return send_err_json(req, 400, "file missing");
    p++;
    const char *end = strchr(p, '"');
    if (!end) return send_err_json(req, 400, "file missing");
    char name[64] = {0};
    size_t n = end - p;
    if (n >= sizeof(name)) return send_err_json(req, 400, "name too long");
    memcpy(name, p, n);

    int fps = JS_DEFAULT_FPS;
    const char *fp = strstr(buf, "\"fps\"");
    if (fp) {
        fp = strchr(fp, ':');
        if (fp) fps = atoi(fp + 1);
    }

    esp_err_t err = js_api_play(name, fps);
    if (err == ESP_ERR_NOT_FOUND) return send_err_json(req, 404, "script not found");
    if (err != ESP_OK) return send_err_json(req, 500, "play failed");

    httpd_resp_set_type(req, "application/json");
    char resp[160];
    snprintf(resp, sizeof(resp), "{\"status\":\"ok\",\"playing\":\"%s\",\"fps\":%d}", name, fps);
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

/// Move to the next or previous script in alphabetical order. Wraps around.
/// `direction` is +1 (next) or -1 (prev). Returns the chosen name (or empty
/// string + ESP_ERR_NOT_FOUND if the library is empty).
static esp_err_t advance_script(int direction, char *out_name, size_t out_len)
{
    char names[32][64];
    int n = js_storage_collect_sorted(names, 32);
    if (n == 0) return ESP_ERR_NOT_FOUND;

    const char *current = js_player_current_name();
    if (!current || !current[0]) {
        char persisted[64] = {0};
        config_get_str_or(CONFIG_KEY_CURRENT_JS, persisted, sizeof(persisted), "");
        current = persisted[0] ? persisted : NULL;
    }

    int idx = -1;
    if (current) {
        for (int i = 0; i < n; i++) {
            if (strcmp(names[i], current) == 0) { idx = i; break; }
        }
    }
    int next;
    if (idx < 0) {
        // Nothing playing → start from the first / last entry depending on direction.
        next = (direction > 0) ? 0 : (n - 1);
    } else {
        next = (idx + direction + n) % n;
    }
    snprintf(out_name, out_len, "%s", names[next]);
    return js_api_play(names[next], JS_DEFAULT_FPS);
}

esp_err_t js_api_play_next(char *out_name, size_t out_len)
{
    return advance_script(+1, out_name, out_len);
}

esp_err_t js_api_play_prev(char *out_name, size_t out_len)
{
    return advance_script(-1, out_name, out_len);
}

void js_api_stop(void)
{
    js_player_stop();
    js_player_set_current_name(NULL);
}

// POST /api/play/next  → advance one script alphabetically and play it.
static esp_err_t play_next_handler(httpd_req_t *req)
{
    if (pairing_http_check(req, PL_ROLE_USER) != ESP_OK) return ESP_FAIL;
    char chosen[64] = {0};
    esp_err_t err = js_api_play_next(chosen, sizeof(chosen));
    if (err == ESP_ERR_NOT_FOUND) return send_err_json(req, 404, "no scripts on device");
    if (err != ESP_OK) return send_err_json(req, 500, "advance failed");
    httpd_resp_set_type(req, "application/json");
    char resp[128];
    snprintf(resp, sizeof(resp), "{\"status\":\"ok\",\"playing\":\"%s\"}", chosen);
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

// POST /api/play/prev  → step one script back alphabetically and play it.
static esp_err_t play_prev_handler(httpd_req_t *req)
{
    if (pairing_http_check(req, PL_ROLE_USER) != ESP_OK) return ESP_FAIL;
    char chosen[64] = {0};
    esp_err_t err = js_api_play_prev(chosen, sizeof(chosen));
    if (err == ESP_ERR_NOT_FOUND) return send_err_json(req, 404, "no scripts on device");
    if (err != ESP_OK) return send_err_json(req, 500, "advance failed");
    httpd_resp_set_type(req, "application/json");
    char resp[128];
    snprintf(resp, sizeof(resp), "{\"status\":\"ok\",\"playing\":\"%s\"}", chosen);
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

// GET /api/play/current  → {"current":"name"|null,"playing":"name"|null}
//   "current" is the persisted CONFIG_KEY_CURRENT_JS (survives reboot).
//   "playing" is what's actually running right now (may differ if mode=api).
static esp_err_t play_current_handler(httpd_req_t *req)
{
    if (pairing_http_check(req, PL_ROLE_USER) != ESP_OK) return ESP_FAIL;
    char persisted[64] = {0};
    config_get_str_or(CONFIG_KEY_CURRENT_JS, persisted, sizeof(persisted), "");
    const char *playing = js_player_current_name();
    char resp[160];
    if (persisted[0] && playing) {
        snprintf(resp, sizeof(resp),
                 "{\"current\":\"%s\",\"playing\":\"%s\"}", persisted, playing);
    } else if (persisted[0]) {
        snprintf(resp, sizeof(resp),
                 "{\"current\":\"%s\",\"playing\":null}", persisted);
    } else if (playing) {
        snprintf(resp, sizeof(resp),
                 "{\"current\":null,\"playing\":\"%s\"}", playing);
    } else {
        snprintf(resp, sizeof(resp), "{\"current\":null,\"playing\":null}");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

// POST /api/stop
static esp_err_t stop_handler(httpd_req_t *req)
{
    if (pairing_http_check(req, PL_ROLE_USER) != ESP_OK) return ESP_FAIL;
    js_api_stop();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

// Phase 22 — POST /api/bench { "mode": "fill"|"fade", "fps": 120 }
// Pure-C render loop; reports its fps via /api/state.fps so we can compare
// directly to JS scripts.
static esp_err_t bench_handler(httpd_req_t *req)
{
    if (pairing_http_check(req, PL_ROLE_CREATOR) != ESP_OK) return ESP_FAIL;
    char buf[128] = {0};
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) return send_err_json(req, 400, "no body");
    int mode = 0;
    const char *m = strstr(buf, "\"mode\"");
    if (m) {
        m = strchr(m, '"');
        if (m) m = strchr(m + 1, '"');
        if (m) {
            if (strstr(m, "fade")) mode = 1;
        }
    }
    int fps = 120;
    const char *fp = strstr(buf, "\"fps\"");
    if (fp) { fp = strchr(fp, ':'); if (fp) fps = atoi(fp + 1); }
    esp_err_t err = js_player_start_cbench(mode, fps);
    if (err != ESP_OK) return send_err_json(req, 500, "bench start failed");
    httpd_resp_set_type(req, "application/json");
    char resp[80];
    snprintf(resp, sizeof(resp), "{\"status\":\"ok\",\"mode\":%d,\"fps\":%d}", mode, fps);
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

/// Register a single URI and complain loudly on failure — silent failures
/// (usually httpd_max_uri_handlers being too low) cost us hours of debugging
/// 404s on routes that "should" exist.
static void register_or_warn(httpd_handle_t server, const httpd_uri_t *u)
{
    esp_err_t err = httpd_register_uri_handler(server, u);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register %s %s failed: %s",
                 u->method == HTTP_GET ? "GET" :
                 u->method == HTTP_POST ? "POST" :
                 u->method == HTTP_PUT ? "PUT" :
                 u->method == HTTP_DELETE ? "DELETE" : "?",
                 u->uri, esp_err_to_name(err));
    }
}

esp_err_t js_api_register(httpd_handle_t server)
{
    httpd_uri_t list = {.uri="/api/js", .method=HTTP_GET, .handler=list_handler};
    register_or_warn(server, &list);

    // Phase 21 — validate is a sibling of read/write/delete; registered as
    // a specific POST so it doesn't collide with the wildcard GET/PUT/DELETE
    // handlers below (different HTTP methods, no ambiguity).
    httpd_uri_t validate = {.uri="/api/js/validate", .method=HTTP_POST, .handler=validate_handler};
    register_or_warn(server, &validate);

    httpd_uri_t read_one = {.uri="/api/js/*", .method=HTTP_GET, .handler=read_handler};
    register_or_warn(server, &read_one);

    httpd_uri_t write_one = {.uri="/api/js/*", .method=HTTP_PUT, .handler=write_handler};
    register_or_warn(server, &write_one);

    httpd_uri_t delete_one = {.uri="/api/js/*", .method=HTTP_DELETE, .handler=delete_handler};
    register_or_warn(server, &delete_one);

    httpd_uri_t play = {.uri="/api/play", .method=HTTP_POST, .handler=play_handler};
    register_or_warn(server, &play);

    httpd_uri_t play_next = {.uri="/api/play/next", .method=HTTP_POST, .handler=play_next_handler};
    register_or_warn(server, &play_next);

    httpd_uri_t play_prev = {.uri="/api/play/prev", .method=HTTP_POST, .handler=play_prev_handler};
    register_or_warn(server, &play_prev);

    httpd_uri_t play_current = {.uri="/api/play/current", .method=HTTP_GET, .handler=play_current_handler};
    register_or_warn(server, &play_current);

    httpd_uri_t stop = {.uri="/api/stop", .method=HTTP_POST, .handler=stop_handler};
    register_or_warn(server, &stop);

    httpd_uri_t bench = {.uri="/api/bench", .method=HTTP_POST, .handler=bench_handler};
    register_or_warn(server, &bench);

    httpd_uri_t compile = {.uri="/api/js/compile", .method=HTTP_POST, .handler=compile_handler};
    register_or_warn(server, &compile);

    ESP_LOGI(TAG, "JS API registered");
    return ESP_OK;
}
