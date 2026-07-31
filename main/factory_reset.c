#include "factory_reset.h"
#include "config_store.h"
#include "error_light.h"
#include "frame_store.h"
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
        // Clear the sticky "provisioned" flag — a factory reset is the explicit,
        // physical way to return a previously-owned lamp to fresh AP/BLE
        // onboarding (the open provisioning AP comes back on the next boot).
        CONFIG_KEY_PROVISIONED,
        // NOTE: CONFIG_KEY_RESET_KEY is deliberately NOT wiped. The recovery
        // key is a durable secret that must survive every reset so the owner
        // can always recover — only an explicit generate/DELETE changes it.
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
    ESP_LOGW(TAG, "Personal-data reset — wifi creds, mode/colors, current_js, ai_api_key, pairing");
    static const char *const keys[] = {
        CONFIG_KEY_WIFI_SSID,
        CONFIG_KEY_WIFI_PASS,
        CONFIG_KEY_CURRENT_JS,
        // Mode and colors are personal data too — and erasing current_js
        // while keeping lamp_mode/base_color left the lamp in a mixed
        // old/new state after re-onboarding (js mode with no script, or a
        // reported color the panel never painted). Wipe the whole set so a
        // reset lamp boots into one coherent default (api mode, default
        // color, on) — main.c's boot reconcile keeps panel and /api/state
        // in agreement from the first boot.
        CONFIG_KEY_LAMP_MODE,
        CONFIG_KEY_BASE_COLOR,
        CONFIG_KEY_LAST_COLOR,
        CONFIG_KEY_POWER_ON,
        CONFIG_KEY_AI_API_KEY,
        // Pairing is per-device personal data — wipe on full reset so the
        // device returns to its default unpaired state for the next owner.
        CONFIG_KEY_PAIR_TOKEN,
        CONFIG_KEY_PAIR_MODE,
        CONFIG_KEY_SHARE_KEYS,
        // Sticky provisioning marker — see factory_reset_wifi(). A full reset
        // also returns the lamp to fresh AP/BLE onboarding.
        CONFIG_KEY_PROVISIONED,
        // NOTE: CONFIG_KEY_RESET_KEY is deliberately NOT wiped here either —
        // the recovery key survives a full reset (and the redeem that triggers
        // one), so it stays usable across ownership changes until an explicit
        // generate/DELETE. See reset_key_api.c and factory_reset_wifi() above.
        // Phase 29 — wormhole render mode + per-ring config. Wiped on a full
        // reset so a wormhole lamp returns to its default strip mode with the
        // firmware-derived ring count and all-zero physical/creative config.
        CONFIG_KEY_WH_MODE,
        CONFIG_KEY_WH_RINGS,
        CONFIG_KEY_WH_PHYS,
        CONFIG_KEY_WH_CREATIVE,
        // PlanV3 Phase V2.5 — swarm mode. Membership (id/key), the pinned
        // channel and the TX sequence counter are personal/relationship data
        // (which swarm this lamp belongs to) — wipe on full reset alongside
        // pairing. Deliberately NOT in factory_reset_wifi's list above: a
        // plain WiFi reset stays on the same physical network and should
        // keep the lamp in its swarm.
        CONFIG_KEY_SW_ID,
        CONFIG_KEY_SW_KEY,
        CONFIG_KEY_SW_CHAN,
        CONFIG_KEY_SW_ON,
        CONFIG_KEY_SW_SEQ,
    };
    config_store_erase_keys(keys, sizeof(keys) / sizeof(keys[0]));
    // The drawn frame is personal data too (Draw/frame mode) — without this
    // it survives on SPIFFS past the wipe and GET /api/frame would still
    // serve the previous owner's picture to whoever pairs the lamp next.
    frame_store_erase();
    confirm_blink(0, 100, 255);   // blue
    mdns_service_stop();
    if (reboot) { vTaskDelay(pdMS_TO_TICKS(200)); esp_restart(); }
    return ESP_OK;
}
