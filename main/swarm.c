#include "swarm.h"
#include "swarm_radio.h"
#include "light_api.h"
#include "js_api.h"
#include "js_player.h"
#include "led_control.h"
#include "config_store.h"
#include "wifi.h"

#include "mbedtls/md.h"
#include "mbedtls/constant_time.h"

#include "esp_wifi.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <string.h>
#include <stdlib.h>

static const char *TAG = "swarm";

// Defined in light_api.c (added alongside this task's notify hooks) but
// deliberately not added to light_api.h — light_api.h is outside this task's
// file scope. Returns the persisted lamp mode ("api"|"js"|"frame"), ignoring
// any live "stream" override, mirroring the same internal read GET
// /api/state and GET /api/mode use before folding the stream check in. A
// follow-up task can promote this to light_api.h if another caller needs it.
void light_api_get_persistent_mode(char *out, size_t out_len);

// =============================================================================
// Packet format — "PLSW" v1 (docs/superpowers/plans/2026-07-31-swarm-firmware.md
// Global Constraints table). Fixed-offset, little-endian seq, <=250 B total.
//
//   off | size | field
//   ----|------|------
//     0 |    4 | magic "PLSW"
//     4 |    1 | version = 1
//     5 |    8 | swarm id (raw bytes of the 16-hex id)
//    13 |    6 | origin STA MAC
//    19 |    4 | seq (u32 LE, per-origin monotonic)
//    23 |    1 | hop (0 = original, 1 = relayed)
//    24 |    1 | on/off (0|1)
//    25 |    3 | color r,g,b
//    28 |    1 | brightness
//    29 |    1 | mode (0 = api, 1 = js, 2 = no-change — origin is in
//               frame/stream/any other mode; receivers apply on/color/
//               brightness and skip mode+effect entirely)
//    30 |    1 | effect-name length L (0-32)
//    31 |    L | effect name (no NUL)
//  31+L |   16 | HMAC-SHA256 over bytes [0, 31+L), truncated to 16 bytes
//
// NOTE the HMAC-covered region INCLUDES the hop byte (offset 23) — a
// relayer that flips hop 0->1 therefore MUST recompute the tag over the
// mutated bytes with the shared swarm key (it has the key, being a member);
// the original origin's tag no longer validates once hop changes. See
// swarm_handle_rx()'s relay step.
// =============================================================================

#define SW_MAGIC            "PLSW"
#define SW_MAGIC_LEN        4
#define SW_VERSION          1
#define SW_ID_LEN           8    // raw bytes (16 hex chars)
#define SW_MAC_LEN          6
#define SW_KEY_LEN          32   // raw bytes (64 hex chars)
#define SW_HMAC_LEN         16   // truncated tag
#define SW_MAX_NAME_LEN     32
#define SW_HEADER_LEN       31   // bytes [0, 31) — everything before the effect name
#define SW_MAX_PACKET_LEN   250

#define SW_OFF_HOP          23

// RX queue slot — fixed size, no heap. Matches the plan's exact layout.
typedef struct {
    uint8_t data[SW_MAX_PACKET_LEN];
    uint8_t len;
    uint8_t mac[6];   // ESP-NOW sender (radio hop, not necessarily the packet's origin)
} swarm_rx_item_t;

// Parsed+validated snapshot.
typedef struct {
    uint8_t  origin[6];
    uint32_t seq;
    uint8_t  hop;
    bool     on;
    uint8_t  r, g, b;
    uint8_t  brightness;
    uint8_t  mode;         // 0 = api, 1 = js, 2 = no-change (skip mode+effect on apply)
    char     name[SW_MAX_NAME_LEN + 1];
} swarm_snapshot_t;

// De-dupe LRU entry.
typedef struct {
    bool     valid;
    uint8_t  mac[6];
    uint32_t seq;
    uint32_t last_used;
} swarm_dedupe_entry_t;

#define SW_DEDUPE_SIZE 16

