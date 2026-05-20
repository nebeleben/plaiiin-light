#include "factory_reset.h"
#include "config_store.h"
#include "error_light.h"
#include "led_control.h"
#include "mdns_service.h"

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "factory_reset";

// Visual confirmation that the user's long-press was recognised. Two quick
// flashes in `color` so the user knows the press registered, much friendlier
// than rebooting silently. Note: while the user is *still holding* the
// button the lit indicator (held by buttons.c) stays steady; this blink runs
// after release on the way to esp_restart().
static void confirm_blink(uint8_t r, uint8_t g, uint8_t b)
{
    led_color_t color = {r, g, b};
    int n = led_control_get_count();
    led_color_t *frame = calloc(n, sizeof(led_color_t));
    if (!frame) return;
    for (int i = 0; i < n; i++) frame[i] = color;
    for (int i = 0; i < 2; i++) {
        // Snap power on/off so the flash reads as a sharp blink, not a 600 ms
        // ramp inside a 160 ms window.
        led_control_power_snap(true);
        // Transient: confirmation flash, not a user color — see paint_solid().
        led_control_set_all_transient(frame, n);
        vTaskDelay(pdMS_TO_TICKS(160));
        led_control_power_snap(false);
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    free(frame);
}

esp_err_t factory_reset_wifi(bool reboot)
{
    // Earlier this function called nvs_open("storage", …) directly, but the
    // config_store namespace is "plaiiin_cfg" — so the erase silently no-op'd
    // and lamps stayed on the same network after every "reset wifi". Both
    // reset paths now route through config_store_erase_keys, which already
    // knows the correct namespace.
    //
    // Phase 12.x: wifi-reset also releases pairing so the lamp comes back
    // claimable by whoever onboards it next. The macOS/Android client side
    // mirrors this by removing the saved row + clearing the local token.
    ESP_LOGW(TAG, "Resetting WiFi credentials + releasing pairing");
    static const char *const keys[] = {
        CONFIG_KEY_WIFI_SSID,
        CONFIG_KEY_WIFI_PASS,
        CONFIG_KEY_PAIR_TOKEN,
        CONFIG_KEY_PAIR_MODE,
        CONFIG_KEY_SHARE_KEYS,
    };
    config_store_erase_keys(keys, sizeof(keys) / sizeof(keys[0]));
    confirm_blink(0, 200, 0);   // green
    // mDNS goodbye so clients drop our cached entry instead of pinning the
    // old WiFi IP for a TTL window after we reboot into AP mode.
    mdns_service_stop();
    if (reboot) { vTaskDelay(pdMS_TO_TICKS(200)); esp_restart(); }
    return ESP_OK;
}

esp_err_t factory_reset_full(bool reboot)
{
    // "Full" is a personal-data wipe: WiFi creds, the JS that was last
    // selected for play, and the AI api key. Hardware config (lamp form/type,
    // pins, pixel-group, orientation, button mappings) is intentionally KEPT
    // so the device is still operational on the next boot — the user just
    // needs to put it back on a network and re-pair an AI key.
    ESP_LOGW(TAG, "Personal-data reset — wifi creds, current_js, ai_api_key, pairing");
    static const char *const keys[] = {
        CONFIG_KEY_WIFI_SSID,
        CONFIG_KEY_WIFI_PASS,
        CONFIG_KEY_CURRENT_JS,
        CONFIG_KEY_AI_API_KEY,
        // Pairing is per-device personal data — wipe on full reset so the
        // device returns to its default unpaired state for the next owner.
        CONFIG_KEY_PAIR_TOKEN,
        CONFIG_KEY_PAIR_MODE,
        CONFIG_KEY_SHARE_KEYS,
        // Phase 29 — wormhole render mode + per-ring config. Wiped on a full
        // reset so a wormhole lamp returns to its default strip mode with the
        // firmware-derived ring count and all-zero physical/creative config.
        CONFIG_KEY_WH_MODE,
        CONFIG_KEY_WH_RINGS,
        CONFIG_KEY_WH_PHYS,
        CONFIG_KEY_WH_CREATIVE,
    };
    config_store_erase_keys(keys, sizeof(keys) / sizeof(keys[0]));
    confirm_blink(0, 100, 255);   // blue
    mdns_service_stop();
    if (reboot) { vTaskDelay(pdMS_TO_TICKS(200)); esp_restart(); }
    return ESP_OK;
}
