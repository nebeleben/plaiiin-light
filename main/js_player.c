#include "js_player.h"
#include "led_control.h"
#include "mjs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_random.h"

static const char *TAG = "js_player";

#define JS_TASK_STACK_SIZE  (16 * 1024)
#define JS_TASK_PRIORITY    5
#define RENDER_MAX_PIXELS   1024

/**
 * mJS only understands `let` (not `var` or `const`). Rewrite in place so
 * AI-generated snippets — which overwhelmingly use `var`/`const` — can run.
 * Replacement is length-preserving ("var" → "let", "const" → space+"let")
 * so string offsets stay identical. Only matches when bounded by non-word
 * characters, so identifiers like `variable` are left alone.
 */
static void translate_var_const_to_let(char *s)
{
    if (!s) return;
    size_t len = strlen(s);
    for (size_t i = 0; i < len; i++) {
        int before_ok = (i == 0) || !(isalnum((unsigned char)s[i-1]) || s[i-1] == '_' || s[i-1] == '$');
        if (!before_ok) continue;
        // "var"
        if (i + 3 <= len && s[i] == 'v' && s[i+1] == 'a' && s[i+2] == 'r') {
            char after = (i + 3 < len) ? s[i+3] : 0;
            if (!(isalnum((unsigned char)after) || after == '_' || after == '$')) {
                s[i] = 'l'; s[i+1] = 'e'; s[i+2] = 't';
                i += 2;
                continue;
            }
        }
        // "const" — "const" is 5 chars, "let" is 3, so we keep it length-preserving
        // by padding with two leading spaces: "  let"
        if (i + 5 <= len && s[i] == 'c' && s[i+1] == 'o' && s[i+2] == 'n' && s[i+3] == 's' && s[i+4] == 't') {
            char after = (i + 5 < len) ? s[i+5] : 0;
            if (!(isalnum((unsigned char)after) || after == '_' || after == '$')) {
                s[i] = ' '; s[i+1] = ' '; s[i+2] = 'l'; s[i+3] = 'e'; s[i+4] = 't';
                i += 4;
                continue;
            }
        }
    }
}

/* Forward declaration — install_math is defined below but used by player_task. */
static void install_math(struct mjs *mjs);

static SemaphoreHandle_t s_lock = NULL;
static TaskHandle_t s_task = NULL;
static bool s_running = false;
static int s_fps = 10;
static char *s_source = NULL;
static char s_current_name[64] = {0};
static char s_last_err[128] = {0};

/**
 * Logical grid: width/height derived from lamp_type when it's a matrix,
 * otherwise treat the strip as 1 row of N.
 */
static void get_grid(int *w, int *h)
{
    int lw = led_control_get_logical_w();
    int lh = led_control_get_logical_h();
    *w = lw > 0 ? lw : led_control_get_count();
    *h = lh > 0 ? lh : 1;
}

/** Pull the result array out of a mjs value into a led_color_t buffer. */
static int extract_frame(struct mjs *mjs, mjs_val_t result, led_color_t *out, int max)
{
    if (!mjs_is_array(result)) return -1;
    int n = (int)mjs_array_length(mjs, result);
    if (n > max) n = max;
    for (int i = 0; i < n; i++) {
        mjs_val_t px = mjs_array_get(mjs, result, i);
        if (!mjs_is_array(px)) { out[i].r = out[i].g = out[i].b = 0; continue; }
        int r = (int)mjs_get_double(mjs, mjs_array_get(mjs, px, 0));
        int g = (int)mjs_get_double(mjs, mjs_array_get(mjs, px, 1));
        int b = (int)mjs_get_double(mjs, mjs_array_get(mjs, px, 2));
        if (r < 0) r = 0; else if (r > 255) r = 255;
        if (g < 0) g = 0; else if (g > 255) g = 255;
        if (b < 0) b = 0; else if (b > 255) b = 255;
        out[i].r = (uint8_t)r;
        out[i].g = (uint8_t)g;
        out[i].b = (uint8_t)b;
    }
    return n;
}