// -----------------------------------------------------------------------------
// State — all of this (except the counters, which have documented multi-task
// writers) is touched only by: the public API functions (called from whatever
// task owns the HTTP/BLE handler that's provisioning) and the worker task
// (swarm_handle_rx / swarm_broadcast_snapshot). Membership changes (join/
// leave/enable) are rare, operator-driven calls — no locking is used, matching
// this file's other single-flag members (s_applying, s_tx_pending) and the
// existing codebase's "plain volatile / accepted benign race" pattern (see
// swarm_radio.c). See the Task 2 report's concurrency self-review for the
// exact task-touches-what breakdown.
// -----------------------------------------------------------------------------
static bool    s_member = false;
static bool    s_enabled = false;
static char    s_id_hex[17] = {0};
static uint8_t s_id_raw[SW_ID_LEN] = {0};
static uint8_t s_key_raw[SW_KEY_LEN] = {0};
static int     s_channel = 0;
static uint8_t s_own_mac[6] = {0};
static bool    s_own_mac_valid = false;

static uint32_t s_seq = 0;   // next sequence number to send

// Loop prevention: swarm_notify_state() is a no-op while true.
static volatile bool s_applying = false;

// TX coalescing.
static volatile bool    s_tx_pending = false;
static volatile int64_t s_tx_pending_since_ms = 0;

// Diagnostics — written only by the worker task; read from any task via
// swarm_debug_stats(), so kept volatile for cross-task visibility.
static volatile uint32_t s_drop_auth = 0;
static volatile uint32_t s_drop_replay = 0;
static volatile uint32_t s_relayed = 0;
static volatile uint32_t s_applied = 0;
static volatile uint32_t s_rx_queue_full = 0;   // internal-only; not part of the public 4-counter API

static swarm_dedupe_entry_t s_dedupe[SW_DEDUPE_SIZE];
static uint32_t s_dedupe_clock = 0;   // logical LRU clock, worker-task-only

static QueueHandle_t s_rx_queue = NULL;
static bool s_rx_cb_installed = false;
static bool s_task_started = false;
static TaskHandle_t s_worker_task_handle = NULL;

// =============================================================================
// Hex helpers
// =============================================================================

static bool is_hex_char(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static bool is_hex_str(const char *s, size_t expect_len)
{
    if (!s || strlen(s) != expect_len) return false;
    for (size_t i = 0; i < expect_len; i++) {
        if (!is_hex_char(s[i])) return false;
    }
    return true;
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return c - 'A' + 10;
}

// Decodes exactly out_len*2 hex chars from `hex` into `out`. Caller must have
// already validated length via is_hex_str.
static void hex_decode(const char *hex, uint8_t *out, size_t out_len)
{
    for (size_t i = 0; i < out_len; i++) {
        out[i] = (uint8_t)((hex_val(hex[i * 2]) << 4) | hex_val(hex[i * 2 + 1]));
    }
}

// =============================================================================
// Crypto — HMAC-SHA256, truncated to 16 bytes, constant-time compare.
// =============================================================================

static void swarm_hmac(const uint8_t key[SW_KEY_LEN], const uint8_t *data, size_t len,
                        uint8_t out_tag[SW_HMAC_LEN])
{
    uint8_t full[32];
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, SW_KEY_LEN, data, len, full);
    memcpy(out_tag, full, SW_HMAC_LEN);
}

// mbedtls_ct_memcmp (mbedtls/constant_time.h) is present unconditionally in
// this IDF's mbedtls (v5.3.2) — verified against the checked-out component
// tree. Used here rather than plain memcmp specifically for the HMAC tag
// compare, per the plan's pin; id/origin/magic compares elsewhere in this
// file are NOT security-sensitive in the same way (a mismatch there just
// means "not our packet") and use plain memcmp.
static bool swarm_ct_eq(const uint8_t *a, const uint8_t *b, size_t len)
{
    return mbedtls_ct_memcmp(a, b, len) == 0;
}

// =============================================================================
// NVS / sequence persistence
// =============================================================================

