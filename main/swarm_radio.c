#include "swarm_radio.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "swarm_radio";

static const uint8_t kBroadcastMac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static bool s_initialized = false;

// s_tx and s_rx each have exactly one writer context (documented per-field
// below); s_tx_fail has TWO — the synchronous esp_now_send() failure path
// (caller's task) and the async send callback's ESP_NOW_SEND_FAIL case
// (esp_now internal task). Plain `volatile` is still enough for all three:
// these are diagnostic counters, not synchronization primitives, and a
// torn/lost increment under the s_tx_fail race is a cosmetic stats blip,
// not a correctness bug — no atomics/locks needed.
static volatile uint32_t s_tx = 0;       // written by swarm_radio_send() callers
static volatile uint32_t s_rx = 0;       // written by the esp_now recv callback
static volatile uint32_t s_tx_fail = 0;  // written by send() (sync fail) + the esp_now send callback (async fail)
static uint8_t s_last_from[6] = { 0 };   // written by the esp_now recv callback

static swarm_radio_rx_cb_t s_user_rx_cb = NULL;

// PlanV3 V2.5 follow-up — the broadcast peer must live on an UP interface.
// AP-only (AP-less onboarding) → WIFI_IF_AP; STA or APSTA → WIFI_IF_STA.
static wifi_interface_t swarm_radio_iface(void)
{
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) == ESP_OK && mode == WIFI_MODE_AP) {
        return WIFI_IF_AP;
    }
    return WIFI_IF_STA;
}

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
        peer.ifidx = swarm_radio_iface();
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
    if (err == ESP_ERR_ESPNOW_IF || err == ESP_ERR_ESPNOW_NOT_FOUND) {
        // The peer's interface is down (mode changed since init) or the peer
        // itself is missing — e.g. a prior re-home's esp_now_del_peer()
        // succeeded but its esp_now_add_peer() failed, leaving no peer at
        // all. Either way: re-home the broadcast peer on the active
        // interface and retry once.
        esp_now_peer_info_t peer = {0};
        memcpy(peer.peer_addr, kBroadcastMac, sizeof(peer.peer_addr));
        peer.channel = 0;
        peer.ifidx = swarm_radio_iface();
        peer.encrypt = false;
        esp_now_del_peer(kBroadcastMac);
        if (esp_now_add_peer(&peer) == ESP_OK) {
            err = esp_now_send(kBroadcastMac, data, len);
        }
    }
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
