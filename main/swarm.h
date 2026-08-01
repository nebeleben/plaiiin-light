#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/**
 * swarm — masterless lamp-to-lamp state propagation over ESP-NOW
 * (PlanV3 V2.5 "swarm-firmware", Task 2).
 *
 * Builds on swarm_radio (Task 1) to add the actual protocol: a "PLSW" v1
 * packet carrying an HMAC-SHA256-authenticated full state snapshot
 * (on/off, color, brightness, mode/effect) with a per-origin monotonic
 * sequence number for replay protection, a small LRU de-dupe table, and
 * relay-once flooding so a swarm propagates beyond direct radio range.
 *
 * One swarm per lamp, provisioned by a client with a shared 16-hex id and
 * 64-hex HMAC key (see swarm_join). A background worker task (started once
 * the lamp is an active member) owns all of this: it is fed received
 * packets by a minimal ESP-NOW callback (copy+queue only, no crypto in
 * WiFi-task context) and coalesces local-change notifications
 * (swarm_notify_state) into a broadcast at most ~10x/second.
 *
 * Loop prevention: applying a received snapshot sets an internal flag that
 * makes swarm_notify_state() a no-op for the duration, so re-applying a
 * peer's state never triggers a broadcast of our own.
 */

#ifdef __cplusplus
extern "C" {
#endif

/** Load the NVS swarm block and, if this lamp is an active member
 *  (sw_id/sw_key present and sw_on == 1), activate: cache the own STA MAC,
 *  restore the TX sequence counter (last-persisted + 64), pin the WiFi
 *  channel when not yet associated to an AP, install the radio RX callback,
 *  and start the worker task. Call once at boot, after swarm_radio_init(). */
void swarm_init(void);

/** Join a swarm: validate id (16 hex chars), key (64 hex chars) and channel
 *  (0 = unset/current, else 1..14), persist them + enable, and activate
 *  membership immediately (join implies enable). Returns
 *  ESP_ERR_INVALID_ARG on malformed input, ESP_OK otherwise. */
esp_err_t swarm_join(const char *id16hex, const char *key64hex, int channel);

/** Leave the current swarm: erases the identity + enabled keys (sw_id,
 *  sw_key, sw_on — NOT sw_chan/sw_seq, which are harmless to leave behind)
 *  and disables. Always returns ESP_OK. */
esp_err_t swarm_leave(void);

/** Enable/disable swarm participation without losing membership. Enabling
 *  on a non-member returns ESP_ERR_INVALID_STATE. */
esp_err_t swarm_set_enabled(bool on);

bool swarm_is_member(void);
bool swarm_is_enabled(void);

/** Copies the 16-hex swarm id into `out` (empty string if not a member).
 *  Always NUL-terminates. Never exposes the key. */
void swarm_get_id(char *out, size_t len);

int swarm_get_channel(void);

/** Cheap, callable from any task (HTTP handler, BLE, MQTT, buttons): marks
 *  local state dirty so the worker task broadcasts a fresh snapshot ~100 ms
 *  from now (coalescing rapid successive calls into one broadcast). No-op
 *  when not an active member, or while applying a received snapshot. */
void swarm_notify_state(void);

/** Snapshot of the running diagnostic counters. Any pointer may be NULL to
 *  skip it. */
void swarm_debug_stats(uint32_t *drop_auth, uint32_t *drop_replay,
                        uint32_t *relayed, uint32_t *applied);

/** FreeRTOS stack high-water mark for the swarm worker task (js_api_play
 *  runs on this 4 KB task), in bytes: the smallest amount of headroom the
 *  task has ever had, since it started. Returns 0 if the worker task isn't
 *  running (not an active member, or not yet activated). For
 *  /api/swarm/stats instrumentation only — not a control input. */
uint32_t swarm_worker_stack_free(void);

#ifdef __cplusplus
}
#endif