static void swarm_load_seq(void)
{
    int32_t persisted = config_get_i32_or(CONFIG_KEY_SW_SEQ, 0);
    // Boot/activation restores persisted+64: every session persists sw_seq
    // every 64 increments, so at most 63 sends could have happened past the
    // last persisted value before a crash/power-loss — starting 64 above it
    // guarantees a *fresh boot* never reuses a sequence number a peer may
    // have already seen from us.
    //
    // swarm_activate() (and therefore this function) can also run more than
    // once per boot — e.g. enable-off -> enable-on, or a re-join — without
    // s_seq ever having reset to 0. Fix (review round 1, finding 3): never
    // let re-activation LOWER the live counter. persisted+64 is only a
    // lower bound recovered from NVS for the "just booted, s_seq still at
    // its 0 initializer" case; once this session has actually been sending,
    // s_seq is the source of truth and is already >= persisted+64 (NVS is
    // written FROM s_seq, never the other way after boot). Clobbering it
    // back down here would make every peer reject our next several dozen
    // broadcasts as replays until the counter climbed back past their
    // last-seen value.
    uint32_t candidate = (uint32_t)persisted + 64;
    if (candidate > s_seq) {
        s_seq = candidate;
        // Fix round 2, bug 2: persist the boosted floor immediately, not
        // just at swarm_next_seq()'s steady every-64-increments cadence.
        // Without this, two reboots that both land inside the same
        // un-persisted 64-window read the SAME stale `persisted` value from
        // NVS, compute the SAME candidate, and re-send sequence numbers a
        // peer already saw last boot — its de-dupe LRU correctly rejects
        // them as replays (reproduced live: 5 broadcasts dropped across a
        // double-reboot in Task 4's bench pass). Writing the new floor back
        // out here means every boot/(re-)activation strictly advances what
        // the NEXT boot will read, even if this session sends nothing at
        // all before crashing again. One extra NVS write per boot/
        // activation — negligible wear next to the steady-state cadence.
        config_store_set_i32(CONFIG_KEY_SW_SEQ, (int32_t)s_seq);
    }
}

static uint32_t swarm_next_seq(void)
{
    uint32_t seq = s_seq;
    s_seq++;
    if (s_seq % 64 == 0) {
        config_store_set_i32(CONFIG_KEY_SW_SEQ, (int32_t)s_seq);
    }
    return seq;
}

// =============================================================================
// Packet pack / parse
// =============================================================================

// Builds a full "PLSW" v1 packet (including HMAC) into `out` (must be able to
// hold SW_MAX_PACKET_LEN). Returns the total length.
static size_t swarm_pack(uint8_t *out, uint32_t seq, uint8_t hop,
                          bool on, uint8_t r, uint8_t g, uint8_t b, uint8_t brightness,
                          uint8_t mode_bit, const char *name)
{
    size_t off = 0;
    memcpy(out + off, SW_MAGIC, SW_MAGIC_LEN); off += SW_MAGIC_LEN;
    out[off++] = SW_VERSION;
    memcpy(out + off, s_id_raw, SW_ID_LEN); off += SW_ID_LEN;
    memcpy(out + off, s_own_mac, SW_MAC_LEN); off += SW_MAC_LEN;
    out[off++] = (uint8_t)(seq & 0xFF);
    out[off++] = (uint8_t)((seq >> 8) & 0xFF);
    out[off++] = (uint8_t)((seq >> 16) & 0xFF);
    out[off++] = (uint8_t)((seq >> 24) & 0xFF);
    out[off++] = hop;
    out[off++] = on ? 1 : 0;
    out[off++] = r;
    out[off++] = g;
    out[off++] = b;
    out[off++] = brightness;
    out[off++] = mode_bit;

    uint8_t name_len = 0;
    if (name && name[0] && mode_bit == 1) {
        size_t n = strlen(name);
        if (n > SW_MAX_NAME_LEN) n = SW_MAX_NAME_LEN;
        name_len = (uint8_t)n;
    }
    out[off++] = name_len;
    if (name_len) {
        memcpy(out + off, name, name_len);
        off += name_len;
    }

    uint8_t tag[SW_HMAC_LEN];
    swarm_hmac(s_key_raw, out, off, tag);
    memcpy(out + off, tag, SW_HMAC_LEN);
    off += SW_HMAC_LEN;
    return off;
}

