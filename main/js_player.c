/* Phase 23 — PLBC-backed JS player. The previous JerryScript backend had
 * ~184 µs of overhead per Frame.setPixel call (~44k cycles to cross the
 * JS↔C boundary); on a 256-LED panel that capped fade at ~7 fps. The PLBC
 * VM dispatches one opcode in ~5–20 cycles. Per-pixel work for fade should
 * now run in <2 ms on this hardware.
 *
 * Lifecycle is simpler than before — there's no engine context with
 * heap-y init/cleanup. Each playback just loads the .bc file into a
 * stack-allocated plbc_program_t + an mallocs-once plbc_runtime_t, then
 * loops plbc_run_pixel.
 *
 * /api/js PUT (in js_api.c) calls plbc_compile and stores both <name>.js
 * and <name>.bc. /api/js/<name>/params PUT mutates the program's param
 * slots in-memory and re-serializes the .bc with the updated values.
 */

#include "js_player.h"
#include "js_api.h"
#include "led_control.h"
#include "js_storage.h"
#include "wormhole.h"
#include "plbc.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "js_player";

#define JS_TASK_STACK_SIZE  (8 * 1024)
#define JS_TASK_PRIORITY    5
#define RENDER_MAX_PIXELS   1024

static SemaphoreHandle_t s_lock = NULL;
static TaskHandle_t s_task = NULL;
static bool s_running = false;
static int s_fps = JS_DEFAULT_FPS;
static char s_current_name[64] = {0};
static char s_last_err[160] = {0};

/* Active program — loaded from .bc by player_task. Owned by the task. */
static plbc_program_t *s_active_prog = NULL;

/* Latest base color, fed into per-pixel shader as base.r/g/b. */
static uint8_t s_base_r = 128, s_base_g = 128, s_base_b = 128;

/* Phase 22 — fps timestamp ring (unchanged from JerryScript port). */
#define FPS_WINDOW_MS 5000u
#define FPS_RING_SIZE 512
static uint32_t s_fps_ring[FPS_RING_SIZE];
static uint16_t s_fps_ring_idx = 0;
static bool     s_fps_ring_full = false;

static void fps_tick(uint32_t now_ms)
{
    s_fps_ring[s_fps_ring_idx] = now_ms;
    s_fps_ring_idx++;
    if (s_fps_ring_idx >= FPS_RING_SIZE) {
        s_fps_ring_idx = 0;
        s_fps_ring_full = true;
    }
}

static void fps_reset(void)
{
    memset(s_fps_ring, 0, sizeof(s_fps_ring));
    s_fps_ring_idx = 0;
    s_fps_ring_full = false;
}

float js_player_get_fps(void)
{
    uint32_t now = esp_log_timestamp();
    uint32_t cutoff = now - FPS_WINDOW_MS;
    int count = 0;
    uint32_t oldest = now;
    int limit = s_fps_ring_full ? FPS_RING_SIZE : s_fps_ring_idx;
    for (int i = 0; i < limit; i++) {
        uint32_t t = s_fps_ring[i];
        if (t == 0) continue;
        if (t > now) continue;
        if (t < cutoff) continue;
        count++;
        if (t < oldest) oldest = t;
    }
    if (count < 2) return 0.0f;
    uint32_t span = now - oldest;
    if (span == 0) return 0.0f;
    return (float)count * 1000.0f / (float)span;
}

static void get_grid(int *w, int *h)
{
    int lw = led_control_get_logical_w();
    int lh = led_control_get_logical_h();
    *w = lw > 0 ? lw : led_control_get_count();
    *h = lh > 0 ? lh : 1;
}

/* Phase 29 — render geometry. For a wormhole lamp the render grid is
 * decoupled from the physical strip: the script renders a 24 x rings (strip)
 * or 24 x 1 (mirror) grid, then wormhole_expand() tiles it onto led_count
 * physical pixels. For every other form this is identical to get_grid(), so
 * the non-wormhole code path is byte-for-byte unchanged. */
static void get_render_grid(int *w, int *h, bool *is_wormhole)
{
    bool wh = wormhole_is_wormhole();
    if (is_wormhole) *is_wormhole = wh;
    if (wh) {
        *w = 24;
        *h = (wormhole_mode() == WORMHOLE_MODE_MIRROR) ? 1 : wormhole_rings();
    } else {
        get_grid(w, h);
    }
}

/* ------------------------------------------------------------------------
 * Player task
 * ------------------------------------------------------------------------ */

