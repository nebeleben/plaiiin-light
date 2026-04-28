#pragma once

#include "esp_err.h"

/** Start mDNS responder and advertise the device under both
 *  `_http._tcp` (so it shows up in generic browsers) and
 *  `_plaiiinlight._tcp` (so the macOS/iOS clients can filter cheaply).
 *
 *  Hostname is derived from the configured node name; TXT records
 *  carry vendor / firmware version / api version / lampType.
 */
esp_err_t mdns_service_start(void);