// Parses + range-checks (NOT authenticates) a received buffer. `hdr_len_out`
// receives the offset of the HMAC tag (31+L) so the caller can verify it.
static bool swarm_parse(const uint8_t *data, size_t len, swarm_snapshot_t *out, size_t *hdr_len_out)
{
    if (len < SW_HEADER_LEN + SW_HMAC_LEN || len > SW_MAX_PACKET_LEN) return false;
    if (memcmp(data, SW_MAGIC, SW_MAGIC_LEN) != 0) return false;
    if (data[4] != SW_VERSION) return false;

    // data[5..12] is the swarm id — checked by the caller (needs s_id_raw).
    memcpy(out->origin, data + 13, SW_MAC_LEN);
    out->seq = (uint32_t)data[19] | ((uint32_t)data[20] << 8) |
               ((uint32_t)data[21] << 16) | ((uint32_t)data[22] << 24);
    out->hop = data[23];
    if (out->hop > 1) return false;
    out->on = data[24] != 0;
    out->r = data[25];
    out->g = data[26];
    out->b = data[27];
    out->brightness = data[28];
    out->mode = data[29];
    if (out->mode > 2) return false;   // 0=api, 1=js, 2=no-change

    uint8_t name_len = data[30];
    if (name_len > SW_MAX_NAME_LEN) return false;
    size_t hdr_len = SW_HEADER_LEN + name_len;
    if (len != hdr_len + SW_HMAC_LEN) return false;   // exact length, no trailing garbage

    memcpy(out->name, data + SW_HEADER_LEN, name_len);
    out->name[name_len] = '\0';

    if (hdr_len_out) *hdr_len_out = hdr_len;
    return true;
}

// =============================================================================
// De-dupe / replay
// =============================================================================

// Returns true (accept) if `seq` is fresh for `mac`; false (drop, replay) if
// seq <= the last-seen value for a known origin. Unknown origins are always
// accepted and inserted (evicting the least-recently-used slot if full).
// Worker-task-only — no locking.
static bool swarm_dedupe_accept(const uint8_t mac[6], uint32_t seq)
{
    s_dedupe_clock++;
    int free_idx = -1;
    int lru_idx = 0;
    uint32_t lru_min = UINT32_MAX;

    for (int i = 0; i < SW_DEDUPE_SIZE; i++) {
        if (s_dedupe[i].valid && memcmp(s_dedupe[i].mac, mac, 6) == 0) {
            if (seq <= s_dedupe[i].seq) return false;
            s_dedupe[i].seq = seq;
            s_dedupe[i].last_used = s_dedupe_clock;
            return true;
        }
        if (!s_dedupe[i].valid && free_idx < 0) free_idx = i;
        if (s_dedupe[i].last_used < lru_min) { lru_min = s_dedupe[i].last_used; lru_idx = i; }
    }

    int idx = (free_idx >= 0) ? free_idx : lru_idx;
    memcpy(s_dedupe[idx].mac, mac, 6);
    s_dedupe[idx].seq = seq;
    s_dedupe[idx].last_used = s_dedupe_clock;
    s_dedupe[idx].valid = true;
    return true;
}

// =============================================================================
// Apply — receives a validated snapshot, drives it through the same
// transport-agnostic helpers HTTP/BLE use. Apply order (pin): power ->
// brightness -> color -> mode/effect.
// =============================================================================

