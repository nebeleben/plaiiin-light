#include "led_control.h"
#include "config_store.h"
#include "led_strip.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "led_control";

// Driver backend: one-wire LEDs use led_strip (SPI-backed), two-wire use raw SPI.
typedef enum {
    LED_BACKEND_STRIP = 0,  // ws2812 / sk6812
    LED_BACKEND_APA102      // sk9822 (APA102-compatible 2-wire protocol)
} led_backend_t;

static led_backend_t s_backend = LED_BACKEND_STRIP;
static char s_led_type[16] = "ws2812";
static bool s_rgbw = false;  // true when the one-wire strip expects 4 bytes per LED (SK6812W)

// One-wire (led_strip) handle
static led_strip_handle_t s_strip = NULL;

// Two-wire (APA102/SK9822) SPI state
static spi_device_handle_t s_spi = NULL;
static uint8_t *s_apa_buf = NULL;      // pre-built frame buffer (start + per-LED + end)
static size_t   s_apa_buf_len = 0;
static bool s_power_on = false;
static int s_led_count = 0;
static led_color_t s_last_color = {0, 0, 0};
static led_color_t *s_frame_buffer = NULL;
static bool s_has_frame_buffer = false;
static uint8_t s_brightness = 255;
static uint8_t s_max_brightness = 255;
// AP-mode (or other transient) cap. 0 = no override. When set, the smaller of
// (s_brightness, s_brightness_override) wins. Never persisted.
static uint8_t s_brightness_override = 0;
static uint32_t s_max_current_ma = 0; // 0 = unlimited
static int s_phys_w = 0;
static int s_phys_h = 0;
static int s_px_group_w = 1;
static int s_px_group_h = 1;
static int  s_rotation = 0;       // 0 / 90 / 180 / 270
static int  s_origin   = 0;       // 0 TL, 1 TR, 2 BL, 3 BR
static bool s_serpentine = true;
static int  s_serp_axis  = 0;     // 0 horizontal, 1 vertical

#define LED_CHANNEL_MA_AT_255  20  // WS2812 per-channel peak at 255

#define NVS_KEY_LAST_COLOR  "last_color"
#define NVS_KEY_BRIGHTNESS  "brightness"
#define NVS_KEY_MAX_BRIGHT  "max_bright"
#define NVS_KEY_MAX_CURR_MA "max_curr_ma"
#define NVS_KEY_PX_GROUP_W  "px_group_w"
#define NVS_KEY_PX_GROUP_H  "px_group_h"
#define NVS_KEY_FADE_ON_MS  "fade_on_ms"
#define NVS_KEY_FADE_OFF_MS "fade_off_ms"

// --- On/off fade animation ---------------------------------------------------
// A multiplicative scale (Q16 fixed-point, 0..65535 = 0..1) is folded into the
// final RGB just before the WS2812/APA102 backend write, so JS-mode frames and
// API-mode static colours dim through the same path. The user-configured
// brightness (s_brightness) is never touched — only this output multiplier is.
//
// State machine, driven by led_control_power():
//   on  : flip s_power_on=true, arm dir=+1, fade_scale ramps current→FULL
//   off : flip s_power_on=false IMMEDIATELY (so /api/state and BLE read off),
//         arm dir=-1, set s_painting_active so the strip keeps being painted
//         while the scale ramps current→0; on completion the fade task
//         performs the real strip clear and drops s_painting_active.
// The painting-active flag lets set_all_impl() (which normally rejects writes
// when s_power_on==false) keep accepting frames during a fade-out, so JS-mode
// fades the live animation rather than freezing on the last frame.
#define FADE_Q16_FULL       65535
#define FADE_TICK_MS        20      // ~50 Hz repaint during fades — smooth
#define FADE_DEFAULT_ON_MS  600
#define FADE_DEFAULT_OFF_MS 800
#define FADE_MAX_MS         5000

static uint16_t s_fade_on_ms  = FADE_DEFAULT_ON_MS;
static uint16_t s_fade_off_ms = FADE_DEFAULT_OFF_MS;
static volatile uint16_t s_fade_scale_q16 = FADE_Q16_FULL;
static volatile int8_t   s_fade_dir = 0;          // +1 fading in, -1 fading out, 0 idle
static volatile uint16_t s_fade_start_scale = FADE_Q16_FULL;
static volatile uint32_t s_fade_start_ms = 0;
static volatile uint16_t s_fade_duration_ms = 0;
static volatile bool     s_painting_active = false;
static TaskHandle_t      s_fade_task_handle = NULL;
// Last wall-time some *external* writer (JS player, API color set, stream
// frame) called into the paint API. The fade task uses this to stay out of
// the way: in JS mode an animator is repainting ~30 Hz, so the fade task
// should NOT paint — each JS frame already picks up the latest scale and
// dims itself. Only when nothing has painted for a while (api-mode static
// colour) does the fade task drive its own repaints.
static volatile uint32_t s_last_external_paint_ms = 0;
static volatile uint32_t s_fade_arm_count = 0;
static volatile uint32_t s_external_paint_count = 0;
static led_fade_complete_cb_t s_fade_done_cb = NULL;