/** Play loop: re-init mjs once, call render() each frame. */
static void player_task(void *arg)
{
    ESP_LOGI(TAG, "Player task started");
    char *source_copy = NULL;
    int fps = 10;
    int w = 1, h = 1;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_source) source_copy = strdup(s_source);
    fps = s_fps;
    get_grid(&w, &h);
    xSemaphoreGive(s_lock);

    if (!source_copy) {
        s_running = false;
        vTaskDelete(NULL);
        return;
    }
    translate_var_const_to_let(source_copy);

    struct mjs *mjs = mjs_create();
    install_math(mjs);
    mjs_err_t err = mjs_exec(mjs, source_copy, NULL);
    if (err != MJS_OK) {
        const char *msg = mjs_strerror(mjs, err);
        ESP_LOGE(TAG, "JS exec failed: %s", msg ? msg : "?");
        snprintf(s_last_err, sizeof(s_last_err), "%s", msg ? msg : "exec failed");
        mjs_destroy(mjs);
        free(source_copy);
        s_running = false;
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    mjs_val_t render_fn = mjs_get(mjs, mjs_get_global(mjs), "render", ~0);
    if (!mjs_is_function(render_fn)) {
        ESP_LOGE(TAG, "JS has no render() function");
        snprintf(s_last_err, sizeof(s_last_err), "no render() function");
        mjs_destroy(mjs);
        free(source_copy);
        s_running = false;
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    int total = w * h;
    if (total > RENDER_MAX_PIXELS) total = RENDER_MAX_PIXELS;
    led_color_t *frame = (led_color_t *)calloc(total, sizeof(led_color_t));
    if (!frame) {
        ESP_LOGE(TAG, "OOM allocating frame buffer");
        mjs_destroy(mjs);
        free(source_copy);
        s_running = false;
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    uint32_t frame_idx = 0;
    TickType_t period = pdMS_TO_TICKS(1000 / (fps > 0 ? fps : 10));
    if (period == 0) period = 1;

    while (s_running) {
        mjs_val_t result;
        mjs_val_t args[3] = {
            mjs_mk_number(mjs, frame_idx),
            mjs_mk_number(mjs, w),
            mjs_mk_number(mjs, h)
        };
        mjs_err_t rerr = mjs_apply(mjs, &result, render_fn, mjs_mk_null(), 3, args);
        if (rerr != MJS_OK) {
            const char *msg = mjs_strerror(mjs, rerr);
            ESP_LOGW(TAG, "render() error: %s", msg ? msg : "?");
            snprintf(s_last_err, sizeof(s_last_err), "render error: %s", msg ? msg : "?");
            break;
        }
        int n = extract_frame(mjs, result, frame, total);
        if (n > 0) {
            // Logical → physical (pixel grouping expansion happens here).
            led_control_set_logical(frame, w, h);
        }
        frame_idx++;
        vTaskDelay(period);
    }

    free(frame);
    mjs_destroy(mjs);
    free(source_copy);
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
 * Math polyfill — mJS ships no Math object. We expose the functions the
 * LED render scripts actually use. Each C function reads args via mjs_arg(),
 * returns via mjs_return().
 * ------------------------------------------------------------------------ */

static double arg_num(struct mjs *mjs, int i)
{
    mjs_val_t v = mjs_arg(mjs, i);
    if (mjs_is_number(v)) return mjs_get_double(mjs, v);
    return 0.0;
}

static void math_abs(struct mjs *mjs)   { mjs_return(mjs, mjs_mk_number(mjs, fabs(arg_num(mjs, 0)))); }
static void math_floor(struct mjs *mjs) { mjs_return(mjs, mjs_mk_number(mjs, floor(arg_num(mjs, 0)))); }
static void math_ceil(struct mjs *mjs)  { mjs_return(mjs, mjs_mk_number(mjs, ceil(arg_num(mjs, 0)))); }
static void math_round(struct mjs *mjs) { mjs_return(mjs, mjs_mk_number(mjs, round(arg_num(mjs, 0)))); }
static void math_sqrt(struct mjs *mjs)  { mjs_return(mjs, mjs_mk_number(mjs, sqrt(arg_num(mjs, 0)))); }
static void math_sin(struct mjs *mjs)   { mjs_return(mjs, mjs_mk_number(mjs, sin(arg_num(mjs, 0)))); }
static void math_cos(struct mjs *mjs)   { mjs_return(mjs, mjs_mk_number(mjs, cos(arg_num(mjs, 0)))); }
static void math_tan(struct mjs *mjs)   { mjs_return(mjs, mjs_mk_number(mjs, tan(arg_num(mjs, 0)))); }
static void math_atan(struct mjs *mjs)  { mjs_return(mjs, mjs_mk_number(mjs, atan(arg_num(mjs, 0)))); }
static void math_atan2(struct mjs *mjs) { mjs_return(mjs, mjs_mk_number(mjs, atan2(arg_num(mjs, 0), arg_num(mjs, 1)))); }
static void math_exp(struct mjs *mjs)   { mjs_return(mjs, mjs_mk_number(mjs, exp(arg_num(mjs, 0)))); }
static void math_log(struct mjs *mjs)   { mjs_return(mjs, mjs_mk_number(mjs, log(arg_num(mjs, 0)))); }
static void math_pow(struct mjs *mjs)   { mjs_return(mjs, mjs_mk_number(mjs, pow(arg_num(mjs, 0), arg_num(mjs, 1)))); }
static void math_random(struct mjs *mjs){
    uint32_t r = esp_random();
    mjs_return(mjs, mjs_mk_number(mjs, (double)r / 4294967295.0));
}
static void math_max(struct mjs *mjs)
{
    int n = mjs_nargs(mjs);
    if (n == 0) { mjs_return(mjs, mjs_mk_number(mjs, -INFINITY)); return; }
    double best = arg_num(mjs, 0);
    for (int i = 1; i < n; i++) { double v = arg_num(mjs, i); if (v > best) best = v; }
    mjs_return(mjs, mjs_mk_number(mjs, best));
}
static void math_min(struct mjs *mjs)
{
    int n = mjs_nargs(mjs);
    if (n == 0) { mjs_return(mjs, mjs_mk_number(mjs, INFINITY)); return; }
    double best = arg_num(mjs, 0);
    for (int i = 1; i < n; i++) { double v = arg_num(mjs, i); if (v < best) best = v; }
    mjs_return(mjs, mjs_mk_number(mjs, best));
}
static void math_sign(struct mjs *mjs)
{
    double x = arg_num(mjs, 0);
    mjs_return(mjs, mjs_mk_number(mjs, (x > 0) - (x < 0)));
}

static void install_math(struct mjs *mjs)
{
    mjs_val_t m = mjs_mk_object(mjs);
    mjs_set(mjs, m, "PI", ~0, mjs_mk_number(mjs, M_PI));
    mjs_set(mjs, m, "E",  ~0, mjs_mk_number(mjs, M_E));
    mjs_set(mjs, m, "abs",    ~0, mjs_mk_foreign_func(mjs, (mjs_func_ptr_t)math_abs));
    mjs_set(mjs, m, "floor",  ~0, mjs_mk_foreign_func(mjs, (mjs_func_ptr_t)math_floor));
    mjs_set(mjs, m, "ceil",   ~0, mjs_mk_foreign_func(mjs, (mjs_func_ptr_t)math_ceil));
    mjs_set(mjs, m, "round",  ~0, mjs_mk_foreign_func(mjs, (mjs_func_ptr_t)math_round));
    mjs_set(mjs, m, "sqrt",   ~0, mjs_mk_foreign_func(mjs, (mjs_func_ptr_t)math_sqrt));
    mjs_set(mjs, m, "sin",    ~0, mjs_mk_foreign_func(mjs, (mjs_func_ptr_t)math_sin));
    mjs_set(mjs, m, "cos",    ~0, mjs_mk_foreign_func(mjs, (mjs_func_ptr_t)math_cos));
    mjs_set(mjs, m, "tan",    ~0, mjs_mk_foreign_func(mjs, (mjs_func_ptr_t)math_tan));
    mjs_set(mjs, m, "atan",   ~0, mjs_mk_foreign_func(mjs, (mjs_func_ptr_t)math_atan));
    mjs_set(mjs, m, "atan2",  ~0, mjs_mk_foreign_func(mjs, (mjs_func_ptr_t)math_atan2));
    mjs_set(mjs, m, "exp",    ~0, mjs_mk_foreign_func(mjs, (mjs_func_ptr_t)math_exp));
    mjs_set(mjs, m, "log",    ~0, mjs_mk_foreign_func(mjs, (mjs_func_ptr_t)math_log));
    mjs_set(mjs, m, "pow",    ~0, mjs_mk_foreign_func(mjs, (mjs_func_ptr_t)math_pow));
    mjs_set(mjs, m, "random", ~0, mjs_mk_foreign_func(mjs, (mjs_func_ptr_t)math_random));
    mjs_set(mjs, m, "max",    ~0, mjs_mk_foreign_func(mjs, (mjs_func_ptr_t)math_max));
    mjs_set(mjs, m, "min",    ~0, mjs_mk_foreign_func(mjs, (mjs_func_ptr_t)math_min));
    mjs_set(mjs, m, "sign",   ~0, mjs_mk_foreign_func(mjs, (mjs_func_ptr_t)math_sign));
    mjs_set(mjs, mjs_get_global(mjs), "Math", ~0, m);
}

/** Copy a transient mjs_strerror string into s_last_err so it survives mjs_destroy. */
static const char *stash_err(struct mjs *mjs, mjs_err_t err)
{
    const char *msg = mjs_strerror(mjs, err);
    if (!msg) msg = "unknown mjs error";
    snprintf(s_last_err, sizeof(s_last_err), "%s", msg);
    return s_last_err;
}

esp_err_t js_player_validate(const char *source, const char **err_out)
{
    if (!source) {
        if (err_out) *err_out = "empty source";
        return ESP_ERR_INVALID_ARG;
    }
    char *scratch = strdup(source);
    if (!scratch) {
        if (err_out) *err_out = "out of memory";
        return ESP_ERR_NO_MEM;
    }
    translate_var_const_to_let(scratch);

    struct mjs *mjs = mjs_create();
    install_math(mjs);
    mjs_err_t err = mjs_exec(mjs, scratch, NULL);
    if (err != MJS_OK) {
        if (err_out) *err_out = stash_err(mjs, err);
        mjs_destroy(mjs);
        free(scratch);
        return ESP_FAIL;
    }
    mjs_val_t render_fn = mjs_get(mjs, mjs_get_global(mjs), "render", ~0);
    if (!mjs_is_function(render_fn)) {
        if (err_out) *err_out = "no render() function";
        mjs_destroy(mjs);
        free(scratch);
        return ESP_FAIL;
    }
    int w, h;
    get_grid(&w, &h);
    mjs_val_t result;
    mjs_val_t args[3] = {
        mjs_mk_number(mjs, 0),
        mjs_mk_number(mjs, w),
        mjs_mk_number(mjs, h)
    };
    mjs_err_t rerr = mjs_apply(mjs, &result, render_fn, mjs_mk_null(), 3, args);
    if (rerr != MJS_OK) {
        if (err_out) *err_out = stash_err(mjs, rerr);
        mjs_destroy(mjs);
        free(scratch);
        return ESP_FAIL;
    }
    if (!mjs_is_array(result)) {
        if (err_out) *err_out = "render() must return an array";
        mjs_destroy(mjs);
        free(scratch);
        return ESP_FAIL;
    }
    mjs_destroy(mjs);
    free(scratch);
    if (err_out) *err_out = NULL;
    return ESP_OK;
}

esp_err_t js_player_start(const char *source, int fps)
{
    if (!source) return ESP_ERR_INVALID_ARG;
    js_player_stop();

    // Playback writes through led_control_set_all, which short-circuits when
    // power is off. Auto-enable so hitting "Play" just works.
    if (!led_control_is_on()) led_control_enable();

    xSemaphoreTake(s_lock, portMAX_DELAY);
    free(s_source);
    s_source = strdup(source);
    s_fps = fps > 0 ? fps : 10;
    s_running = true;
    xSemaphoreGive(s_lock);

    if (xTaskCreate(player_task, "js_player", JS_TASK_STACK_SIZE, NULL,
                    JS_TASK_PRIORITY, &s_task) != pdPASS) {
        s_running = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void js_player_stop(void)
{
    if (!s_running) return;
    s_running = false;
    // Let the task finish its current frame.
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
