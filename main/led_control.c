#include "led_control.h"
#include "config_store.h"
#include "led_strip.h"
#include "driver/spi_master.h"
#include "esp_log.h"

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
static uint32_t s_max_current_ma = 0; // 0 = unlimited
static int s_phys_w = 0;
static int s_phys_h = 0;
static int s_px_group_w = 1;
static int s_px_group_h = 1;

#define LED_CHANNEL_MA_AT_255  20  // WS2812 per-channel peak at 255

#define NVS_KEY_LAST_COLOR  "last_color"
#define NVS_KEY_BRIGHTNESS  "brightness"
#define NVS_KEY_MAX_BRIGHT  "max_bright"
#define NVS_KEY_MAX_CURR_MA "max_curr_ma"
#define NVS_KEY_PX_GROUP_W  "px_group_w"
#define NVS_KEY_PX_GROUP_H  "px_group_h"

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

static void apply_last_color(void)
{
    if (!backend_ready() || s_led_count == 0) return;
    // Build a temporary uniform buffer so the current cap sees the full draw.
    uint32_t sum = (uint32_t)s_led_count * (s_last_color.r + s_last_color.g + s_last_color.b);
    uint8_t cap = s_brightness;
    if (s_max_brightness < cap) cap = s_max_brightness;
    uint8_t effective = cap;
    if (s_max_current_ma > 0 && sum > 0) {
        uint32_t at_cap = (uint32_t)(((uint64_t)sum * LED_CHANNEL_MA_AT_255 * cap) / (255ULL * 255ULL));
        if (at_cap > s_max_current_ma) {
            effective = (uint8_t)(((uint32_t)cap * s_max_current_ma) / at_cap);
        }
    }
    for (int i = 0; i < s_led_count; i++) {
        backend_set_pixel(i, s_last_color.r, s_last_color.g, s_last_color.b, effective);
    }
    backend_refresh();
    ESP_LOGI(TAG, "Restored color #%02x%02x%02x cap=%u effective=%u",
             s_last_color.r, s_last_color.g, s_last_color.b, cap, effective);
}

static void apply_frame_buffer(void)
{
    if (!s_has_frame_buffer || s_frame_buffer == NULL) {
        apply_last_color();
        return;
    }
    uint8_t effective = compute_effective_brightness(s_frame_buffer, s_led_count);
    for (int i = 0; i < s_led_count; i++) {
        backend_set_pixel(i, s_frame_buffer[i].r, s_frame_buffer[i].g, s_frame_buffer[i].b, effective);
    }
    backend_refresh();
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

    s_power_on = true;
    apply_last_color();
    ESP_LOGI(TAG, "LED driver '%s' initialized: %d LEDs on GPIO %d (clk %d), brightness=%d max_bright=%d max_ma=%lu",
             s_led_type, led_count, gpio_pin, clk_pin,
             s_brightness, s_max_brightness, (unsigned long)s_max_current_ma);
    return ESP_OK;
}

esp_err_t led_control_set_all(const led_color_t *colors, int count)
{
    if (!backend_ready() || !s_power_on) return ESP_ERR_INVALID_STATE;
    if (colors == NULL || count <= 0) return ESP_ERR_INVALID_ARG;

    if (count != s_led_count) {
        ESP_LOGW(TAG, "set_all: frame has %d pixels, expected %d", count, s_led_count);
        if (count > s_led_count) count = s_led_count;
    }

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
    for (int i = 0; i < count; i++) {
        backend_set_pixel(i, colors[i].r, colors[i].g, colors[i].b, effective);
    }

    if (count > 0) {
        s_last_color = colors[0];
        save_last_color();
    }

    return backend_refresh();
}

esp_err_t led_control_set_pixel(int index, uint8_t r, uint8_t g, uint8_t b)
{
    if (!backend_ready() || !s_power_on) return ESP_ERR_INVALID_STATE;
    if (index < 0 || index >= s_led_count) return ESP_ERR_INVALID_ARG;

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
    backend_set_pixel(index, r, g, b, effective);
    return ESP_OK;
}

esp_err_t led_control_refresh(void)
{
    if (!backend_ready()) return ESP_ERR_INVALID_STATE;
    return backend_refresh();
}

esp_err_t led_control_clear(void)
{
    if (!backend_ready()) return ESP_ERR_INVALID_STATE;
    return backend_clear();
}

void led_control_enable(void)
{
    s_power_on = true;
}

esp_err_t led_control_power(bool on)
{
    if (!backend_ready()) return ESP_ERR_INVALID_STATE;
    s_power_on = on;
    if (on) {
        apply_frame_buffer();
    } else {
        backend_clear();
    }
    ESP_LOGI(TAG, "Power %s", on ? "ON" : "OFF");
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

esp_err_t led_control_set_logical(const led_color_t *colors, int logical_w, int logical_h)
{
    if (!colors || logical_w <= 0 || logical_h <= 0) return ESP_ERR_INVALID_ARG;
    int gw = s_px_group_w > 0 ? s_px_group_w : 1;
    int gh = s_px_group_h > 0 ? s_px_group_h : 1;
    int phys_w = s_phys_w > 0 ? s_phys_w : (logical_w * gw);
    int phys_h = s_phys_h > 0 ? s_phys_h : (logical_h * gh);
    int total = phys_w * phys_h;
    if (total > s_led_count) total = s_led_count;

    led_color_t *frame = (led_color_t *)calloc(total, sizeof(led_color_t));
    if (!frame) return ESP_ERR_NO_MEM;

    // phys_idx runs along the physical LED chain. On matrix panels the LEDs
    // are typically wired in a serpentine pattern, so for odd rows the first
    // LED on the chain corresponds to the right edge of the logical grid.
    bool serpentine = phys_h > 1;
    for (int py = 0; py < phys_h; py++) {
        int ly = py / gh;
        if (ly >= logical_h) ly = logical_h - 1;
        for (int px = 0; px < phys_w; px++) {
            // Map the logical-space X according to serpentine wiring so that
            // consecutive LEDs on the chain read the right logical column.
            int phys_col = (serpentine && (py % 2 == 1)) ? (phys_w - 1 - px) : px;
            int lx = phys_col / gw;
            if (lx >= logical_w) lx = logical_w - 1;
            int phys_idx = py * phys_w + px;
            int log_idx = ly * logical_w + lx;
            if (phys_idx < total) frame[phys_idx] = colors[log_idx];
        }
    }
    esp_err_t err = led_control_set_all(frame, total);
    free(frame);
    return err;
}