static void player_task(void *arg)
{
    (void) arg;
    ESP_LOGI(TAG, "Player task started for '%s'", s_current_name);

    /* Load the compiled bytecode from storage. */
    void *bc_buf = NULL;
    size_t bc_len = 0;
    if (js_storage_read_bc(s_current_name, &bc_buf, &bc_len) != ESP_OK) {
        snprintf(s_last_err, sizeof(s_last_err),
                 "no .bc for '%s' — re-PUT to recompile", s_current_name);
        ESP_LOGE(TAG, "%s", s_last_err);
        s_running = false; s_task = NULL; vTaskDelete(NULL);
        return;
    }

    plbc_program_t *prog = (plbc_program_t *)calloc(1, sizeof(*prog));
    if (!prog) {
        free(bc_buf);
        s_running = false; s_task = NULL; vTaskDelete(NULL);
        return;
    }
    char err[128] = {0};
    if (plbc_load(bc_buf, bc_len, prog, err, sizeof(err)) != ESP_OK) {
        snprintf(s_last_err, sizeof(s_last_err), "bytecode load: %s", err);
        ESP_LOGE(TAG, "%s", s_last_err);
        free(bc_buf); free(prog);
        s_running = false; s_task = NULL; vTaskDelete(NULL);
        return;
    }
    free(bc_buf);

    /* Render geometry — decoupled from the physical strip for a wormhole
     * lamp (Phase 29). For every other form `is_wormhole` is false and the
     * code path below is identical to before. */
    bool is_wormhole = false;
    int w, h;
    get_render_grid(&w, &h, &is_wormhole);
    int total = w * h;
    if (total > RENDER_MAX_PIXELS) total = RENDER_MAX_PIXELS;

    led_color_t *frame = (led_color_t *)calloc(total, sizeof(led_color_t));
    if (!frame) {
        free(prog);
        s_running = false; s_task = NULL; vTaskDelete(NULL);
        return;
    }

    /* For a wormhole lamp the rendered `frame` is a render buffer; it must be
     * tiled onto the physical strip via wormhole_expand() before going to
     * led_control. `phys` holds the led_count physical pixels. */
    int wh_rings = is_wormhole ? wormhole_rings() : 0;
    bool wh_mirror = is_wormhole && (wormhole_mode() == WORMHOLE_MODE_MIRROR);
    int phys_total = is_wormhole ? (wh_rings * 24) : 0;
    led_color_t *phys = NULL;
    if (is_wormhole) {
        phys = (led_color_t *)calloc(phys_total > 0 ? phys_total : 1,
                                     sizeof(led_color_t));
        if (!phys) {
            free(frame); free(prog);
            s_running = false; s_task = NULL; vTaskDelete(NULL);
            return;
        }
    }

    plbc_runtime_t rt;
    if (plbc_runtime_init(&rt, prog, w, h) != ESP_OK) {
        ESP_LOGE(TAG, "plbc_runtime_init failed");
        free(frame); free(prog);
        s_running = false; s_task = NULL; vTaskDelete(NULL);
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_active_prog = prog;
    rt.base_r = s_base_r;
    rt.base_g = s_base_g;
    rt.base_b = s_base_b;
    /* Per-playback seed for stateless scripts that want a fresh look every
     * run. Masked to 24 bits so it stays exact in float32 (the VM stack is
     * f32). Different every play because esp_log_timestamp is monotonic
     * and never resets without a reboot. */
    rt.play_start_ms = esp_log_timestamp() & 0xFFFFFFu;
    /* Full-precision start time for the `time` input — per-frame elapsed
     * ms is computed as `current - play_start_full_ms`. Keeping it
     * separate from the hash-seed `play_start_ms` lets the seed stay
     * 24-bit (exact in float32) while elapsed-time tracking uses the
     * full 32-bit range. */
    rt.play_start_full_ms = esp_log_timestamp();
    rt.now_ms = 0;
    xSemaphoreGive(s_lock);

    int fps = s_fps;
    TickType_t period = pdMS_TO_TICKS(1000 / (fps > 0 ? fps : 10));
    if (period == 0) period = 1;
    TickType_t last_wake = xTaskGetTickCount();
    uint32_t frame_idx = 0;
    int trap_count = 0;
    const int TRAP_LIMIT = 3;

    while (s_running) {
        rt.frame_idx = frame_idx;
        /* Snapshot wall-clock-elapsed-ms once per frame — all pixels see
         * the same `time` value, animations are smooth even if fps shifts. */
        rt.now_ms = esp_log_timestamp() - rt.play_start_full_ms;
        /* Refresh base color in case /api/color was called mid-playback. */
        rt.base_r = s_base_r;
        rt.base_g = s_base_g;
        rt.base_b = s_base_b;

        bool frame_ok = true;
        for (int i = 0; i < total; i++) {
            uint8_t r = 0, g = 0, b = 0;
            esp_err_t er = plbc_run_pixel(prog, &rt, i, &r, &g, &b);
            if (er != ESP_OK) {
                frame_ok = false;
                snprintf(s_last_err, sizeof(s_last_err),
                         "vm trap at pixel %d frame %u", i, (unsigned)frame_idx);
                ESP_LOGW(TAG, "%s", s_last_err);
                break;
            }
            frame[i].r = r; frame[i].g = g; frame[i].b = b;
        }

        if (frame_ok) {
            if (is_wormhole) {
                /* Tile the render buffer onto the physical strip — the SAME
                 * wormhole_expand() the WebSocket stream path uses. */
                wormhole_expand(frame, total, phys, wh_rings, wh_mirror);
                led_control_set_all(phys, phys_total);
            } else {
                led_control_set_logical(frame, w, h);
            }
            xSemaphoreTake(s_lock, portMAX_DELAY);
            fps_tick(esp_log_timestamp());
            xSemaphoreGive(s_lock);
        } else if (++trap_count >= TRAP_LIMIT) {
            ESP_LOGE(TAG, "Too many traps — stopping playback");
            break;
        }

        frame_idx++;
        vTaskDelayUntil(&last_wake, period);
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_active_prog = NULL;
    xSemaphoreGive(s_lock);

    plbc_runtime_free(&rt);
    free(frame);
    free(phys);
    free(prog);
    ESP_LOGI(TAG, "Player task stopped");
    s_running = false;
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t js_player_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    return s_lock ? ESP_OK : ESP_ERR_NO_MEM;
}

/* ------------------------------------------------------------------------
 * Validate — compiles to a throwaway program. Stateless; safe regardless
 * of whether a player is currently running (the VM has no shared state
 * between compile and run).
 * ------------------------------------------------------------------------ */

esp_err_t js_player_validate(const char *source, const char **err_out)
{
    if (!source) {
        if (err_out) *err_out = "empty source";
        return ESP_ERR_INVALID_ARG;
    }
    plbc_program_t *tmp = (plbc_program_t *)calloc(1, sizeof(*tmp));
    if (!tmp) {
        if (err_out) *err_out = "out of memory";
        return ESP_ERR_NO_MEM;
    }
    esp_err_t r = plbc_compile(source, strlen(source), tmp,
                               s_last_err, sizeof(s_last_err));
    free(tmp);
    if (r != ESP_OK) {
        if (err_out) *err_out = s_last_err;
        return ESP_FAIL;
    }
    if (err_out) *err_out = NULL;
    return ESP_OK;
}

esp_err_t js_player_start(const char *source, int fps)
{
    /* The `source` arg is the script *name* in the PLBC era — we already
     * have the bytecode in storage. Phase 22's start(source, fps) interface
     * stays so callers in js_api.c don't change shape, but we ignore the
     * raw source body and look up s_current_name's .bc instead. The caller
     * (js_api_play) is expected to set_current_name before invoking start. */
    (void) source;
    js_player_stop();

    if (!led_control_is_on()) led_control_enable();

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_fps = fps > 0 ? fps : JS_DEFAULT_FPS;
    fps_reset();
    s_running = true;
    xSemaphoreGive(s_lock);

    if (xTaskCreatePinnedToCore(player_task, "js_player", JS_TASK_STACK_SIZE,
                                NULL, JS_TASK_PRIORITY, &s_task, 1) != pdPASS) {
        s_running = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void js_player_stop(void)
{
    if (!s_running) return;
    s_running = false;
    for (int i = 0; i < 50 && s_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

bool js_player_is_running(void)
{
    return s_running;
}

const char *js_player_current_name(void)
{
    return s_current_name[0] ? s_current_name : NULL;
}

void js_player_set_current_name(const char *name)
{
    if (!name) { s_current_name[0] = 0; return; }
    snprintf(s_current_name, sizeof(s_current_name), "%s", name);
}

int js_player_apply_params_json(const char *json, size_t len)
{
    if (!json || s_current_name[0] == '\0') return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int updated = 0;
    if (s_active_prog) {
        updated = plbc_apply_params_json(s_active_prog, json, len);
    }
    xSemaphoreGive(s_lock);
    /* Persist by re-serializing the program to its .bc file. We do this
     * outside the lock so other tasks aren't blocked on flash writes. */
    if (updated > 0 && s_active_prog) {
        uint8_t *buf = (uint8_t *)malloc(8192);
        if (buf) {
            int n = plbc_serialize(s_active_prog, buf, 8192);
            if (n > 0) js_storage_write_bc(s_current_name, buf, (size_t)n);
            free(buf);
        }
    }
    return updated;
}

int js_player_dump_params_json(char *out, size_t max_len)
{
    if (!out || max_len == 0) return 0;
    int n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_active_prog) {
        n = plbc_params_to_json(s_active_prog, out, max_len);
    } else {
        /* Player idle — load the .bc from storage just for the schema. */
        void *buf = NULL; size_t buf_len = 0;
        if (s_current_name[0] && js_storage_read_bc(s_current_name, &buf, &buf_len) == ESP_OK) {
            plbc_program_t *p = (plbc_program_t *)calloc(1, sizeof(*p));
            if (p && plbc_load(buf, buf_len, p, NULL, 0) == ESP_OK) {
                n = plbc_params_to_json(p, out, max_len);
            }
            free(p);
            free(buf);
        }
    }
    xSemaphoreGive(s_lock);
    return n > 0 ? n : 0;
}

void js_player_set_base_color(uint8_t r, uint8_t g, uint8_t b)
{
    s_base_r = r;
    s_base_g = g;
    s_base_b = b;
}

void js_player_get_base_color(uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (r) *r = s_base_r;
    if (g) *g = s_base_g;
    if (b) *b = s_base_b;
}

/* ------------------------------------------------------------------------
 * Diagnostic C-bench mode (Phase 22 — unchanged). Runs a pure-C render
 * loop with no VM involvement.
 * ------------------------------------------------------------------------ */

static void c_bench_fill_solid(led_color_t *buf, int total, uint32_t frame_idx)
{
    (void) frame_idx;
    uint8_t r = s_base_r, g = s_base_g, b = s_base_b;
    for (int i = 0; i < total; i++) { buf[i].r = r; buf[i].g = g; buf[i].b = b; }
}

static void c_bench_fade(led_color_t *buf, int total, uint32_t frame_idx)
{
    double dt = 1000.0 / 10.0;
    double now = (double)frame_idx * dt;
    double fi = 400.0, hd = 200.0, fo = 800.0, density = 0.75;
    double active = fi + hd + fo;
    double cycle = active / density;
    double holdEnd = fi + hd;
    for (int i = 0; i < total; i++) {
        double prod = (double)i * 2654435761.0;
        double offset = fmod(prod, cycle);
        double t = fmod(now + offset, cycle);
        if (t < 0) t += cycle;
        double bright = 0;
        if (t < fi) bright = t / fi;
        else if (t < holdEnd) bright = 1.0;
        else if (t < active) bright = 1.0 - (t - holdEnd) / fo;
        if (bright < 0) bright = 0; else if (bright > 1) bright = 1;
        buf[i].r = (uint8_t)(s_base_r * bright);
        buf[i].g = (uint8_t)(s_base_g * bright);
        buf[i].b = (uint8_t)(s_base_b * bright);
    }
}

static void c_bench_task(void *arg)
{
    int mode = (int)(intptr_t)arg;
    int w, h;
    get_grid(&w, &h);
    int total = w * h;
    if (total > RENDER_MAX_PIXELS) total = RENDER_MAX_PIXELS;
    led_color_t *frame = (led_color_t *)calloc(total, sizeof(led_color_t));
    if (!frame) { s_running = false; s_task = NULL; vTaskDelete(NULL); return; }
    int fps = s_fps;
    TickType_t period = pdMS_TO_TICKS(1000 / (fps > 0 ? fps : 10));
    if (period == 0) period = 1;
    TickType_t last_wake = xTaskGetTickCount();
    uint32_t frame_idx = 0;
    while (s_running) {
        if (mode == 0) c_bench_fill_solid(frame, total, frame_idx);
        else           c_bench_fade(frame, total, frame_idx);
        led_control_set_logical(frame, w, h);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        fps_tick(esp_log_timestamp());
        xSemaphoreGive(s_lock);
        frame_idx++;
        vTaskDelayUntil(&last_wake, period);
    }
    free(frame);
    s_running = false; s_task = NULL; vTaskDelete(NULL);
}

esp_err_t js_player_start_cbench(int mode, int fps)
{
    js_player_stop();
    if (!led_control_is_on()) led_control_enable();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_fps = fps > 0 ? fps : JS_DEFAULT_FPS;
    fps_reset();
    s_running = true;
    xSemaphoreGive(s_lock);
    snprintf(s_current_name, sizeof(s_current_name), "__cbench_%d", mode);
    if (xTaskCreatePinnedToCore(c_bench_task, "c_bench", 8192,
                                (void *)(intptr_t)mode,
                                JS_TASK_PRIORITY, &s_task, 1) != pdPASS) {
        s_running = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