static void swarm_apply(const swarm_snapshot_t *pkt)
{
    s_applying = true;

    light_api_apply_power(pkt->on);
    light_api_apply_brightness(pkt->brightness);
    // In js/frame mode this updates baseColor only (by design) — see
    // light_api_apply_color_solid's doc comment.
    light_api_apply_color_solid(pkt->r, pkt->g, pkt->b);

    if (pkt->mode == 1) {
        // js — a lamp in frame mode receiving js DOES switch (swarm wins).
        //
        // Review round 1 fixes:
        //   finding 1 (critical) — only attempt to play when the packet
        //     says the lamp is ON. js_player_start() unconditionally
        //     re-enables the LEDs if they're off, so an unguarded play call
        //     here fought light_api_apply_power(false) above and left an
        //     "off" origin's peers stuck on. Mirrors light_api_apply_mode's
        //     own js branch, which only calls start_current_js() when
        //     led_control_is_on().
        //   finding 2 (important) — skip the play call entirely when the
        //     receiver is already running this exact script, mirroring
        //     start_current_js()'s js_api_is_running()+name-match fast path
        //     (light_api.c) — otherwise every accepted packet (e.g. a
        //     peer's brightness-only nudge) restarted the animation from
        //     frame 0 on every receiver.
        bool ok_to_flip_mode = true;
        if (pkt->on && pkt->name[0]) {
            const char *cur = js_api_current_name();
            bool already_playing = js_api_is_running() && cur && strcmp(cur, pkt->name) == 0;
            if (!already_playing) {
                esp_err_t rc = js_api_play(pkt->name, JS_DEFAULT_FPS);
                ok_to_flip_mode = (rc == ESP_OK);
            }
        }
        // else: off, or no effect name in the packet — nothing to play;
        // light_api_apply_mode("js") below still updates the persisted
        // mode string, and (being is_on-guarded itself) won't start
        // anything while off.
        if (ok_to_flip_mode) {
            // Idempotent: if we just played (or the fast path found it
            // already playing), start_current_js()'s own fast path inside
            // this call just no-ops.
            light_api_apply_mode("js");
        }
        // else: script missing locally — "color fallback". Leave mode and
        // the currently-playing effect alone; on/color/brightness above
        // still applied.
    } else if (pkt->mode == 0) {
        light_api_apply_mode("api");
    }
    // else pkt->mode == 2 ("no-change" — origin's persisted mode is neither
    // api nor js, e.g. frame/stream): skip mode+effect entirely, per the
    // review-round-1 protocol pin (docs/superpowers/plans/
    // 2026-07-31-swarm-firmware.md, commit 4ba13de). on/brightness/color
    // above were already applied unconditionally.

    s_applying = false;
}

// =============================================================================
// RX — minimal radio callback (queues only, runs in the ESP-NOW driver's
// callback context) + worker-task validation/apply/relay.
// =============================================================================

static void swarm_radio_recv_cb(const uint8_t *mac, const uint8_t *data, size_t len)
{
    if (!s_rx_queue || len == 0 || len > SW_MAX_PACKET_LEN) return;
    swarm_rx_item_t item;
    memcpy(item.data, data, len);
    item.len = (uint8_t)len;
    if (mac) memcpy(item.mac, mac, 6);
    else memset(item.mac, 0, 6);
    // Non-blocking send (0 tick timeout): this runs in the esp_now driver's
    // callback context, not an ISR, but it must never block — drop-on-full
    // per the plan's pin.
    if (xQueueSend(s_rx_queue, &item, 0) != pdTRUE) {
        s_rx_queue_full++;
    }
}

static void swarm_relay(const swarm_rx_item_t *item, size_t hdr_len)
{
    // 10-49 ms jitter before rebroadcasting, to spread simultaneous
    // relays from multiple lamps that all heard the same origin packet.
    vTaskDelay(pdMS_TO_TICKS(10 + (esp_random() % 40)));

    uint8_t buf[SW_MAX_PACKET_LEN];
    size_t total_len = hdr_len + SW_HMAC_LEN;
    memcpy(buf, item->data, total_len);
    buf[SW_OFF_HOP] = 1;
    // The HMAC-covered region includes the hop byte — must recompute after
    // flipping it (see the packet-format comment at the top of this file).
    uint8_t tag[SW_HMAC_LEN];
    swarm_hmac(s_key_raw, buf, hdr_len, tag);
    memcpy(buf + hdr_len, tag, SW_HMAC_LEN);

    swarm_radio_send(buf, total_len);
    s_relayed++;
}

