#include "config_store.h"
#include "wifi.h"
#include "http_server.h"
#include "led_control.h"
#include "error_light.h"
#include "plaiiin_mqtt.h"
#include "js_player.h"
#include "js_storage.h"
#include "js_api.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "plaiiinlight_os";

void app_main(void)
{
    // 1. Init NVS config store (must be first)
    ESP_ERROR_CHECK(config_store_init());

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

    // 5. Init WiFi (STA or AP based on saved config)
    ESP_ERROR_CHECK(wifi_init());

    if (wifi_get_mode() == PLAIIIN_WIFI_AP) {
        error_light_set(ERROR_LIGHT_AP_MODE);
    }

    // 6. Init JS storage + player
    if (js_storage_init() != ESP_OK) {
        ESP_LOGW(TAG, "JS storage init failed — /api/js endpoints disabled");
    }
    js_player_init();

    // 7. Start HTTP server (registers JS API inside)
    httpd_handle_t server = http_server_start();
    if (!server) {
        ESP_LOGE(TAG, "Failed to start HTTP server!");
        error_light_set(ERROR_LIGHT_CONFIG_ERROR);
    } else {
        js_api_register(server);
    }

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
            // 8. Start MQTT if configured
            mqtt_client_start();
        } else {
            ESP_LOGW(TAG, "WiFi connection timeout");
            error_light_set(ERROR_LIGHT_NO_WIFI);
        }
    }

    ESP_LOGI(TAG, "PlaiiinLightOS ready.");
}
