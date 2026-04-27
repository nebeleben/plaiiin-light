#pragma once

#include "esp_err.h"

/**
 * Visual error codes shown on the LED strip.
 *   - No WiFi:        Slow red blink (1s on, 1s off)
 *   - Config error:   Fast red-yellow alternating (200ms)
 *   - AP mode:        Slow blue pulse
 */

typedef enum {
    ERROR_LIGHT_NONE,
    ERROR_LIGHT_NO_WIFI,
    ERROR_LIGHT_CONFIG_ERROR,
    ERROR_LIGHT_AP_MODE
} error_light_pattern_t;

esp_err_t error_light_init(void);
void error_light_set(error_light_pattern_t pattern);
void error_light_clear(void);