// Serialises full-frame writes between the JS player, API-color setters and
// the fade task. Recursive so apply_frame_buffer can fall through to
// apply_last_color (or callers can wrap larger transactions) without
// self-deadlocking. NULL until init completes — early callers (which run
// single-threaded before tasks exist) just skip the lock.
static SemaphoreHandle_t s_render_mutex = NULL;
static inline void render_lock(void)
{
    if (s_render_mutex) xSemaphoreTakeRecursive(s_render_mutex, portMAX_DELAY);
}
static inline void render_unlock(void)
{
    if (s_render_mutex) xSemaphoreGiveRecursive(s_render_mutex);
}

// Forward decls — definitions live below led_control_init() so the fade
// machinery groups visually with led_control_power().
static void fade_task(void *arg);
static void fade_arm(int8_t dir, uint16_t duration_ms);
static uint16_t fade_observe_scale_q16(int8_t *out_completed_dir);
static inline uint32_t now_ms(void);

static inline uint8_t scale_with(uint8_t val, uint8_t effective)
{
    return (uint8_t)(((uint16_t)val * effective) / 255);
}

static uint32_t estimate_current_ma(const led_color_t *colors, int count, uint8_t effective)
{
    uint32_t sum = 0;
    for (int i = 0; i < count; i++) {
        sum += colors[i].r;
        sum += colors[i].g;
        sum += colors[i].b;
    }
    return (uint32_t)(((uint64_t)sum * LED_CHANNEL_MA_AT_255 * effective) / (255ULL * 255ULL));
}

static uint8_t compute_effective_brightness(const led_color_t *colors, int count)
{
    uint8_t cap = s_brightness;
    if (s_max_brightness < cap) cap = s_max_brightness;
    if (s_brightness_override > 0 && s_brightness_override < cap) cap = s_brightness_override;
    if (s_max_current_ma == 0 || count == 0 || colors == NULL) return cap;
    uint32_t at_cap = estimate_current_ma(colors, count, cap);
    if (at_cap <= s_max_current_ma) return cap;
    uint32_t scaled = ((uint32_t)cap * s_max_current_ma) / at_cap;
    if (scaled > 255) scaled = 255;
    return (uint8_t)scaled;
}

static void save_last_color(void)
{
    int32_t packed = ((int32_t)s_last_color.r << 16) |
                     ((int32_t)s_last_color.g << 8) |
                     (int32_t)s_last_color.b;
    config_store_set_i32(NVS_KEY_LAST_COLOR, packed);
}

static void load_last_color(void)
{
    int32_t packed = config_get_i32_or(NVS_KEY_LAST_COLOR, 0);
    s_last_color.r = (packed >> 16) & 0xFF;
    s_last_color.g = (packed >> 8) & 0xFF;
    s_last_color.b = packed & 0xFF;
}

/* Forward declarations — backend helpers call into the two-wire path below. */
static esp_err_t apa102_refresh(void);
static void apa102_set_pixel(int idx, uint8_t r, uint8_t g, uint8_t b, uint8_t effective);

/** Write pixel to whichever backend is active. */
static inline void backend_set_pixel(int idx, uint8_t r, uint8_t g, uint8_t b, uint8_t effective)
{
    if (s_backend == LED_BACKEND_APA102) {
        apa102_set_pixel(idx, r, g, b, effective);
    } else if (s_rgbw) {
        // SK6812W expects 4 bytes per LED (GRBW). We keep the white channel at 0
        // so the public RGB API stays identical — the W sub-pixel is simply unused.
        led_strip_set_pixel_rgbw(s_strip, idx,
            scale_with(r, effective), scale_with(g, effective), scale_with(b, effective), 0);
    } else {
        led_strip_set_pixel(s_strip, idx,
            scale_with(r, effective), scale_with(g, effective), scale_with(b, effective));
    }
}

static inline esp_err_t backend_refresh(void)
{
    return (s_backend == LED_BACKEND_APA102) ? apa102_refresh() : led_strip_refresh(s_strip);
}

static inline esp_err_t backend_clear(void)
{
    if (s_backend == LED_BACKEND_APA102) {
        for (int i = 0; i < s_led_count; i++) apa102_set_pixel(i, 0, 0, 0, 0);
        return apa102_refresh();
    }
    return led_strip_clear(s_strip);
}

static inline bool backend_ready(void)
{
    return s_backend == LED_BACKEND_APA102 ? (s_spi != NULL) : (s_strip != NULL);
}

static inline uint8_t fade_apply(uint8_t v, uint16_t fade_q16)
{
    if (fade_q16 == FADE_Q16_FULL) return v;
    if (fade_q16 == 0) return 0;
    return (uint8_t)(((uint32_t)v * fade_q16) >> 16);
}

static void apply_last_color(void)
{
    render_lock();
    if (!backend_ready() || s_led_count == 0) { render_unlock(); return; }
    uint16_t fade_q16 = fade_observe_scale_q16(NULL);
    // Build a temporary uniform buffer so the current cap sees the full draw.
    uint32_t sum = (uint32_t)s_led_count * (s_last_color.r + s_last_color.g + s_last_color.b);
    uint8_t cap = s_brightness;
    if (s_max_brightness < cap) cap = s_max_brightness;
    if (s_brightness_override > 0 && s_brightness_override < cap) cap = s_brightness_override;
    uint8_t effective = cap;
    if (s_max_current_ma > 0 && sum > 0) {
        uint32_t at_cap = (uint32_t)(((uint64_t)sum * LED_CHANNEL_MA_AT_255 * cap) / (255ULL * 255ULL));
        if (at_cap > s_max_current_ma) {
            effective = (uint8_t)(((uint32_t)cap * s_max_current_ma) / at_cap);
        }
    }
    uint8_t r = fade_apply(s_last_color.r, fade_q16);
    uint8_t g = fade_apply(s_last_color.g, fade_q16);
    uint8_t b = fade_apply(s_last_color.b, fade_q16);
    for (int i = 0; i < s_led_count; i++) {
        backend_set_pixel(i, r, g, b, effective);
    }
    backend_refresh();
    render_unlock();
}

