#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * NVS-backed configuration store.
 * All keys fall back to Kconfig defaults if not set.
 */

#define CONFIG_KEY_WIFI_SSID     "wifi_ssid"
#define CONFIG_KEY_WIFI_PASS     "wifi_pass"
#define CONFIG_KEY_NODE_NAME     "node_name"
#define CONFIG_KEY_VENDOR_NAME   "vendor_name"
#define CONFIG_KEY_API_VERSION   "api_version"
#define CONFIG_KEY_LED_PIN       "led_pin"
#define CONFIG_KEY_LED_CLK_PIN   "led_clk_pin"
#define CONFIG_KEY_LED_COUNT     "led_count"
#define CONFIG_KEY_LED_TYPE      "led_type"
#define CONFIG_KEY_LAMP_TYPE     "lamp_type"
#define CONFIG_KEY_LAMP_FORM     "lamp_form"
#define CONFIG_KEY_MQTT_ACTIVE   "mqtt_active"
#define CONFIG_KEY_MQTT_HOST     "mqtt_host"
#define CONFIG_KEY_MQTT_PORT     "mqtt_port"
#define CONFIG_KEY_PX_GROUP_W    "px_group_w"
#define CONFIG_KEY_PX_GROUP_H    "px_group_h"
#define CONFIG_KEY_ROTATION      "rotation"
// Origin corner of the *physical* LED chain on a matrix panel.
// 0=TL, 1=TR, 2=BL, 3=BR. Default 0 (top-left).
#define CONFIG_KEY_ORIGIN        "origin"
// Whether wiring zig-zags between rows/cols (1) or all rows/cols start
// from the same edge (0). Default 1 — common for WS2812 matrices.
#define CONFIG_KEY_SERPENTINE    "serpentine"
// Primary axis the chain runs along on a matrix panel.
// 0 = horizontal (chain advances along X within a row, then steps in Y),
// 1 = vertical   (chain advances along Y within a column, then steps in X).
// Default 0.
#define CONFIG_KEY_SERP_AXIS     "serp_axis"

esp_err_t config_store_init(void);
esp_err_t config_store_get_str(const char *key, char *out, size_t max_len);
esp_err_t config_store_set_str(const char *key, const char *value);
esp_err_t config_store_get_i32(const char *key, int32_t *out);
esp_err_t config_store_set_i32(const char *key, int32_t value);
bool config_store_has_wifi(void);

/** Read string from NVS; if not set, copy fallback into out. */
void config_get_str_or(const char *key, char *out, size_t max_len, const char *fallback);
/** Read int from NVS; if not set, return fallback. */
int32_t config_get_i32_or(const char *key, int32_t fallback);