static void swarm_handle_rx(const swarm_rx_item_t *item)
{
    if (!s_member || !s_enabled) return;

    // Fast, non-cryptographic checks first (magic/version/length/swarm id) —
    // cheap enough to run before touching HMAC, and lets us silently ignore
    // stray "PLSW-SPIKE" ping traffic (Task 1) and other swarms' packets
    // without spending a counter or any crypto on them.
    if (item->len < SW_ID_LEN + 13) return;   // sanity floor before indexing the id field
    if (memcmp(item->data, SW_MAGIC, SW_MAGIC_LEN) != 0) return;
    if (memcmp(item->data + 5, s_id_raw, SW_ID_LEN) != 0) return;   // not our swarm

    swarm_snapshot_t pkt;
    size_t hdr_len = 0;
    if (!swarm_parse(item->data, item->len, &pkt, &hdr_len)) return;   // malformed

    // Own-MAC filter (loop prevention): ignore our own packet if a peer
    // relayed it back to us. Compares the packet's ORIGIN field, not the
    // ESP-NOW sender — a relayed hop=1 packet's sender is the relaying
    // peer, but its origin is still us.
    if (s_own_mac_valid && memcmp(pkt.origin, s_own_mac, SW_MAC_LEN) == 0) return;

    uint8_t tag[SW_HMAC_LEN];
    swarm_hmac(s_key_raw, item->data, hdr_len, tag);
    if (!swarm_ct_eq(tag, item->data + hdr_len, SW_HMAC_LEN)) {
        s_drop_auth++;
        return;
    }

    if (!swarm_dedupe_accept(pkt.origin, pkt.seq)) {
        s_drop_replay++;
        return;
    }

    swarm_apply(&pkt);
    s_applied++;

    // Relay exactly once: hop==0 -> rebroadcast hop=1; never relay hop==1.
    if (pkt.hop == 0) {
        swarm_relay(item, hdr_len);
    }
}

// =============================================================================
// TX — snapshot build (mirrors GET /api/state's accessors exactly) +
// coalesced broadcast.
// =============================================================================

static void swarm_broadcast_snapshot(void)
{
    if (!s_member || !s_enabled) return;

    bool on = led_control_is_on();
    uint8_t r = 0, g = 0, b = 0;
    js_player_get_base_color(&r, &g, &b);
    uint8_t brightness = led_control_get_brightness();

    char mode[16] = {0};
    light_api_get_persistent_mode(mode, sizeof(mode));
    // Review round 1, finding 4 (protocol decision, plan commit 4ba13de):
    // the wire format encodes api(0)/js(1)/no-change(2). A lamp whose
    // persisted mode is neither api nor js (frame, stream, or anything
    // future) broadcasts mode=2 — receivers apply on/color/brightness and
    // leave mode+effect alone, so "frame itself never propagates" actually
    // holds (previously this encoded as api(0), which yanked js-mode peers
    // into api on every frame-mode color/brightness tweak).
    uint8_t mode_bit;
    if (strcmp(mode, "js") == 0) {
        mode_bit = 1;
    } else if (strcmp(mode, "api") == 0) {
        mode_bit = 0;
    } else {
        mode_bit = 2;   // frame, stream (transient, never persisted here anyway), or future modes
    }
    const char *name = (mode_bit == 1) ? js_api_current_name() : NULL;

    uint32_t seq = swarm_next_seq();
    uint8_t buf[SW_MAX_PACKET_LEN];
    size_t len = swarm_pack(buf, seq, 0, on, r, g, b, brightness, mode_bit, name);
    swarm_radio_send(buf, len);
}