static void apply_frame_buffer(void)
{
    render_lock();
    if (!s_has_frame_buffer || s_frame_buffer == NULL) {
        apply_last_color();
        render_unlock();
        return;
    }
    uint16_t fade_q16 = fade_observe_scale_q16(NULL);
    uint8_t effective = compute_effective_brightness(s_frame_buffer, s_led_count);
    for (int i = 0; i < s_led_count; i++) {
        backend_set_pixel(i,
            fade_apply(s_frame_buffer[i].r, fade_q16),
            fade_apply(s_frame_buffer[i].g, fade_q16),
            fade_apply(s_frame_buffer[i].b, fade_q16),
            effective);
    }
    backend_refresh();
    render_unlock();
}

/** Set up led_strip SPI backend for WS2812 / SK6812(W). */
static esp_err_t init_one_wire(int gpio_pin, int led_count, led_model_t model, bool rgbw)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = gpio_pin,
        .max_leds = led_count,
        .led_pixel_format = rgbw ? LED_PIXEL_FORMAT_GRBW : LED_PIXEL_FORMAT_GRB,
        .led_model = model,
        .flags.invert_out = false,
    };
    led_strip_spi_config_t spi_config = {
        .clk_src = SPI_CLK_SRC_DEFAULT,
        .spi_bus = SPI2_HOST,
        .flags.with_dma = true,
    };
    s_rgbw = rgbw;
    return led_strip_new_spi_device(&strip_config, &spi_config, &s_strip);
}

