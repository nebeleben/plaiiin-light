#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

/**
 * LED strip driver. Controls WS2812 LEDs via the RMT peripheral
 * using the ESP-IDF led_strip component.
 */

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} led_color_t;

/** Initialize the LED driver.
 *  @param gpio_pin Data line (both chip families).
 *  @param clk_pin  Clock line (two-wire chips only; pass -1 for WS2812/SK6812).
 *  @param led_count Number of LEDs on the strip.
 *  @param led_type "ws2812" | "sk6812" | "sk9822". Unknown values fall back to ws2812.
 */
esp_err_t led_control_init(int gpio_pin, int clk_pin, int led_count, const char *led_type);
esp_err_t led_control_set_all(const led_color_t *colors, int count);
esp_err_t led_control_set_pixel(int index, uint8_t r, uint8_t g, uint8_t b);
esp_err_t led_control_refresh(void);
esp_err_t led_control_clear(void);
esp_err_t led_control_power(bool on);
void led_control_enable(void);  // power on without restoring last color
bool led_control_is_on(void);
int led_control_get_count(void);
led_color_t led_control_get_last_color(void);

/** Sets the requested brightness. Will be clamped to max_brightness and to
 *  the value allowed by max_current_mA given the current buffer. */
void led_control_set_brightness(uint8_t brightness);
uint8_t led_control_get_brightness(void);

/** Upper bound the user can request. 0 = no cap (full 255). */
void led_control_set_max_brightness(uint8_t max_brightness);
uint8_t led_control_get_max_brightness(void);

/** Peak current budget in mA. Renderer scales brightness down so the
 *  estimated draw for the current buffer never exceeds this. 0 = disabled. */
void led_control_set_max_current_ma(uint32_t max_current_ma);
uint32_t led_control_get_max_current_ma(void);

/** Physical matrix dimensions. Set once at init from lamp_type config. */
void led_control_set_physical_grid(int w, int h);
int  led_control_get_physical_w(void);
int  led_control_get_physical_h(void);

/** Logical-to-physical pixel grouping. (1,1) = one physical LED per logical pixel. */
void led_control_set_pixel_group(int w, int h);
int  led_control_get_pixel_group_w(void);
int  led_control_get_pixel_group_h(void);
int  led_control_get_logical_w(void);
int  led_control_get_logical_h(void);

/** Render a logical buffer onto the physical strip honouring pixel grouping.
 *  Expects logical_w * logical_h pixels. */
esp_err_t led_control_set_logical(const led_color_t *colors, int logical_w, int logical_h);
