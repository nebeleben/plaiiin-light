#pragma once

/**
 * Factory reset over power plug (PlanV3 Phase V2.1).
 *
 * Counts consecutive "fast" power-on boots in NVS (CONFIG_KEY_PLUG_CNT).
 * A boot that stays powered for 10 s clears the streak; the 5th fast
 * power-cycle in a row triggers factory_reset_full() — blue flash,
 * personal-data wipe, hardware identity + recovery key kept.
 *
 * Only ESP_RST_POWERON boots count. Software resets, panics, watchdogs
 * and brownouts never increment the counter, so crash loops and mains
 * flicker cannot wipe a lamp.
 *
 * Call once from app_main() after led_control_init() + error_light_init()
 * (the confirmation blink needs working LEDs). May not return: on the
 * triggering boot it reboots inside factory_reset_full(true).
 */
void plug_reset_check(void);