/** Set up direct-SPI backend for SK9822 (APA102-compatible 2-wire protocol). */
static esp_err_t init_apa102(int data_pin, int clk_pin, int led_count)
{
    if (clk_pin < 0) {
        ESP_LOGE(TAG, "SK9822 requires a clock pin; set PLAIIIN_LED_CLK_PIN");
        return ESP_ERR_INVALID_ARG;
    }
    spi_bus_config_t bus = {
        .mosi_io_num = data_pin,
        .sclk_io_num = clk_pin,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4 + led_count * 4 + 4 + (led_count + 15) / 16,
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    spi_device_interface_config_t dev = {
        .clock_speed_hz = 4 * 1000 * 1000, // 4 MHz is safe for long lines
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 1,
    };
    err = spi_bus_add_device(SPI2_HOST, &dev, &s_spi);
    if (err != ESP_OK) return err;

    // Pre-allocate the full frame: 4-byte start + (4 bytes × count) + end clocks.
    // End frame: 4 bytes of 0x00 followed by ceil(count/16) bytes of 0x00 for extra SCK edges.
    size_t end_bytes = 4 + (led_count + 15) / 16;
    s_apa_buf_len = 4 + (size_t)led_count * 4 + end_bytes;
    s_apa_buf = (uint8_t *)heap_caps_calloc(1, s_apa_buf_len, MALLOC_CAP_DMA);
    if (!s_apa_buf) return ESP_ERR_NO_MEM;

    // Pre-fill start frame (0x00000000) — already zeroed by calloc.
    // Default each LED header to 0xE0 (brightness=0, MSB bits=111).
    for (int i = 0; i < led_count; i++) {
        s_apa_buf[4 + i * 4] = 0xE0;
    }
    return ESP_OK;
}

/** Push the APA102 frame buffer over SPI. Pixels assumed already filled. */
static esp_err_t apa102_refresh(void)
{
    if (!s_spi || !s_apa_buf) return ESP_ERR_INVALID_STATE;
    spi_transaction_t t = {0};
    t.length = s_apa_buf_len * 8;
    t.tx_buffer = s_apa_buf;
    return spi_device_polling_transmit(s_spi, &t);
}

/** Write one pixel into the APA102 frame buffer. effective is applied here. */
static void apa102_set_pixel(int idx, uint8_t r, uint8_t g, uint8_t b, uint8_t effective)
{
    // Header byte: 0xE0 | brightness (0..31). Map effective 0..255 into 0..31.
    uint8_t brightness5 = (uint8_t)((effective * 31) / 255);
    if (brightness5 == 0 && effective > 0) brightness5 = 1;
    uint8_t *p = &s_apa_buf[4 + idx * 4];
    p[0] = 0xE0 | (brightness5 & 0x1F);
    p[1] = b;  // APA102/SK9822 is BGR on the wire
    p[2] = g;
    p[3] = r;
}

esp_err_t led_control_init(int gpio_pin, int clk_pin, int led_count, const char *led_type)
{
    if (!s_render_mutex) {
        s_render_mutex = xSemaphoreCreateRecursiveMutex();
        if (!s_render_mutex) {
            ESP_LOGE(TAG, "Failed to create render mutex");
            return ESP_ERR_NO_MEM;
        }
    }
    s_led_count = led_count;
    load_last_color();
    s_brightness = (uint8_t)config_get_i32_or(NVS_KEY_BRIGHTNESS, 255);
    s_max_brightness = (uint8_t)config_get_i32_or(NVS_KEY_MAX_BRIGHT, 255);
    if (s_max_brightness == 0) s_max_brightness = 255;
    s_max_current_ma = (uint32_t)config_get_i32_or(NVS_KEY_MAX_CURR_MA, 0);
    s_px_group_w = (int)config_get_i32_or(NVS_KEY_PX_GROUP_W, 1);
    s_px_group_h = (int)config_get_i32_or(NVS_KEY_PX_GROUP_H, 1);
    if (s_px_group_w < 1) s_px_group_w = 1;
    if (s_px_group_h < 1) s_px_group_h = 1;

    s_rotation   = (int)config_get_i32_or(CONFIG_KEY_ROTATION, 0);
    if (s_rotation != 0 && s_rotation != 90 && s_rotation != 180 && s_rotation != 270) s_rotation = 0;
    s_origin     = (int)config_get_i32_or(CONFIG_KEY_ORIGIN, 0);
    if (s_origin < 0 || s_origin > 3) s_origin = 0;
    s_serpentine = config_get_i32_or(CONFIG_KEY_SERPENTINE, 1) != 0;
    s_serp_axis  = (int)config_get_i32_or(CONFIG_KEY_SERP_AXIS, 0);
    if (s_serp_axis != 0 && s_serp_axis != 1) s_serp_axis = 0;

    int32_t on_ms  = config_get_i32_or(NVS_KEY_FADE_ON_MS,  FADE_DEFAULT_ON_MS);
    int32_t off_ms = config_get_i32_or(NVS_KEY_FADE_OFF_MS, FADE_DEFAULT_OFF_MS);
    if (on_ms  < 0) on_ms  = 0; else if (on_ms  > FADE_MAX_MS) on_ms  = FADE_MAX_MS;
    if (off_ms < 0) off_ms = 0; else if (off_ms > FADE_MAX_MS) off_ms = FADE_MAX_MS;
    s_fade_on_ms  = (uint16_t)on_ms;
    s_fade_off_ms = (uint16_t)off_ms;

    s_frame_buffer = (led_color_t *)calloc(led_count, sizeof(led_color_t));
    if (!s_frame_buffer) {
        ESP_LOGE(TAG, "Failed to allocate frame buffer");
        return ESP_ERR_NO_MEM;
    }

    snprintf(s_led_type, sizeof(s_led_type), "%s", led_type ? led_type : "ws2812");

    esp_err_t err;
    if (strcmp(s_led_type, "sk9822") == 0 || strcmp(s_led_type, "apa102") == 0) {
        s_backend = LED_BACKEND_APA102;
        err = init_apa102(gpio_pin, clk_pin, led_count);
    } else if (strcmp(s_led_type, "sk6812w") == 0 || strcmp(s_led_type, "sk6812rgbw") == 0) {
        s_backend = LED_BACKEND_STRIP;
        err = init_one_wire(gpio_pin, led_count, LED_MODEL_SK6812, /*rgbw=*/true);
    } else if (strcmp(s_led_type, "sk6812") == 0) {
        s_backend = LED_BACKEND_STRIP;
        err = init_one_wire(gpio_pin, led_count, LED_MODEL_SK6812, /*rgbw=*/false);
    } else {
        s_backend = LED_BACKEND_STRIP;
        err = init_one_wire(gpio_pin, led_count, LED_MODEL_WS2812, /*rgbw=*/false);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init LED driver (%s): %s", s_led_type, esp_err_to_name(err));
        return err;
    }

    // Spawn the fade-driver task before any fade is armed. It sleeps on its
    // own notification when idle, so the cost is one suspended task slot.
    xTaskCreate(fade_task, "led_fade", 2048, NULL, 5, &s_fade_task_handle);

    // Boot: lamp comes up powered, fading in from black to the saved colour.
    s_power_on = true;
    s_painting_active = true;
    s_fade_scale_q16 = 0;
    fade_arm(+1, s_fade_on_ms);
    ESP_LOGI(TAG, "LED driver '%s' initialized: %d LEDs on GPIO %d (clk %d), brightness=%d max_bright=%d max_ma=%lu fade=%u/%u ms",
             s_led_type, led_count, gpio_pin, clk_pin,
             s_brightness, s_max_brightness, (unsigned long)s_max_current_ma,
             s_fade_on_ms, s_fade_off_ms);
    return ESP_OK;
}

static esp_err_t set_all_impl(const led_color_t *colors, int count)
{
    if (!backend_ready()) return ESP_ERR_INVALID_STATE;
    // While a fade-out is in progress we keep accepting writes even after
    // s_power_on flips false — that's what lets a JS-mode fade-out dim the
    // live animation instead of freezing on the last frame.
    if (!s_power_on && !s_painting_active) return ESP_ERR_INVALID_STATE;
    if (colors == NULL || count <= 0) return ESP_ERR_INVALID_ARG;

    if (count != s_led_count) {
        ESP_LOGW(TAG, "set_all: frame has %d pixels, expected %d", count, s_led_count);
        if (count > s_led_count) count = s_led_count;
    }

    render_lock();
    if (s_frame_buffer) {
        memcpy(s_frame_buffer, colors, count * sizeof(led_color_t));
        for (int i = count; i < s_led_count; i++) {
            s_frame_buffer[i].r = 0;
            s_frame_buffer[i].g = 0;
            s_frame_buffer[i].b = 0;
        }
        s_has_frame_buffer = true;
    }

    uint8_t effective = compute_effective_brightness(colors, count);
    uint16_t fade_q16 = fade_observe_scale_q16(NULL);
    for (int i = 0; i < count; i++) {
        backend_set_pixel(i,
            fade_apply(colors[i].r, fade_q16),
            fade_apply(colors[i].g, fade_q16),
            fade_apply(colors[i].b, fade_q16),
            effective);
    }

    esp_err_t err = backend_refresh();
    s_last_external_paint_ms = now_ms();
    s_external_paint_count++;
    render_unlock();
    return err;
}

esp_err_t led_control_set_all(const led_color_t *colors, int count)
{
    esp_err_t err = set_all_impl(colors, count);
    if (err == ESP_OK && colors != NULL && count > 0) {
        s_last_color = colors[0];
        save_last_color();
    }
    return err;
}

esp_err_t led_control_set_all_transient(const led_color_t *colors, int count)
{
    return set_all_impl(colors, count);
}

esp_err_t led_control_set_pixel(int index, uint8_t r, uint8_t g, uint8_t b)
{
    if (!backend_ready()) return ESP_ERR_INVALID_STATE;
    if (!s_power_on && !s_painting_active) return ESP_ERR_INVALID_STATE;
    if (index < 0 || index >= s_led_count) return ESP_ERR_INVALID_ARG;

    render_lock();
    // Track the pixel in the frame buffer so brightness/current re-apply sees it,
    // and so max_current_ma is enforced against the whole buffer (not just this pixel).
    if (s_frame_buffer) {
        s_frame_buffer[index].r = r;
        s_frame_buffer[index].g = g;
        s_frame_buffer[index].b = b;
        s_has_frame_buffer = true;
    }

    uint8_t effective = s_frame_buffer
        ? compute_effective_brightness(s_frame_buffer, s_led_count)
        : s_brightness;
    if (s_max_brightness < effective) effective = s_max_brightness;
    if (s_brightness_override > 0 && s_brightness_override < effective) effective = s_brightness_override;
    uint16_t fade_q16 = fade_observe_scale_q16(NULL);
    backend_set_pixel(index,
        fade_apply(r, fade_q16),
        fade_apply(g, fade_q16),
        fade_apply(b, fade_q16),
        effective);
    s_last_external_paint_ms = now_ms();
    s_external_paint_count++;
    render_unlock();
    return ESP_OK;
}

esp_err_t led_control_refresh(void)
{
    if (!backend_ready()) return ESP_ERR_INVALID_STATE;
    render_lock();
    esp_err_t err = backend_refresh();
    render_unlock();
    return err;
}

esp_err_t led_control_clear(void)
{
    if (!backend_ready()) return ESP_ERR_INVALID_STATE;
    render_lock();
    esp_err_t err = backend_clear();
    render_unlock();
    return err;
}

void led_control_enable(void)
{
    s_power_on = true;
}

// Fade scale at progress 0..1 — *linear* for now. The sin curve we tried
// front-loaded the change (fade-out dropped 39 % in the first 200 ms,
// reading as a snap; fade-in flashed to 30 % in 120 ms, reading as a blink).
// Linear feels smoother end-to-end; we can re-introduce a non-linear curve
// once the whole pipeline reads cleanly.
static inline uint16_t ramp_q16(float p)
{
    if (p <= 0.f) return 0;
    if (p >= 1.f) return FADE_Q16_FULL;
    int v = (int)(p * (float)FADE_Q16_FULL + 0.5f);
    if (v < 0) v = 0; else if (v > FADE_Q16_FULL) v = FADE_Q16_FULL;
    return (uint16_t)v;
}

static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// Lazy scale read: computes the eased scale at NOW based on the armed fade
// state. Painters call this on every frame so each render uses an up-to-date
// scale (no separate ticker fighting the JS player). When the fade reaches
// its rail this function settles the rail and clears s_fade_dir; the dir
// observed at completion is reported via *out_completed_dir so the fade task
// can do the post-fade cleanup (strip clear, callback) exactly once. Most
// callers pass NULL — they don't care about completion events.
static uint16_t fade_observe_scale_q16(int8_t *out_completed_dir)
{
    if (out_completed_dir) *out_completed_dir = 0;
    int8_t dir = s_fade_dir;
    if (dir == 0) return s_fade_scale_q16;

    uint32_t elapsed = now_ms() - s_fade_start_ms;
    uint16_t dur = s_fade_duration_ms;
    int32_t start = s_fade_start_scale;
    int32_t target = (dir > 0) ? FADE_Q16_FULL : 0;

    if (dur == 0 || elapsed >= dur) {
        // Landed on the rail. Settle exactly once.
        uint16_t final = (uint16_t)target;
        s_fade_scale_q16 = final;
        s_fade_dir = 0;
        if (out_completed_dir) *out_completed_dir = dir;
        return final;
    }

    float p = (float)elapsed / (float)dur;
    uint16_t eased = ramp_q16(p);
    // Q16 lerp must be done in int64 — (FULL - 0) * (eased near FULL) is
    // ~4.3 billion, which overflows int32 and wraps the result negative,
    // which then makes `start - negative` clamp to FULL (visible bug: scale
    // jumped back to bright mid fade-out, jumped down to 0 mid fade-in).
    int64_t delta_e16 = (int64_t)(target - start) * (int64_t)eased;
    int32_t scale = start + (int32_t)(delta_e16 >> 16);
    if (scale < 0) scale = 0;
    if (scale > FADE_Q16_FULL) scale = FADE_Q16_FULL;
    s_fade_scale_q16 = (uint16_t)scale;
    return (uint16_t)scale;
}

// Fade driver. Sleeps on its own notification when idle. While a fade is
// armed:
//   - In JS mode (something painting recently): does NOT paint — every JS
//     frame picks up the lazily-computed scale from fade_observe_scale_q16
//     and dims itself. We just poll for completion.
//   - In API mode (no recent external write): repaints from the saved frame
//     so the static colour visibly dims.
// On fade-OUT completion: clears the strip and fires s_fade_done_cb so
// light_api can stop the JS player AFTER the live animation has finished
// dimming (rather than killing it the instant the off command lands).
static void fade_task(void *arg)
{
    (void)arg;
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (1) {
            int8_t completed_dir = 0;
            fade_observe_scale_q16(&completed_dir);
            if (completed_dir != 0) {
                if (completed_dir < 0) {
                    // Fade-OUT done — clear strip, drop painting flag, notify.
                    // Also drop the cached frame buffer: the next fade-in's
                    // pre-JS window (the few ms before the new JS task spawns
                    // and starts painting) should fall through to a uniform
                    // last_color paint, NOT replay the stale last JS frame.
                    render_lock();
                    s_painting_active = false;
                    s_has_frame_buffer = false;
                    backend_clear();
                    render_unlock();
                    if (s_fade_done_cb) s_fade_done_cb(true);
                } else {
                    if (s_fade_done_cb) s_fade_done_cb(false);
                }
                break;
            }
            // Mid-fade. Repaint only if no external writer has touched the
            // strip recently — JS player paints ~30 ms apart, so a 60 ms gate
            // keeps us out of its way while still driving API-mode fades.
            uint32_t since_ext = now_ms() - s_last_external_paint_ms;
            if (since_ext > 60) {
                apply_frame_buffer();
            }
            vTaskDelay(pdMS_TO_TICKS(FADE_TICK_MS));
        }
    }
}

