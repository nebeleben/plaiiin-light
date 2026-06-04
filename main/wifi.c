#include "wifi.h"
#include "config_store.h"
#include "pairing.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include <string.h>

static const char *TAG = "wifi";
static plaiiin_wifi_mode_t s_mode = PLAIIIN_WIFI_NONE;
static bool s_connected = false;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        ESP_LOGW(TAG, "Disconnected, retrying...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_connected = true;
    }
}

static esp_err_t start_sta(void)
{
    char ssid[64] = {0};
    char pass[64] = {0};
    config_store_get_str(CONFIG_KEY_WIFI_SSID, ssid, sizeof(ssid));
    config_store_get_str(CONFIG_KEY_WIFI_PASS, pass, sizeof(pass));

    esp_netif_create_default_wifi_sta();

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    s_mode = PLAIIIN_WIFI_STA;
    ESP_LOGI(TAG, "STA mode, connecting to '%s' (power save off)", ssid);
    return ESP_OK;
}

static esp_err_t start_ap(void)
{
    esp_netif_create_default_wifi_ap();

    // Build SSID = <node_name>-<MAC suffix>. node_name comes from NVS so a
    // profile-burned lamp ("tower8v2") shows up as "tower8v2-3FA8" instead of
    // a generic "PlaiiinLight-3FA8". Falls back to the Kconfig prefix on a
    // brand-new chip with no profile yet.
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char node[24];
    config_get_str_or(CONFIG_KEY_NODE_NAME, node, sizeof(node),
                      CONFIG_PLAIIIN_WIFI_AP_SSID_PREFIX);
    char ssid[32];
    snprintf(ssid, sizeof(ssid), "%s-%02X%02X", node, mac[4], mac[5]);

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.ap.ssid, ssid, sizeof(wifi_config.ap.ssid) - 1);
    wifi_config.ap.ssid_len = strlen(ssid);
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.channel = 1;

    const char *ap_pass = CONFIG_PLAIIIN_WIFI_AP_PASSWORD;
    if (strlen(ap_pass) >= 8) {
        strncpy((char *)wifi_config.ap.password, ap_pass, sizeof(wifi_config.ap.password) - 1);
        wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_mode = PLAIIIN_WIFI_AP;
    ESP_LOGI(TAG, "AP mode started: '%s'", ssid);
    return ESP_OK;
}

// Tear down the provisioning SoftAP at runtime. Called when a lamp is claimed
// over BLE (it no longer needs an open captive portal anyone in range could
// reach). No-op unless we're actually in AP mode. Soft-fails rather than
// ESP_ERROR_CHECK so a hiccup here never crashes a freshly-claimed lamp.
esp_err_t wifi_provisioning_ap_stop(void)
{
    if (s_mode != PLAIIIN_WIFI_AP) return ESP_OK;
    ESP_LOGI(TAG, "stopping provisioning AP (lamp claimed over BLE)");
    esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_stop err=%s", esp_err_to_name(err));
        return err;
    }
    esp_wifi_set_mode(WIFI_MODE_NULL);
    s_mode = PLAIIIN_WIFI_NONE;
    return ESP_OK;
}

esp_err_t wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    if (config_store_has_wifi()) {
        return start_sta();
    } else if (pairing_is_paired() || pairing_is_provisioned()) {
        // No WiFi creds, but the lamp is either currently claimed OR was claimed
        // before and then released from the owner's app. Either way it must NOT
        // bring up the open provisioning AP: that AP is unauthenticated (the
        // captive portal grants admin to anyone in radio range), so reopening it
        // on unpair would silently expose full control to a passer-by. The lamp
        // keeps advertising over BLE so the owner can drive / re-claim it; only a
        // factory reset (which clears CONFIG_KEY_PROVISIONED) returns it to AP
        // onboarding. See SECURITY.md.
        ESP_LOGI(TAG, "no WiFi creds but provisioned/paired — skipping AP (BLE-only)");
        s_mode = PLAIIIN_WIFI_NONE;
        return ESP_OK;
    } else {
        return start_ap();
    }
}

plaiiin_wifi_mode_t wifi_get_mode(void)
{
    return s_mode;
}

bool wifi_is_connected(void)
{
    return s_connected;
}
