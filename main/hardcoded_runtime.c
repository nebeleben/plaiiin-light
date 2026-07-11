/*
 * Phase 35 — runtime that plays a hardcoded_effect_t.
 *
 * Mirrors the surface of js_player.c (start/stop/is_running/current_name/fps)
 * so js_api can dispatch by name (hardcoded first, PLBC fallback) without
 * caring which kind of effect is live.
 *
 * Render path is much simpler than PLBC's per-pixel VM: the effect fills the
 * whole w*h frame in one call. We still respect wormhole-mode geometry (the
 * effect renders into the logical w*h, then wormhole_expand() tiles it onto
 * the physical strip, exactly like js_player does for PLBC).
 *
 * Only one effect runs at a time, and only one player task across the whole
 * firmware (hardcoded OR PLBC) — js_api.c kills whichever one is live before
 * starting a new playback in either runtime.
 */

#include "hardcoded_effects.h"
#include "config_store.h"
#include "led_control.h"
#include "wormhole.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>

// Render task core affinity — see js_player.c. Dual-core (ESP32) pins to core 1
// to stay off the WiFi/BT core; single-core (C3) has only core 0.
#if CONFIG_FREERTOS_UNICORE
#define PLAIIIN_RENDER_CORE 0
#else
#define PLAIIIN_RENDER_CORE 1
#endif

static const char *TAG = "hc_runtime";

#define HC_TASK_PRIORITY     5
#define HC_TASK_STACK_BYTES  4096
/* Upper bound on logical render-grid pixels. Cube uses the chain-direct path
 * (one LED per cell) so its limit scales with led_count, not this constant. */
#define HC_RENDER_MAX_PIXELS 1024
#define HC_FPS_WINDOW_MS     5000
#define HC_FPS_WINDOW_SLOTS  256

static SemaphoreHandle_t s_lock = NULL;
static TaskHandle_t      s_task = NULL;
static volatile bool     s_running = false;
static const hardcoded_effect_t *s_eff = NULL;
static int               s_fps_target = 0;
static char              s_current_name[64] = {0};

/* Rolling fps window — same shape as js_player's. */
static uint32_t s_fps_ring[HC_FPS_WINDOW_SLOTS];
static size_t   s_fps_head = 0;
static size_t   s_fps_count = 0;

static void fps_tick(uint32_t now_ms)
{
    s_fps_ring[s_fps_head] = now_ms;
    s_fps_head = (s_fps_head + 1) % HC_FPS_WINDOW_SLOTS;
    if (s_fps_count < HC_FPS_WINDOW_SLOTS) s_fps_count++;
}

static float fps_now(uint32_t now_ms)
{
    if (s_fps_count == 0) return 0.f;
    /* Count entries within HC_FPS_WINDOW_MS of now. */
    int kept = 0;
    for (size_t i = 0; i < s_fps_count; i++) {
        size_t idx = (s_fps_head + HC_FPS_WINDOW_SLOTS - 1 - i) % HC_FPS_WINDOW_SLOTS;
        if (now_ms - s_fps_ring[idx] > HC_FPS_WINDOW_MS) break;
        kept++;
    }
    if (kept < 2) return 0.f;
    size_t newest = (s_fps_head + HC_FPS_WINDOW_SLOTS - 1) % HC_FPS_WINDOW_SLOTS;
    size_t oldest = (s_fps_head + HC_FPS_WINDOW_SLOTS - kept) % HC_FPS_WINDOW_SLOTS;
    uint32_t span = s_fps_ring[newest] - s_fps_ring[oldest];
    if (span == 0) return 0.f;
    return (float)(kept - 1) * 1000.f / (float)span;
}

/* Geometry — duplicated from js_player.c (small, no benefit in factoring out
 * for one extra caller; if a third arrives, lift it into led_control). */
static void get_grid(int *w, int *h)
{
    int lw = led_control_get_logical_w();
    int lh = led_control_get_logical_h();
    *w = lw > 0 ? lw : led_control_get_count();
    *h = lh > 0 ? lh : 1;
}

/* The cube wires 5 panels (front/right/back/left/top) back-to-back into one
 * 320-LED chain, while led_control_set_logical only writes a single panel.
 * For cube effects we therefore allocate a chain-sized buffer, pass it as
 * (w=led_count, h=1), and bypass set_logical with led_control_set_all. The
 * effect uses the form-prompt idx convention (face = idx/64) to draw across
 * faces. Tower and other matrix forms keep the per-panel logical-grid path. */
static bool is_cube_form(void)
{
    char lamp_form[32];
    config_get_str_or(CONFIG_KEY_LAMP_FORM, lamp_form, sizeof(lamp_form),
                      CONFIG_PLAIIIN_FORM);
    return strcmp(lamp_form, "cube") == 0;
}