static void fade_arm(int8_t dir, uint16_t duration_ms)
{
    if (duration_ms == 0) {
        // Snap path through the fade machinery: jump to target, paint once.
        s_fade_scale_q16 = (dir > 0) ? FADE_Q16_FULL : 0;
        s_fade_dir = 0;
        if (dir > 0) {
            apply_frame_buffer();
        } else {
            render_lock();
            s_painting_active = false;
            backend_clear();
            render_unlock();
        }
        return;
    }
    s_fade_start_scale = s_fade_scale_q16;
    s_fade_start_ms = now_ms();
    s_fade_duration_ms = duration_ms;
    s_fade_dir = dir;
    s_fade_arm_count++;
    if (s_fade_task_handle) {
        xTaskNotifyGive(s_fade_task_handle);
    }
}

esp_err_t led_control_power(bool on)
{
    if (!backend_ready()) return ESP_ERR_INVALID_STATE;
    if (on) {
        // If we were mid-fade-out, the strip is still being painted (scale > 0)
        // and s_power_on is already false — flipping back to on cancels the
        // clear and reverses direction from the current scale.
        s_power_on = true;
        s_painting_active = true;
        fade_arm(+1, s_fade_on_ms);
    } else {
        // Per Phase 33 spec: /api/state and BLE flip to off the instant the
        // command lands; the fade just animates the visual.
        s_power_on = false;
        s_painting_active = true;
        fade_arm(-1, s_fade_off_ms);
    }
    ESP_LOGI(TAG, "Power %s (fade %ums)", on ? "ON" : "OFF",
             on ? s_fade_on_ms : s_fade_off_ms);
    return ESP_OK;
}

