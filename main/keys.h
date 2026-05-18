#pragma once

#include "esp_err.h"
#include "esp_http_server.h"
#include "pairing.h"

/**
 * Phase 27 — share keys. Limited-role keys an admin hands out so other people
 * can use a paired lamp without full control. Stored as a JSON array in NVS
 * (CONFIG_KEY_SHARE_KEYS); the admin key itself stays in pairing.c.
 *
 * Each entry: { id, key, role:"user"|"creator", label, created, revoked }.
 * See docs/sharing-api.md.
 */

/** Log the stored key count at boot. */
esp_err_t keys_init(void);

/** Resolve a raw token against the share-key list. Returns the entry's role
 *  (user|creator), or PL_ROLE_NONE if no non-revoked key matches. */
pl_role_t keys_role_for(const char *token);

/** Drop every share key (called when the lamp is unpaired). */
void keys_clear_all(void);

/** Register the /api/keys* and /api/whoami HTTP handlers. */
void keys_api_register(httpd_handle_t server);
