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
// Persistent lamp mode. "api" (default — direct color via /api/color) or
// "js" (plays the script named in CONFIG_KEY_CURRENT_JS in a loop). The
// websocket "stream" mode is volatile and not stored here.
#define CONFIG_KEY_LAMP_MODE     "lamp_mode"
// Last-played JS script name (no .js suffix). Picked back up on boot when
// lamp_mode == "js" and reused by /api/play/next|prev.
#define CONFIG_KEY_CURRENT_JS    "current_js"
// Latest base color set via /api/color, packed as 0x00RRGGBB. Passed as the
// 4th arg to render() so JS animations can tint themselves to whatever HA
// (or any other client) most recently asked for.
#define CONFIG_KEY_BASE_COLOR    "base_color"
// Hardware buttons (Phase 8). Each is a GPIO number; -1 disables. The driver
// inits an iot_button per pin only when the value is >= 0, so leaving these
// at -1 has zero runtime cost on devices without buttons wired.
#define CONFIG_KEY_BTN_PWR_PIN   "btn_pwr_pin"
#define CONFIG_KEY_BTN_NEXT_PIN  "btn_next_pin"
#define CONFIG_KEY_BTN_PREV_PIN  "btn_prev_pin"
// AI provider API key for the on-device /compose page. Lives in NVS so a
// factory reset can wipe it. The /api/ai/key endpoint never returns the
// raw key on read — only a has-key boolean — so a snooper can't pull it
// off a device just because it sits on the same network.
#define CONFIG_KEY_AI_API_KEY    "ai_api_key"
// Phase 9 — pairing. `pair_mode` is "unpaired" (default; everything works
// like before) or "paired" (HTTP requests need Authorization: Bearer
// <token>, BLE writes need an encrypted/bonded link). `pair_token` is a
// 32-byte secret rendered as URL-safe base64. Wiped by factory_reset_full.
#define CONFIG_KEY_PAIR_MODE     "pair_mode"
#define CONFIG_KEY_PAIR_TOKEN    "pair_token"
// Phase 27 — role-based sharing. JSON array of share-key entries
// [{"id","key","role":"user|creator","label","revoked","created"}], stored
// as one NVS string. The admin key stays in CONFIG_KEY_PAIR_TOKEN; these are
// the additional limited-role keys an admin hands out. Wiped by factory
// reset alongside the pair token. See keys.{h,c}.
#define CONFIG_KEY_SHARE_KEYS    "share_keys"
// Bluetooth lifecycle policy:
//   "auto"   — BT advertises only when WiFi is unconfigured or fails to
//              associate within ~15s after boot. Once WiFi is up, BT shuts
//              down to free RAM and reduce surface area.
//   "always" — BT advertises whenever the device is on.
//   "never"  — BT stack is not initialized at all.
// Default "auto".
#define CONFIG_KEY_BT_ENABLED    "bt_enabled"
// Phase 26 — per-lamp physical-form descriptor. Optional free-text override
// for the firmware-generated form description that clients inject into AI
// compose prompts. Unset (the default) means "use the firmware default built
// from lamp_form + geometry". Wiped by factory reset along with the rest of
// the namespace. See form_prompt.{h,c}.
#define CONFIG_KEY_FORM_PROMPT   "form_prompt"
// Phase 33 — name of the JS script played on the LEDs while the lamp is in
// AP / onboarding mode (no WiFi credentials). Defined per profile so each
// lamp form ships its own onboarding-look. Empty (the default) means
// "fall back to the built-in blue pulse on the first 3 LEDs". See main.c's
// AP-mode branch. The configured script is loaded from SPIFFS and played
// looped; if missing or won't compile the fallback also kicks in.
#define CONFIG_KEY_AP_JS         "ap_js"
// Phase 29 — wormhole lamp render mode. Only meaningful when lamp_form ==
// "wormhole"; ignored by every other form. Wiped by factory reset alongside
// the rest of the namespace. See wormhole.{h,c} and docs/wormhole-api.md.
//   wh_mode     — string, "strip" (default, out-of-the-box) or "mirror". In
//                 strip mode the effect renders the whole construct; in
//                 mirror mode it renders one 24-LED ring and firmware tiles
//                 it onto every physical ring.
//   wh_rings    — i32, explicit ring count. Default led_count / 24 (v1=2, v2=4).
//   wh_phys     — JSON array string, one object per ring of set-once mounting
//                 facts {"face","direction","offset"}. share_keys pattern —
//                 a JSON array packed into one NVS string. Default all-zero.
//   wh_creative — JSON array string, one object per ring of per-lamp creative
//                 knobs {"reverse","offset","brightness"} (mirror mode only).
//                 Default {reverse:false, offset:0, brightness:1.0} per ring.
#define CONFIG_KEY_WH_MODE       "wh_mode"
#define CONFIG_KEY_WH_RINGS      "wh_rings"
#define CONFIG_KEY_WH_PHYS       "wh_phys"
#define CONFIG_KEY_WH_CREATIVE   "wh_creative"

esp_err_t config_store_init(void);
esp_err_t config_store_get_str(const char *key, char *out, size_t max_len);
esp_err_t config_store_set_str(const char *key, const char *value);
esp_err_t config_store_get_i32(const char *key, int32_t *out);
esp_err_t config_store_set_i32(const char *key, int32_t value);

/** Erase one or more keys from the config namespace, then commit. Used by
 *  factory_reset to wipe selective fields (wifi, pair token, …) without
 *  touching the rest. ESP_ERR_NVS_NOT_FOUND for individual keys is treated
 *  as success — already gone is the same as just gone. */
esp_err_t config_store_erase_keys(const char *const *keys, int count);

bool config_store_has_wifi(void);

/** Read string from NVS; if not set, copy fallback into out. */
void config_get_str_or(const char *key, char *out, size_t max_len, const char *fallback);
/** Read int from NVS; if not set, return fallback. */
int32_t config_get_i32_or(const char *key, int32_t fallback);