esp_err_t led_control_power_snap(bool on)
{
    if (!backend_ready()) return ESP_ERR_INVALID_STATE;
    // Cancel any in-flight fade and pin scale at the target. System flashes
    // (factory-reset confirm, long-press warning, stream-mode auto-wake) need
    // the visual to land immediately, not animate.
    s_fade_dir = 0;
    s_fade_scale_q16 = FADE_Q16_FULL;
    s_power_on = on;
    if (on) {
        s_painting_active = true;
        apply_frame_buffer();
    } else {
        render_lock();
        s_painting_active = false;
        backend_clear();
        render_unlock();
    }
    ESP_LOGI(TAG, "Power %s (snap)", on ? "ON" : "OFF");
    return ESP_OK;
}

bool led_control_is_on(void)
{
    return s_power_on;
}

int led_control_get_count(void)
{
    return s_led_count;
}

led_color_t led_control_get_last_color(void)
{
    return s_last_color;
}

void led_control_set_brightness(uint8_t brightness)
{
    s_brightness = brightness;
    config_store_set_i32(NVS_KEY_BRIGHTNESS, (int32_t)brightness);
    ESP_LOGI(TAG, "Brightness set to %d (cap %d)", brightness, s_max_brightness);
    if (s_power_on && backend_ready()) {
        apply_frame_buffer();
    }
}

