#pragma once

#include "esp_err.h"
#include "esp_http_server.h"
#include "pairing.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * Phase 27 — share keys. Limited-role keys an admin hands out so other people
 * can use a paired lamp without full control. Stored as a JSON array in NVS
 * (CONFIG_KEY_SHARE_KEYS); the admin key itself stays in pairing.c.
 *
 * Each entry: { id, key, role:"user"|"creator"|"ai", label, created, revoked }.
 * AI entries additionally store p (AI_PERM_* mask), mb (max brightness,
 * -1 = no cap) and ow (only when on); the HTTP API renders those as
 * perms/maxBrightness/onlyWhenOn. See docs/sharing-api.md.
 */

/** Log the stored key count at boot. */
esp_err_t keys_init(void);

/* pl_access_t (what a share key grants) is defined in pairing.h. */

/** Resolve a raw token against the share-key list. Fills `out` and returns
 *  true for a non-revoked match; false (out->role = PL_ROLE_NONE) otherwise. */
bool keys_lookup(const char *token, pl_access_t *out);

/** Drop every share key (called when the lamp is unpaired). */
void keys_clear_all(void);

/** Register the /api/keys* and /api/whoami HTTP handlers. */
void keys_api_register(httpd_handle_t server);