static void get_render_grid(int *w, int *h, bool *is_wormhole, bool *is_cube)
{
    bool wh = wormhole_is_wormhole();
    bool cu = !wh && is_cube_form();
    if (is_wormhole) *is_wormhole = wh;
    if (is_cube) *is_cube = cu;
    if (wh) {
        *w = (wormhole_mode() == WORMHOLE_MODE_MIRROR) ? 24 : (24 * wormhole_rings());
        *h = 1;
    } else if (cu) {
        *w = led_control_get_count();
        *h = 1;
    } else {
        get_grid(w, h);
    }
}

static void hc_player_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Hardcoded player started for '%s'", s_current_name);

    bool is_wormhole = false, is_cube = false;
    int w, h;
    get_render_grid(&w, &h, &is_wormhole, &is_cube);
    int total = w * h;
    /* Cube uses chain-space directly (one LED per cell), so its buffer must
     * scale with led_count and not be clipped by HC_RENDER_MAX_PIXELS. */
    if (!is_cube && total > HC_RENDER_MAX_PIXELS) total = HC_RENDER_MAX_PIXELS;

    led_color_t *frame = (led_color_t *)calloc(total, sizeof(led_color_t));
    int wh_rings = is_wormhole ? wormhole_rings() : 0;
    bool wh_mirror = is_wormhole && (wormhole_mode() == WORMHOLE_MODE_MIRROR);
    int phys_total = is_wormhole ? (wh_rings * 24) : 0;
    led_color_t *phys = NULL;
    if (is_wormhole) {
        phys = (led_color_t *)calloc(phys_total > 0 ? phys_total : 1,
                                     sizeof(led_color_t));
    }
    if (!frame || (is_wormhole && !phys)) {
        ESP_LOGE(TAG, "Failed to allocate frame buffers");
        free(frame); free(phys);
        s_running = false; s_task = NULL; vTaskDelete(NULL);
        return;
    }

    if (s_eff->init) {
        esp_err_t err = s_eff->init(w, h);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Effect init failed: %s", esp_err_to_name(err));
            free(frame); free(phys);
            s_running = false; s_task = NULL; vTaskDelete(NULL);
            return;
        }
    }

    int fps = s_fps_target > 0 ? s_fps_target : s_eff->default_fps;
    if (fps <= 0) fps = 30;
    TickType_t period = pdMS_TO_TICKS(1000 / fps);
    if (period == 0) period = 1;
    TickType_t last_wake = xTaskGetTickCount();

    uint32_t play_start_ms = esp_log_timestamp();

    while (s_running) {
        uint32_t now_full = esp_log_timestamp();
        uint32_t elapsed  = now_full - play_start_ms;

        memset(frame, 0, total * sizeof(led_color_t));
        esp_err_t er = s_eff->render_frame(w, h, frame, elapsed);
        if (er == ESP_OK) {
            if (is_wormhole) {
                wormhole_expand(frame, total, phys, wh_rings, wh_mirror);
                led_control_set_all(phys, phys_total);
            } else if (is_cube) {
                led_control_set_all(frame, total);
            } else {
                led_control_set_logical(frame, w, h);
            }
            xSemaphoreTake(s_lock, portMAX_DELAY);
            fps_tick(now_full);
            xSemaphoreGive(s_lock);
        } else {
            ESP_LOGW(TAG, "render_frame returned %s — stopping",
                     esp_err_to_name(er));
            break;
        }

        vTaskDelayUntil(&last_wake, period);
    }

    if (s_eff && s_eff->deinit) s_eff->deinit();

    free(frame); free(phys);
    s_eff = NULL;
    s_current_name[0] = '\0';
    s_running = false;
    s_task = NULL;
    ESP_LOGI(TAG, "Hardcoded player stopped");
    vTaskDelete(NULL);
}

esp_err_t hardcoded_runtime_start(const hardcoded_effect_t *eff, int fps)
{
    if (!eff) return ESP_ERR_INVALID_ARG;
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) return ESP_ERR_NO_MEM;
    }
    hardcoded_runtime_stop();   /* idempotent */

    s_eff = eff;
    s_fps_target = fps > 0 ? fps : eff->default_fps;
    snprintf(s_current_name, sizeof(s_current_name), "%s", eff->name);
    s_fps_head = 0;
    s_fps_count = 0;
    s_running = true;

    if (xTaskCreatePinnedToCore(hc_player_task, "hc_player",
                                HC_TASK_STACK_BYTES, NULL,
                                HC_TASK_PRIORITY, &s_task, PLAIIIN_RENDER_CORE) != pdPASS) {
        s_running = false;
        s_eff = NULL;
        s_current_name[0] = '\0';
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void hardcoded_runtime_stop(void)
{
    if (!s_running) return;
    s_running = false;
    /* Wait briefly for the task to exit so a follow-up start() doesn't race. */
    for (int i = 0; i < 50 && s_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

bool hardcoded_runtime_is_running(void)
{
    return s_running;
}

const char *hardcoded_runtime_current_name(void)
{
    return s_running ? s_current_name : NULL;
}

float hardcoded_runtime_get_fps(void)
{
    if (!s_lock) return 0.f;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    float fps = fps_now(esp_log_timestamp());
    xSemaphoreGive(s_lock);
    return fps;
}