uint8_t led_control_get_brightness(void)
{
    uint8_t cap = s_brightness;
    if (s_max_brightness < cap) cap = s_max_brightness;
    return cap;
}

void led_control_set_brightness_override(uint8_t cap)
{
    s_brightness_override = cap;
    ESP_LOGI(TAG, "Brightness override %s (cap %u)",
             cap == 0 ? "cleared" : "set", cap);
    if (s_power_on && backend_ready()) {
        apply_frame_buffer();
    }
}

uint8_t led_control_get_brightness_override(void)
{
    return s_brightness_override;
}

void led_control_set_max_brightness(uint8_t max_brightness)
{
    if (max_brightness == 0) max_brightness = 255;
    s_max_brightness = max_brightness;
    config_store_set_i32(NVS_KEY_MAX_BRIGHT, (int32_t)max_brightness);
    ESP_LOGI(TAG, "Max brightness set to %d", max_brightness);
    if (s_power_on && backend_ready()) {
        apply_frame_buffer();
    }
}

uint8_t led_control_get_max_brightness(void)
{
    return s_max_brightness;
}

void led_control_set_max_current_ma(uint32_t max_current_ma)
{
    s_max_current_ma = max_current_ma;
    config_store_set_i32(NVS_KEY_MAX_CURR_MA, (int32_t)max_current_ma);
    ESP_LOGI(TAG, "Max current set to %lu mA", (unsigned long)max_current_ma);
    if (s_power_on && backend_ready()) {
        apply_frame_buffer();
    }
}

uint32_t led_control_get_max_current_ma(void)
{
    return s_max_current_ma;
}

void led_control_set_physical_grid(int w, int h)
{
    s_phys_w = w;
    s_phys_h = h;
}

int led_control_get_physical_w(void) { return s_phys_w; }
int led_control_get_physical_h(void) { return s_phys_h; }

void led_control_set_pixel_group(int w, int h)
{
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    s_px_group_w = w;
    s_px_group_h = h;
    config_store_set_i32(NVS_KEY_PX_GROUP_W, w);
    config_store_set_i32(NVS_KEY_PX_GROUP_H, h);
    ESP_LOGI(TAG, "Pixel group set to %dx%d", w, h);
}

int led_control_get_pixel_group_w(void) { return s_px_group_w; }
int led_control_get_pixel_group_h(void) { return s_px_group_h; }

int led_control_get_logical_w(void)
{
    if (s_phys_w <= 0) return s_led_count;
    return s_phys_w / (s_px_group_w > 0 ? s_px_group_w : 1);
}

int led_control_get_logical_h(void)
{
    if (s_phys_h <= 0) return 1;
    return s_phys_h / (s_px_group_h > 0 ? s_px_group_h : 1);
}

// Map a chain index `chain_i` (the i-th LED on the wire) into its panel coordinates
// (px,py) on a phys_w × phys_h grid given origin corner + serpentine wiring + axis.
// (px,py) are in *panel* (post-rotation) space — i.e. how the source image looks
// when oriented for the viewer.
//
// For multi-panel chains (e.g. the cube — five 8x8 faces wired back-to-back into
// one 320-LED strip) the chain row/col counter is taken modulo the panel size,
// so each face maps to the same (px,py) range and gets the same source image
// tiled onto it. Single-panel forms (tower) are unaffected because their chain
// already fits within phys_w*phys_h.
static inline void chain_to_panel(int chain_i, int phys_w, int phys_h,
                                  int origin, bool serpentine, int serp_axis,
                                  int *out_px, int *out_py)
{
    int x, y;
    if (serp_axis == 0) {
        // Chain advances along X within a row, then steps in Y.
        int row_chain = chain_i / phys_w;
        int col       = chain_i - row_chain * phys_w;
        int row       = (phys_h > 0) ? (row_chain % phys_h) : row_chain;
        // For serpentine wiring, every odd row reverses direction.
        if (serpentine && (row & 1)) col = phys_w - 1 - col;
        x = col;
        y = row;
    } else {
        // Chain advances along Y within a column, then steps in X.
        int col_chain = chain_i / phys_h;
        int row       = chain_i - col_chain * phys_h;
        int col       = (phys_w > 0) ? (col_chain % phys_w) : col_chain;
        if (serpentine && (col & 1)) row = phys_h - 1 - row;
        x = col;
        y = row;
    }
    // Origin corner: TL is the unflipped baseline.
    if (origin == LED_ORIGIN_TR || origin == LED_ORIGIN_BR) x = phys_w - 1 - x;
    if (origin == LED_ORIGIN_BL || origin == LED_ORIGIN_BR) y = phys_h - 1 - y;
    *out_px = x;
    *out_py = y;
}

