#pragma once

#include "esp_err.h"

/**
 * Hardware button driver. Up to three independent buttons:
 *   power  — single tap toggles on/off; long-press 10s triggers factory reset.
 *   next   — single tap plays the next js script (when mode=="js").
 *   prev   — single tap plays the previous js script (when mode=="js").
 *
 * Each pin is read from NVS at boot (CONFIG_KEY_BTN_*_PIN). A pin of -1
 * disables that button entirely — no GPIO is touched, no iot_button is
 * created. The driver is safe to call when *no* button is configured.
 *
 * Buttons are assumed active-low (TTP223 capacitive boards default to
 * active-low after configuration; momentary switches go ground via a pull-up).
 */
esp_err_t buttons_init(void);
