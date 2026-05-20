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
/** Like set_all but does NOT persist colors[0] as last_color — for transient
 *  indicator frames (long-press warnings, factory-reset confirm flashes) that
 *  shouldn't outlive themselves across reboots. */
esp_err_t led_control_set_all_transient(const led_color_t *colors, int count);
esp_err_t led_control_set_pixel(int index, uint8_t r, uint8_t g, uint8_t b);
esp_err_t led_control_refresh(void);
esp_err_t led_control_clear(void);
/** User-facing power toggle. Runs the configured on/off fade animation.
 *  Fade duration is per-direction (see led_control_set_fade_durations);
 *  duration == 0 makes that direction instant. */
esp_err_t led_control_power(bool on);
/** Instant on/off — bypasses the fade. Use for system indicator flashes
 *  (factory-reset confirm, long-press warning) and other places where the
 *  visual must snap, not animate. */
esp_err_t led_control_power_snap(bool on);
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

/** Panel orientation knobs — applied in led_control_set_logical().
 *  rotation: 0/90/180/270 — visual rotation of the source image.
 *  origin:   0=TL, 1=TR, 2=BL, 3=BR — corner where the physical chain begins.
 *  serpentine: 1 = chain zig-zags between rows/cols, 0 = each row/col restarts from origin edge.
 *  serp_axis: 0 = chain advances along X within a row, 1 = along Y within a column.
 */
typedef enum {
    LED_ORIGIN_TL = 0,
    LED_ORIGIN_TR = 1,
    LED_ORIGIN_BL = 2,
    LED_ORIGIN_BR = 3,
} led_origin_t;

void led_control_set_orientation(int rotation, int origin, bool serpentine, int serp_axis);
int  led_control_get_rotation(void);
int  led_control_get_origin(void);
bool led_control_get_serpentine(void);
int  led_control_get_serp_axis(void);

/** On/off fade durations in milliseconds. 0 disables the fade in that
 *  direction. Persisted to NVS. The transient indicator path
 *  (led_control_set_all_transient + led_control_power_snap) bypasses this. */
void     led_control_set_fade_durations(uint16_t on_ms, uint16_t off_ms);
uint16_t led_control_get_fade_on_ms(void);
uint16_t led_control_get_fade_off_ms(void);

/** Fade-completion callback. Fires from the fade task once a power-fade
 *  finishes. was_off==true means a fade-OUT just completed and the strip
 *  has been cleared — light_api uses this to stop the JS player only
 *  after the live animation has finished dimming. NULL clears the cb. */
typedef void (*led_fade_complete_cb_t)(bool was_off);
void led_control_set_fade_complete_cb(led_fade_complete_cb_t cb);

/** Diagnostic snapshot of fade-engine state. Used by /api/fade/debug to
 *  observe what's actually happening during a fade without serial logs. */
typedef struct {
    uint16_t scale_q16;
    int8_t   dir;
    uint16_t duration_ms;
    uint32_t elapsed_ms;
    bool     power_on;
    bool     painting_active;
    uint32_t since_external_paint_ms;
    uint32_t arm_count;
    uint32_t external_paint_count;
} led_fade_debug_t;
void led_control_fade_debug(led_fade_debug_t *out);
