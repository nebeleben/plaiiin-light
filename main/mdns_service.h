#pragma once

#include "esp_err.h"
#include <stdbool.h>

/** Start mDNS responder and advertise the device under both
 *  `_http._tcp` (so it shows up in generic browsers) and
 *  `_plaiiinlight._tcp` (so the macOS/iOS clients can filter cheaply).
 *
 *  Hostname is derived from the configured node name; TXT records
 *  carry vendor / firmware version / api version / lampType.
 */
esp_err_t mdns_service_start(void);

/** Update the `paired` TXT record on both advertised services. Called by
 *  the pair endpoint handlers whenever pair state flips, so discovering
 *  clients can pre-filter lamps that are already owned by someone else.
 *  Safe to call before mDNS is up — becomes a no-op. */
esp_err_t mdns_service_set_paired(bool paired);

/** Tear down the responder and send mDNS goodbye packets so clients (macOS
 *  NWBrowser, Android NsdManager) drop their cached entry immediately
 *  instead of waiting for TTL expiry. Called from factory_reset right
 *  before esp_restart so the device doesn't appear "still discovered" on
 *  WiFi while it's actually rebooting into AP mode. */
esp_err_t mdns_service_stop(void);