esp_err_t led_control_set_logical(const led_color_t *colors, int logical_w, int logical_h)
{
    if (!colors || logical_w <= 0 || logical_h <= 0) return ESP_ERR_INVALID_ARG;
    int gw = s_px_group_w > 0 ? s_px_group_w : 1;
    int gh = s_px_group_h > 0 ? s_px_group_h : 1;
    int phys_w = s_phys_w > 0 ? s_phys_w : (logical_w * gw);
    int phys_h = s_phys_h > 0 ? s_phys_h : (logical_h * gh);
    // Iterate the whole chain; chain_to_panel modulos by phys_w/phys_h so
    // multi-panel chains (cube) get the source image tiled onto every face.
    int total = s_led_count;

    led_color_t *frame = (led_color_t *)calloc(total, sizeof(led_color_t));
    if (!frame) return ESP_ERR_NO_MEM;

    // For non-matrix panels (single row strips), the orientation transform
    // collapses to identity and the chain is just the source order.
    bool is_matrix = (phys_h > 1);
    int rotation = is_matrix ? s_rotation : 0;
    int origin   = is_matrix ? s_origin   : 0;
    bool serp    = is_matrix ? s_serpentine : false;
    int serp_axis = is_matrix ? s_serp_axis : 0;

    // Panel-space dimensions are the same as physical dimensions; the
    // rotation only affects how we sample the *source* image.
    for (int chain_i = 0; chain_i < total; chain_i++) {
        int px, py;
        chain_to_panel(chain_i, phys_w, phys_h, origin, serp, serp_axis, &px, &py);

        // Rotation: physical panel is mounted rotated by `rotation` degrees
        // CW relative to the source image. To display the image upright on a
        // panel rotated by R, sample the source from inverse rotation.
        int sx, sy, src_w, src_h;
        if (rotation == 0)        { sx = px;            sy = py;            src_w = phys_w; src_h = phys_h; }
        else if (rotation == 90)  { sx = py;            sy = phys_w - 1 - px; src_w = phys_h; src_h = phys_w; }
        else if (rotation == 180) { sx = phys_w - 1 - px; sy = phys_h - 1 - py; src_w = phys_w; src_h = phys_h; }
        else /* 270 */            { sx = phys_h - 1 - py; sy = px;            src_w = phys_h; src_h = phys_w; }

        // Source image lives in logical space — fold pixel grouping in.
        int lx = sx / gw;
        int ly = sy / gh;
        if (lx >= logical_w) lx = logical_w - 1;
        if (ly >= logical_h) ly = logical_h - 1;
        // (src_w/src_h are kept above for clarity and future bounds checks.)
        (void)src_w; (void)src_h;

        frame[chain_i] = colors[ly * logical_w + lx];
    }

    esp_err_t err = led_control_set_all(frame, total);
    free(frame);
    return err;
}

void led_control_set_orientation(int rotation, int origin, bool serpentine, int serp_axis)
{
    if (rotation != 0 && rotation != 90 && rotation != 180 && rotation != 270) rotation = 0;
    if (origin < 0 || origin > 3) origin = 0;
    if (serp_axis != 0 && serp_axis != 1) serp_axis = 0;
    s_rotation   = rotation;
    s_origin     = origin;
    s_serpentine = serpentine;
    s_serp_axis  = serp_axis;
    config_store_set_i32(CONFIG_KEY_ROTATION,   rotation);
    config_store_set_i32(CONFIG_KEY_ORIGIN,     origin);
    config_store_set_i32(CONFIG_KEY_SERPENTINE, serpentine ? 1 : 0);
    config_store_set_i32(CONFIG_KEY_SERP_AXIS,  serp_axis);
    ESP_LOGI(TAG, "Orientation: rot=%d origin=%d serp=%d axis=%d",
             rotation, origin, serpentine ? 1 : 0, serp_axis);
    // Re-render the current frame so the change shows up immediately.
    if (s_power_on && backend_ready()) {
        apply_frame_buffer();
    }
}

int  led_control_get_rotation(void)  { return s_rotation; }
int  led_control_get_origin(void)    { return s_origin; }
bool led_control_get_serpentine(void){ return s_serpentine; }
int  led_control_get_serp_axis(void) { return s_serp_axis; }

void led_control_set_fade_durations(uint16_t on_ms, uint16_t off_ms)
{
    if (on_ms  > FADE_MAX_MS) on_ms  = FADE_MAX_MS;
    if (off_ms > FADE_MAX_MS) off_ms = FADE_MAX_MS;
    s_fade_on_ms  = on_ms;
    s_fade_off_ms = off_ms;
    config_store_set_i32(NVS_KEY_FADE_ON_MS,  (int32_t)on_ms);
    config_store_set_i32(NVS_KEY_FADE_OFF_MS, (int32_t)off_ms);
    ESP_LOGI(TAG, "Fade durations set: on=%ums off=%ums", on_ms, off_ms);
}

uint16_t led_control_get_fade_on_ms(void)  { return s_fade_on_ms; }
uint16_t led_control_get_fade_off_ms(void) { return s_fade_off_ms; }

void led_control_set_fade_complete_cb(led_fade_complete_cb_t cb)
{
    s_fade_done_cb = cb;
}

void led_control_fade_debug(led_fade_debug_t *out)
{
    if (!out) return;
    int8_t dir = s_fade_dir;
    out->scale_q16 = s_fade_scale_q16;
    out->dir = dir;
    out->duration_ms = s_fade_duration_ms;
    out->elapsed_ms = (dir != 0) ? (now_ms() - s_fade_start_ms) : 0;
    out->power_on = s_power_on;
    out->painting_active = s_painting_active;
    out->since_external_paint_ms = now_ms() - s_last_external_paint_ms;
    out->arm_count = s_fade_arm_count;
    out->external_paint_count = s_external_paint_count;
}
