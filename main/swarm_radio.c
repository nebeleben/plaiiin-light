#include "swarm_radio.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "swarm_radio";

static const uint8_t kBroadcastMac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static bool s_initialized = false;

// Each counter has exactly one writer context (documented per-field below),
// so plain `volatile` is enough here — no atomics/locks needed.
static volatile uint32_t s_tx = 0;       // written by swarm_radio_send() callers
static volatile uint32_t s_rx = 0;       // written by the esp_now recv callback
static volatile uint32_t s_tx_fail = 0;  // written by send() (sync fail) + the esp_now send callback (async fail)
static uint8_t s_last_from[6] = { 0 };   // written by the esp_now recv callback

static swarm_radio_rx_cb_t s_user_rx_cb = NULL;

static void swarm_recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (!info || !data || len <= 0) return;
    s_rx++;
    if (info->src_addr) memcpy(s_last_from, info->src_addr, sizeof(s_last_from));
    if (s_user_rx_cb) s_user_rx_cb(info->src_addr, data, (size_t)len);
}

static void swarm_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    (void)mac_addr;
    if (status != ESP_NOW_SEND_SUCCESS) s_tx_fail++;
}

esp_err_t swarm_radio_init(void)
{
    if (s_initialized) return ESP_OK;

    esp_err_t err = esp_now_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_now_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_now_register_recv_cb(swarm_recv_cb);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_now_register_recv_cb failed: %s", esp_err_to_name(err));
        esp_now_deinit();
        return err;
    }

    err = esp_now_register_send_cb(swarm_send_cb);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_now_register_send_cb failed: %s", esp_err_to_name(err));
        esp_now_deinit();
        return err;
    }

    if (!esp_now_is_peer_exist(kBroadcastMac)) {
        esp_now_peer_info_t peer = {0};
        memcpy(peer.peer_addr, kBroadcastMac, sizeof(peer.peer_addr));
        peer.channel = 0;              // current channel (STA's association)
        peer.ifidx = WIFI_IF_STA;
        peer.encrypt = false;
        err = esp_now_add_peer(&peer);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_now_add_peer(broadcast) failed: %s", esp_err_to_name(err));
            esp_now_deinit();
            return err;
        }
    }

    s_initialized = true;
    ESP_LOGI(TAG, "swarm_radio initialized (ESP-NOW broadcast peer ready)");
    return ESP_OK;
}

esp_err_t swarm_radio_send(const uint8_t *data, size_t len)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (!data || len == 0 || len > 250) return ESP_ERR_INVALID_SIZE;

    s_tx++;
    esp_err_t err = esp_now_send(kBroadcastMac, data, len);
    if (err != ESP_OK) {
        // Synchronous failure (e.g. internal queue full) — the send callback
        // never fires for a packet that was never queued, so count it here.
        s_tx_fail++;
        ESP_LOGW(TAG, "esp_now_send failed: %s", esp_err_to_name(err));
    }
    return err;
}

void swarm_radio_set_rx(swarm_radio_rx_cb_t cb)
{
    s_user_rx_cb = cb;
}

void swarm_radio_stats(uint32_t *tx, uint32_t *rx, uint32_t *tx_fail)
{
    if (tx) *tx = s_tx;
    if (rx) *rx = s_rx;
    if (tx_fail) *tx_fail = s_tx_fail;
}

void swarm_radio_last_from(uint8_t mac[6])
{
    memcpy(mac, s_last_from, 6);
}
