#include "config_store.h"
#include "wifi.h"
#include "http_server.h"
#include "led_control.h"
#include "error_light.h"
#include "plaiiin_mqtt.h"
#include "js_player.h"
#include "js_storage.h"
#include "plbc.h"
#include <stdlib.h>
#include "js_api.h"
#include "mdns_service.h"
#include "bt_service.h"
#include "buttons.h"
#include "ai_key_api.h"
#include "reset_key_api.h"
#include "pairing.h"
#include "keys.h"
#include "wormhole.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "plaiiinlight_os";

/* Install the embedded default scripts onto SPIFFS if missing, and compile
 * a .bc for each so the player can run them immediately. Idempotent — if a
 * script is already on disk the slot is skipped (the user is free to edit
 * built-ins in place, and a firmware upgrade respects those edits). Wipe
 * via /api/js DELETE to get the next reinstall on reboot.
 *
 * As of Phase 36 only `noop` is firmware-embedded — the rest of the former
 * built-ins (fade, plasma, breath, heartbeat, shootingstar, particles,
 * sinwave, blaze) live in effects/default/ and get SPIFFS-flashed by
 * `profile-burn.sh --full` alongside the form-specific effects. Devices
 * that have never been --full-burned will therefore only have `noop` on
 * SPIFFS, and AP-mode's ap_js="breath" will hit the 3-LED fallback. */
static void install_default_scripts(void)
{
    extern const uint8_t noop_js_start[]         asm("_binary_noop_js_start");
    extern const uint8_t noop_js_end[]           asm("_binary_noop_js_end");
    struct { const char *name; const uint8_t *start; const uint8_t *end; } defaults[] = {
        { "noop",         noop_js_start,         noop_js_end },
    };
    for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++) {
        if (js_storage_exists(defaults[i].name)) continue;
        size_t src_len = defaults[i].end - defaults[i].start;
        /* EMBED_TXTFILES appends a NUL terminator — drop it so the stored
         * .js is clean source and the compiler doesn't see a stray '\0'. */
        while (src_len > 0 && defaults[i].start[src_len - 1] == 0) src_len--;
        esp_err_t werr = js_storage_write(defaults[i].name,
                                          (const char *)defaults[i].start,
                                          src_len);
        if (werr != ESP_OK) {
            ESP_LOGW(TAG, "Could not install %s.js: %s", defaults[i].name,
                     esp_err_to_name(werr));
            continue;
        }
        plbc_program_t *prog = (plbc_program_t *)calloc(1, sizeof(*prog));
        if (!prog) {
            ESP_LOGW(TAG, "OOM seeding %s.bc", defaults[i].name);
            continue;
        }
        char cerr[128] = {0};
        if (plbc_compile((const char *)defaults[i].start, src_len, prog,
                         cerr, sizeof(cerr)) != ESP_OK) {
            ESP_LOGW(TAG, "compile %s.js: %s", defaults[i].name, cerr);
            free(prog);
            continue;
        }
        /* Heap allocation — main_task stack is 3.5 KB, this buffer is up to
         * 8 KB. Stack-allocating it would crash on first boot. */
        uint8_t *bc = (uint8_t *)malloc(8192);
        if (!bc) { free(prog); ESP_LOGW(TAG, "OOM bc buf"); continue; }
        int n = plbc_serialize(prog, bc, 8192);
        free(prog);
        if (n <= 0) {
            free(bc);
            ESP_LOGW(TAG, "serialize %s.bc failed", defaults[i].name);
            continue;
        }
        if (js_storage_write_bc(defaults[i].name, bc, (size_t)n) == ESP_OK) {
            ESP_LOGI(TAG, "Installed default %s.js + .bc (%d B)", defaults[i].name, n);
        }
        free(bc);
    }
}