// =============================================================================
// Worker task
// =============================================================================

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void swarm_worker_task(void *arg)
{
    (void)arg;
    for (;;) {
        swarm_rx_item_t item;
        BaseType_t got = xQueueReceive(s_rx_queue, &item, pdMS_TO_TICKS(100));
        if (got == pdTRUE) {
            swarm_handle_rx(&item);
        }

        if (s_tx_pending) {
            if (now_ms() - s_tx_pending_since_ms >= 100) {
                s_tx_pending = false;
                swarm_broadcast_snapshot();
            }
        }
    }
}

// =============================================================================
// Activation — idempotent. Called whenever this lamp transitions into an
// active (member && enabled) state: at boot (swarm_init), on swarm_join, and
// on swarm_set_enabled(true). The RX callback and worker task, once
// installed/started, are left running for the process lifetime (see the
// report's concurrency notes on why: avoids a second, unsynchronized
// swarm_radio_set_rx() call racing live RX traffic). Both gate their actual
// work on s_member/s_enabled internally, so a disabled-but-once-activated
// lamp costs a harmlessly idling task, not stale behavior.
// =============================================================================

static void swarm_activate(void)
{
    if (!s_own_mac_valid) {
        if (esp_wifi_get_mac(WIFI_IF_STA, s_own_mac) == ESP_OK) {
            s_own_mac_valid = true;
        }
    }

    swarm_load_seq();

    // AP-less channel pin (honesty: unverified this plan — both bench lamps
    // are on the AP). Only act if a real channel was provisioned.
    if (!wifi_is_connected() && s_channel >= 1 && s_channel <= 14) {
        esp_err_t err = esp_wifi_set_channel((uint8_t)s_channel, WIFI_SECOND_CHAN_NONE);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_set_channel(%d) failed: %s", s_channel, esp_err_to_name(err));
        }
    }

    if (!s_rx_queue) {
        s_rx_queue = xQueueCreate(8, sizeof(swarm_rx_item_t));
    }
    if (!s_rx_cb_installed && s_rx_queue) {
        swarm_radio_set_rx(swarm_radio_recv_cb);
        s_rx_cb_installed = true;
    }
    if (!s_task_started && s_rx_queue) {
        if (xTaskCreate(swarm_worker_task, "swarm_worker", 4096, NULL,
                         tskIDLE_PRIORITY + 2, &s_worker_task_handle) == pdPASS) {
            s_task_started = true;
        } else {
            ESP_LOGE(TAG, "failed to start swarm worker task");
        }
    }
}

// Worker-task stack high-water mark, in bytes — see swarm.h. Callable from
// any task: uxTaskGetStackHighWaterMark() is safe to call on a handle from
// outside the target task. Returns 0 if the worker was never started (the
// only writer of s_worker_task_handle is swarm_activate(), and it's never
// cleared once set, matching the "tasks run for the process lifetime" note
// above).
uint32_t swarm_worker_stack_free(void)
{
    if (!s_worker_task_handle) return 0;
    // uxTaskGetStackHighWaterMark() returns the minimum-ever-remaining
    // headroom in stack *words* (StackType_t units), not bytes.
    UBaseType_t words = uxTaskGetStackHighWaterMark(s_worker_task_handle);
    return (uint32_t)words * (uint32_t)sizeof(StackType_t);
}

// =============================================================================
// Public API
// =============================================================================

void swarm_init(void)
{
    char id_hex[17] = {0};
    char key_hex[65] = {0};
    config_get_str_or(CONFIG_KEY_SW_ID, id_hex, sizeof(id_hex), "");
    config_get_str_or(CONFIG_KEY_SW_KEY, key_hex, sizeof(key_hex), "");
    s_channel = (int)config_get_i32_or(CONFIG_KEY_SW_CHAN, 0);
    bool on = config_get_i32_or(CONFIG_KEY_SW_ON, 0) != 0;

    if (is_hex_str(id_hex, 16) && is_hex_str(key_hex, 64)) {
        hex_decode(id_hex, s_id_raw, SW_ID_LEN);
        hex_decode(key_hex, s_key_raw, SW_KEY_LEN);
        strncpy(s_id_hex, id_hex, sizeof(s_id_hex) - 1);
        s_member = true;
    }
    s_enabled = s_member && on;

    if (s_member && s_enabled) {
        ESP_LOGI(TAG, "swarm member (id %s), enabled — activating", s_id_hex);
        swarm_activate();
    } else {
        ESP_LOGI(TAG, "swarm: %s", s_member ? "member, disabled" : "not a member");
    }
}

