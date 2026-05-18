#pragma once

#include "esp_err.h"
#include "esp_http_server.h"
#include <stdbool.h>
#include <stddef.h>

/**
 * Phase 9 — pairing token + mode. Phase 27 — role-based sharing.
 *
 * NVS state:
 *   pair_mode  = "unpaired" | "paired"   (default: unpaired)
 *   pair_token = 32 random bytes, base64-url encoded (43 chars) — the ADMIN key
 *   share_keys = JSON list of limited-role keys (see keys.{h,c})
 *
 * Unpaired: HTTP / BLE behave like pre-1.6 — no auth, treated as admin.
 * Paired:   HTTP requires `Authorization: Bearer <token>`. The token resolves
 *           to a role: the pair_token → admin; a share key → user|creator.
 *           Every endpoint declares a minimum role; see docs/sharing-api.md.
 *
 * Token comparison is constant-time.
 */

/** Access roles, ordered: NONE < USER < CREATOR < ADMIN. */
typedef enum {
    PL_ROLE_NONE    = 0,
    PL_ROLE_USER    = 1,
    PL_ROLE_CREATOR = 2,
    PL_ROLE_ADMIN   = 3,
} pl_role_t;

/** Lowercase name of a role ("admin"/"creator"/"user"/"none"). */
const char *pl_role_name(pl_role_t role);

esp_err_t pairing_init(void);

/** True if the device is currently in paired mode. */
bool pairing_is_paired(void);

/** Resolve the role a raw token grants: admin when unpaired, admin for the
 *  pair_token, user|creator for a matching non-revoked share key, else NONE. */
pl_role_t pairing_role_for_token(const char *token);

/** Resolve the role of an HTTP request from its Authorization header.
 *  Returns PL_ROLE_ADMIN when unpaired or in Wi-Fi AP mode. */
pl_role_t pairing_resolve_role(httpd_req_t *req);

/** Legacy helper — true if `token` grants any valid role. */
bool pairing_check(const char *token);

/** Generate a fresh admin token, switch to paired mode, write it to
 *  `out_token` (NUL-terminated, ≤64 bytes). */
esp_err_t pairing_pair(char *out_token, size_t out_len);

/** Drop pairing: wipe the admin token, flip mode to "unpaired". */
esp_err_t pairing_unpair(void);

/** Read the current admin token. ESP_ERR_NOT_FOUND when unpaired. */
esp_err_t pairing_get_token(char *out, size_t out_len);

/**
 * Standard guard for HTTP handlers. Requires the request to carry a token
 * granting at least `min_role`. Sends 401 (no/invalid token) or 403
 * (insufficient role) and returns ESP_FAIL on failure.
 *
 *     if (pairing_http_check(req, PL_ROLE_USER) != ESP_OK) return ESP_FAIL;
 */
esp_err_t pairing_http_check(httpd_req_t *req, pl_role_t min_role);

/** Websocket-upgrade variant — reads the token from `?token=`. */
esp_err_t pairing_ws_check(httpd_req_t *req, pl_role_t min_role);

/** Generate a fresh 43-char base64-url token. `out` needs ≥64 bytes.
 *  Shared by pairing (admin token) and keys.c (share keys). */
void pl_token_generate(char *out, size_t out_len);

/** Constant-time string compare — shared with keys.c for key matching. */
bool pairing_ct_strcmp(const char *a, const char *b);