void app_main(void)
{
    // 1. Init NVS config store (must be first)
    ESP_ERROR_CHECK(config_store_init());
    pairing_init();   // reads pair_mode + caches; safe before WiFi/HTTP.
    keys_init();      // logs the stored share-key count.

    // 1b. Seed NVS from Kconfig for keys that are still unset. This freezes
    // the values reported by /api into NVS, so subsequent OTAs (which only
    // touch the app partition, not NVS) can no longer "move" them if a later
    // firmware changes its Kconfig fallbacks. Only seeds missing keys — never
    // overwrites user-set values.
    {
        char existing[64];
        // strings
        if (config_store_get_str(CONFIG_KEY_NODE_NAME, existing, sizeof(existing)) != ESP_OK)
            config_store_set_str(CONFIG_KEY_NODE_NAME, CONFIG_PLAIIIN_NODE_NAME);
        if (config_store_get_str(CONFIG_KEY_VENDOR_NAME, existing, sizeof(existing)) != ESP_OK)
            config_store_set_str(CONFIG_KEY_VENDOR_NAME, CONFIG_PLAIIIN_VENDOR_NAME);
        if (config_store_get_str(CONFIG_KEY_LED_TYPE, existing, sizeof(existing)) != ESP_OK)
            config_store_set_str(CONFIG_KEY_LED_TYPE, CONFIG_PLAIIIN_LED_TYPE);
        if (config_store_get_str(CONFIG_KEY_LAMP_TYPE, existing, sizeof(existing)) != ESP_OK)
            config_store_set_str(CONFIG_KEY_LAMP_TYPE, CONFIG_PLAIIIN_LAMP_TYPE);
        if (config_store_get_str(CONFIG_KEY_LAMP_FORM, existing, sizeof(existing)) != ESP_OK)
            config_store_set_str(CONFIG_KEY_LAMP_FORM, CONFIG_PLAIIIN_FORM);
        // ints
        int32_t tmp;
        if (config_store_get_i32(CONFIG_KEY_LED_PIN, &tmp) != ESP_OK)
            config_store_set_i32(CONFIG_KEY_LED_PIN, CONFIG_PLAIIIN_LED_PIN);
        if (config_store_get_i32(CONFIG_KEY_LED_CLK_PIN, &tmp) != ESP_OK)
            config_store_set_i32(CONFIG_KEY_LED_CLK_PIN, CONFIG_PLAIIIN_LED_CLK_PIN);
        if (config_store_get_i32(CONFIG_KEY_LED_COUNT, &tmp) != ESP_OK)
            config_store_set_i32(CONFIG_KEY_LED_COUNT, CONFIG_PLAIIIN_LED_COUNT);
        // Phase 8 — buttons (each pin -1 means "not wired")
        if (config_store_get_i32(CONFIG_KEY_BTN_PWR_PIN,  &tmp) != ESP_OK)
            config_store_set_i32(CONFIG_KEY_BTN_PWR_PIN,  CONFIG_PLAIIIN_BTN_PWR_PIN);
        if (config_store_get_i32(CONFIG_KEY_BTN_NEXT_PIN, &tmp) != ESP_OK)
            config_store_set_i32(CONFIG_KEY_BTN_NEXT_PIN, CONFIG_PLAIIIN_BTN_NEXT_PIN);
        if (config_store_get_i32(CONFIG_KEY_BTN_PREV_PIN, &tmp) != ESP_OK)
            config_store_set_i32(CONFIG_KEY_BTN_PREV_PIN, CONFIG_PLAIIIN_BTN_PREV_PIN);
        // Pixel grouping + orientation. Seed-only (Kconfig defaults never
        // stomp values the user may have tweaked at runtime via /api).
        if (config_store_get_i32(CONFIG_KEY_PX_GROUP_W, &tmp) != ESP_OK)
            config_store_set_i32(CONFIG_KEY_PX_GROUP_W, CONFIG_PLAIIIN_PX_GROUP_W);
        if (config_store_get_i32(CONFIG_KEY_PX_GROUP_H, &tmp) != ESP_OK)
            config_store_set_i32(CONFIG_KEY_PX_GROUP_H, CONFIG_PLAIIIN_PX_GROUP_H);
        if (config_store_get_i32(CONFIG_KEY_ROTATION,   &tmp) != ESP_OK)
            config_store_set_i32(CONFIG_KEY_ROTATION,   CONFIG_PLAIIIN_ROTATION);
        if (config_store_get_i32(CONFIG_KEY_ORIGIN,     &tmp) != ESP_OK)
            config_store_set_i32(CONFIG_KEY_ORIGIN,     CONFIG_PLAIIIN_ORIGIN);
        if (config_store_get_i32(CONFIG_KEY_SERPENTINE, &tmp) != ESP_OK)
            config_store_set_i32(CONFIG_KEY_SERPENTINE, CONFIG_PLAIIIN_SERPENTINE);
        if (config_store_get_i32(CONFIG_KEY_SERP_AXIS,  &tmp) != ESP_OK)
            config_store_set_i32(CONFIG_KEY_SERP_AXIS,  CONFIG_PLAIIIN_SERP_AXIS);
    }

    // 1c. Same for factory WiFi creds (only if the build carries them and NVS
    // is empty). Runtime changes via /network still win afterwards.
    if (!config_store_has_wifi()) {
        const char *ssid = CONFIG_PLAIIIN_NETWORK_SSID;
        const char *pw   = CONFIG_PLAIIIN_NETWORK_PW;
        if (ssid && ssid[0]) {
            config_store_set_str(CONFIG_KEY_WIFI_SSID, ssid);
            config_store_set_str(CONFIG_KEY_WIFI_PASS, pw ? pw : "");
            ESP_LOGI(TAG, "Seeded factory WiFi creds (ssid=%s)", ssid);
        }
    }

    // 2. Read runtime config (NVS overrides Kconfig defaults)
    char node_name[64], vendor[64], api_ver[32];
    config_get_str_or(CONFIG_KEY_NODE_NAME, node_name, sizeof(node_name), CONFIG_PLAIIIN_NODE_NAME);
    config_get_str_or(CONFIG_KEY_VENDOR_NAME, vendor, sizeof(vendor), CONFIG_PLAIIIN_VENDOR_NAME);
    config_get_str_or(CONFIG_KEY_API_VERSION, api_ver, sizeof(api_ver), CONFIG_PLAIIIN_API_VERSION);
    int32_t led_pin = config_get_i32_or(CONFIG_KEY_LED_PIN, CONFIG_PLAIIIN_LED_PIN);
    int32_t led_clk_pin = config_get_i32_or(CONFIG_KEY_LED_CLK_PIN, CONFIG_PLAIIIN_LED_CLK_PIN);
    int32_t led_count = config_get_i32_or(CONFIG_KEY_LED_COUNT, CONFIG_PLAIIIN_LED_COUNT);
    char led_type[16] = {0};
    config_get_str_or(CONFIG_KEY_LED_TYPE, led_type, sizeof(led_type), CONFIG_PLAIIIN_LED_TYPE);

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " PlaiiinLightOS starting...");
    ESP_LOGI(TAG, " Node: %s", node_name);
    ESP_LOGI(TAG, " Vendor: %s  API: %s", vendor, api_ver);
    ESP_LOGI(TAG, " LEDs: %ld %s on data GPIO %ld (clk %ld)",
             (long)led_count, led_type, (long)led_pin, (long)led_clk_pin);
    ESP_LOGI(TAG, "========================================");

    // 3. Init LED strip with runtime config
    ESP_ERROR_CHECK(led_control_init((int)led_pin, (int)led_clk_pin, (int)led_count, led_type));

    // 3b. Derive physical grid from lamp_type ("matrixWxH" → WxH, else Nx1 strip).
    char lamp_type_str[32] = {0};
    config_get_str_or(CONFIG_KEY_LAMP_TYPE, lamp_type_str, sizeof(lamp_type_str), CONFIG_PLAIIIN_LAMP_TYPE);
    int phys_w = (int)led_count, phys_h = 1;
    if (strncmp(lamp_type_str, "matrix", 6) == 0) {
        sscanf(lamp_type_str + 6, "%dx%d", &phys_w, &phys_h);
    }
    led_control_set_physical_grid(phys_w, phys_h);
    ESP_LOGI(TAG, " Grid: %dx%d, logical %dx%d (group %dx%d)",
             phys_w, phys_h,
             led_control_get_logical_w(), led_control_get_logical_h(),
             led_control_get_pixel_group_w(), led_control_get_pixel_group_h());

    // 4. Init error light system
    error_light_init();

    // 5. Init WiFi (STA or AP based on saved config). AP-mode LED setup is
    //    deferred to step 6a — it needs js_player_init to have run so we can
    //    try to play the configured onboarding script.
    ESP_ERROR_CHECK(wifi_init());

    // 5b. Bring up BLE. Lifecycle policy is read from NVS:
    //     "auto"   → starts now, gets torn down once WiFi associates;
    //     "always" → stays up;
    //     "never"  → no-op.
    // Defaults to "auto", which is what onboarding expects (lamp is reachable
    // over BLE during the WiFi-creds handoff).
    if (bt_service_start() != ESP_OK) {
        ESP_LOGW(TAG, "BT init failed — onboarding falls back to AP captive portal");
    }

    // 6. Init JS storage + player
    if (js_storage_init() != ESP_OK) {
        ESP_LOGW(TAG, "JS storage init failed — /api/js endpoints disabled");
    }
    js_player_init();

    // Phase 29 — load the wormhole render mode + per-ring config. Applies the
    // boot fallback (mirror -> strip when the geometry gate fails). A cheap
    // no-op for every non-wormhole lamp. Must run after led_control_init so
    // it sees the real led_count.
    wormhole_reload();

    // Phase 33 — AP-mode onboarding indicator. If we're in AP mode try to
    // play the configured ap_js script; on success we also cap brightness
    // to ~30 % so the lamp doesn't run at full blast for hours during
    // onboarding. If no script is configured, the script is missing, or it
    // won't compile, fall back to the built-in blue pulse on LEDs 0..2.
    //
    // Phase 36 — the default-scripts installer (former step 6a) runs INSIDE
    // this block, before the play attempt. On a fresh boot the embedded
    // built-ins (breath, fade, ...) aren't on SPIFFS yet, so without this
    // ordering ap_js="breath" always failed with "not found" and the lamp
    // only ever showed the 3-LED fallback blink. Installing first means the
    // configured AP script is present when we try to play it.
    install_default_scripts();
    if (wifi_get_mode() == PLAIIIN_WIFI_AP) {
        char ap_js_name[64] = {0};
        config_get_str_or(CONFIG_KEY_AP_JS, ap_js_name, sizeof(ap_js_name), "");
        bool played = false;
        if (ap_js_name[0]) {
            char *src = NULL;
            size_t src_len = 0;
            if (js_storage_read(ap_js_name, &src, &src_len) == ESP_OK && src) {
                if (js_player_start(src, JS_DEFAULT_FPS) == ESP_OK) {
                    js_player_set_current_name(ap_js_name);
                    led_control_set_brightness_override(75); // ~30 % of 255
                    played = true;
                    ESP_LOGI(TAG, "AP mode: playing '%s' as onboarding indicator", ap_js_name);
                } else {
                    ESP_LOGW(TAG, "AP mode: ap_js '%s' won't start — using fallback", ap_js_name);
                }
                free(src);
            } else {
                ESP_LOGW(TAG, "AP mode: ap_js '%s' not found on SPIFFS — using fallback", ap_js_name);
            }
        }
        if (!played) {
            error_light_set(ERROR_LIGHT_AP_MODE);
        }
    }

    /* Default scripts are installed in install_default_scripts() above, just
     * before the AP-mode play attempt, so ap_js can find its script on a
     * fresh boot. Nothing to do here anymore. */

    // 6a-bis. Phase 25 — compile any stored .js that has no .bc yet. byForm
    // effects are flashed into the storage partition as raw .js by
    // scripts/profile-burn.sh (--full) and arrive without bytecode; the
    // player only runs .bc. This pass also self-heals any script left
    // .js-only by an older firmware. The general built-ins seeded above
    // already have their .bc, so they're skipped here.
    //
    // Phase 41 — the check is bc_current(), not bc_exists(): a firmware
    // upgrade that bumps the PLBC .bc format leaves stale-version bytecode the
    // player can't load, so we recompile any .bc whose magic/version no longer
    // match. Same-version .bc are skipped (no rebuild churn on normal boots).
    {
        char (*names)[64] = (char (*)[64])malloc(32 * 64);
        if (!names) {
            ESP_LOGW(TAG, "OOM — skipping uncompiled-effect pass");
        } else {
            int n = js_storage_collect_sorted(names, 32);
            for (int i = 0; i < n; i++) {
                if (js_storage_bc_current(names[i])) continue;
                char *src = NULL;
                size_t src_len = 0;
                if (js_storage_read(names[i], &src, &src_len) != ESP_OK || !src) {
                    ESP_LOGW(TAG, "compile pass: cannot read %s.js", names[i]);
                    continue;
                }
                plbc_program_t *prog = (plbc_program_t *)calloc(1, sizeof(*prog));
                if (!prog) { free(src); ESP_LOGW(TAG, "OOM compiling %s", names[i]); continue; }
                char cerr[128] = {0};
                if (plbc_compile(src, src_len, prog, cerr, sizeof(cerr)) != ESP_OK) {
                    ESP_LOGW(TAG, "compile pass: %s.js rejected: %s", names[i], cerr);
                    free(prog); free(src);
                    continue;
                }
                free(src);
                uint8_t *bc = (uint8_t *)malloc(8192);
                if (!bc) { free(prog); ESP_LOGW(TAG, "OOM bc buf"); continue; }
                int bn = plbc_serialize(prog, bc, 8192);
                free(prog);
                if (bn <= 0) {
                    free(bc);
                    ESP_LOGW(TAG, "serialize %s.bc failed", names[i]);
                    continue;
                }
                if (js_storage_write_bc(names[i], bc, (size_t)bn) == ESP_OK) {
                    ESP_LOGI(TAG, "Compiled %s.js -> .bc (%d B)", names[i], bn);
                }
                free(bc);
            }
            free(names);
        }
    }

    // 6b. Restore base color from NVS (packed 0x00RRGGBB) so JS scripts see
    // whatever color HA last set, even after a reboot.
    {
        int32_t packed = config_get_i32_or(CONFIG_KEY_BASE_COLOR, -1);
        if (packed >= 0) {
            js_player_set_base_color((uint8_t)((packed >> 16) & 0xFF),
                                     (uint8_t)((packed >> 8)  & 0xFF),
                                     (uint8_t)( packed        & 0xFF));
        }
    }

    // 6c. Restore the persisted on/off state so a reboot or power-loss brings
    // the lamp back the way the user left it. Default ON — the common case and
    // backward-compatible with lamps burned before this key existed.
    bool want_on = config_get_i32_or(CONFIG_KEY_POWER_ON, 1) != 0;

    // 6d. If the persisted mode is "js" and a script is selected, resume it —
    // but only actually start the player (which lights the panel) when the
    // lamp should be on. When it was left off we leave the player idle and
    // the script name in place; the next power-on resumes it via
    // light_api_apply_power() -> start_current_js().
    {
        char mode[16] = {0};
        config_get_str_or(CONFIG_KEY_LAMP_MODE, mode, sizeof(mode), "api");
        if (strcmp(mode, "js") == 0) {
            char name[64] = {0};
            config_get_str_or(CONFIG_KEY_CURRENT_JS, name, sizeof(name), "");
            if (!name[0]) {
                // Fall back to noop so the lamp does *something* in js mode.
                snprintf(name, sizeof(name), "noop");
                config_store_set_str(CONFIG_KEY_CURRENT_JS, name);
            }
            if (want_on) {
                char *src = NULL; size_t len = 0;
                if (js_storage_read(name, &src, &len) == ESP_OK) {
                    if (js_player_start(src, JS_DEFAULT_FPS) == ESP_OK) {
                        js_player_set_current_name(name);
                        ESP_LOGI(TAG, "Resumed js mode with %s", name);
                    }
                    free(src);
                }
            }
        }
    }

    // 6e. Settle the panel to the restored on/off state. js-mode-on already lit
    // up via the player above; snap the rest into place: force off when left
    // off, or snap on for api mode (which has no player) when left on.
    if (!want_on) {
        led_control_power_snap(false);
    } else if (!led_control_is_on()) {
        led_control_power_snap(true);
    }

    // 7. Start HTTP server (registers JS API inside)
    httpd_handle_t server = http_server_start();
    if (!server) {
        ESP_LOGE(TAG, "Failed to start HTTP server!");
        error_light_set(ERROR_LIGHT_CONFIG_ERROR);
    } else {
        js_api_register(server);
        ai_key_api_register(server);
        reset_key_api_register(server);
        keys_api_register(server);
    }

    // 7b. Hardware buttons (no-op when no pins configured).
    buttons_init();

    // 7. Wait for WiFi connection (if in STA mode)
    if (wifi_get_mode() == PLAIIIN_WIFI_STA) {
        ESP_LOGI(TAG, "Waiting for WiFi connection...");
        int attempts = 0;
        while (!wifi_is_connected() && attempts < 30) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            attempts++;
        }
        if (wifi_is_connected()) {
            ESP_LOGI(TAG, "WiFi connected!");
            error_light_clear();
            // mDNS responder — needs a network interface, so it's started after WiFi.
            if (mdns_service_start() != ESP_OK) {
                ESP_LOGW(TAG, "mDNS start failed — discovery from app will use fallback scan");
            }
            // 8. Start MQTT if configured
            mqtt_client_start();
            // BT in "auto" mode hands off to WiFi once we're online.
            bt_service_notify_wifi_connected();
        } else {
            ESP_LOGW(TAG, "WiFi connection timeout");
            error_light_set(ERROR_LIGHT_NO_WIFI);
        }
    }

    ESP_LOGI(TAG, "PlaiiinLightOS ready.");
}