esp_err_t swarm_join(const char *id16hex, const char *key64hex, int channel)
{
    if (!is_hex_str(id16hex, 16)) return ESP_ERR_INVALID_ARG;
    if (!is_hex_str(key64hex, 64)) return ESP_ERR_INVALID_ARG;
    if (channel < 0 || channel > 14) return ESP_ERR_INVALID_ARG;

    uint8_t id_raw[SW_ID_LEN];
    uint8_t key_raw[SW_KEY_LEN];
    hex_decode(id16hex, id_raw, SW_ID_LEN);
    hex_decode(key64hex, key_raw, SW_KEY_LEN);

    // Persist first (source of truth), then flip RAM state.
    config_store_set_str(CONFIG_KEY_SW_ID, id16hex);
    config_store_set_str(CONFIG_KEY_SW_KEY, key64hex);
    config_store_set_i32(CONFIG_KEY_SW_CHAN, channel);
    config_store_set_i32(CONFIG_KEY_SW_ON, 1);

    memcpy(s_id_raw, id_raw, SW_ID_LEN);
    memcpy(s_key_raw, key_raw, SW_KEY_LEN);
    strncpy(s_id_hex, id16hex, sizeof(s_id_hex) - 1);
    s_id_hex[16] = '\0';
    s_channel = channel;
    s_member = true;
    s_enabled = true;   // join implies enable

    swarm_activate();
    ESP_LOGI(TAG, "joined swarm %s (channel %d)", s_id_hex, channel);
    return ESP_OK;
}

esp_err_t swarm_leave(void)
{
    static const char *const keys[] = {
        CONFIG_KEY_SW_ID,
        CONFIG_KEY_SW_KEY,
        CONFIG_KEY_SW_ON,
    };
    config_store_erase_keys(keys, sizeof(keys) / sizeof(keys[0]));

    s_member = false;
    s_enabled = false;
    memset(s_id_raw, 0, sizeof(s_id_raw));
    memset(s_key_raw, 0, sizeof(s_key_raw));   // wipe the secret from RAM
    s_id_hex[0] = '\0';

    ESP_LOGI(TAG, "left swarm");
    return ESP_OK;
}

esp_err_t swarm_set_enabled(bool on)
{
    if (on && !s_member) return ESP_ERR_INVALID_STATE;
    config_store_set_i32(CONFIG_KEY_SW_ON, on ? 1 : 0);
    s_enabled = on && s_member;
    if (s_enabled) swarm_activate();
    return ESP_OK;
}

bool swarm_is_member(void) { return s_member; }
bool swarm_is_enabled(void) { return s_enabled; }

void swarm_get_id(char *out, size_t len)
{
    snprintf(out, len, "%s", s_member ? s_id_hex : "");
}

int swarm_get_channel(void) { return s_channel; }

void swarm_notify_state(void)
{
    if (!s_member || !s_enabled) return;
    if (s_applying) return;
    if (!s_tx_pending) {
        s_tx_pending_since_ms = now_ms();
    }
    s_tx_pending = true;
}

void swarm_debug_stats(uint32_t *drop_auth, uint32_t *drop_replay,
                        uint32_t *relayed, uint32_t *applied)
{
    if (drop_auth) *drop_auth = s_drop_auth;
    if (drop_replay) *drop_replay = s_drop_replay;
    if (relayed) *relayed = s_relayed;
    if (applied) *applied = s_applied;
}
