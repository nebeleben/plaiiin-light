#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

/**
 * swarm_radio — ESP-NOW broadcast transport (PlanV3 V2.5 "swarm-firmware",
 * Task 1 spike).
 *
 * Broadcasts raw frames (<=250 B) to FF:FF:FF:FF:FF:FF on the current WiFi
 * channel over the STA interface, running alongside normal WiFi (HTTP/MQTT)
 * and BLE. This module is transport only — framing/protocol is Task 2's
 * job. Its recv callback runs FROM THE ESP-NOW DRIVER'S CALLBACK CONTEXT,
 * so any callback registered via swarm_radio_set_rx() must only queue work,
 * never block or do heavy processing.
 */

#ifdef __cplusplus
extern "C" {
#endif

/** Bring up esp_now and register the broadcast peer. The WiFi driver must
 *  already be started (call this after wifi_init() *and* after the HTTP
 *  server comes up, so init never races association). Idempotent — a
 *  second call is a no-op returning ESP_OK. Safe whether WiFi is in STA or
 *  APSTA mode. Failure is non-fatal for the caller: the lamp still works
 *  without swarm features, just log a warning. */
esp_err_t swarm_radio_init(void);

/** Broadcast `len` (1..250) bytes to FF:FF:FF:FF:FF:FF. Returns
 *  ESP_ERR_INVALID_STATE if swarm_radio_init() hasn't succeeded yet, or
 *  ESP_ERR_INVALID_SIZE if `len` is out of range. */
esp_err_t swarm_radio_send(const uint8_t *data, size_t len);

/** Callback invoked FROM THE ESP-NOW RECV CALLBACK CONTEXT for every frame
 *  this lamp receives (after the module's own bookkeeping runs). `mac` is
 *  the 6-byte sender address; `data`/`len` point at a buffer only valid for
 *  the duration of the call. MUST NOT block — queue and return. */
typedef void (*swarm_radio_rx_cb_t)(const uint8_t *mac, const uint8_t *data, size_t len);
void swarm_radio_set_rx(swarm_radio_rx_cb_t cb);

/** Snapshot of the running counters. Any pointer may be NULL to skip it. */
void swarm_radio_stats(uint32_t *tx, uint32_t *rx, uint32_t *tx_fail);

/** Spike/diagnostic: 6-byte MAC of the last sender the recv bookkeeping saw
 *  (all-zero until the first RX). Backs GET /api/swarm/stats' "lastFrom". */
void swarm_radio_last_from(uint8_t mac[6]);

#ifdef __cplusplus
}
#endif
