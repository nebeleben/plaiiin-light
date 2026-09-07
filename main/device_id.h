#pragma once
#include <stddef.h>
// Stable per-lamp identifier: the factory (eFuse) base MAC as 12 lowercase
// hex chars, no separators — e.g. "a4cf12e3f5b8". Survives renames, OTA,
// reflash and factory reset. Emitted in the mDNS TXT record (`id`),
// GET /api (`deviceId`) and the BLE device-info characteristic (`id`) so
// clients can recognise a lamp after its node name changes.
#define DEVICE_ID_LEN 12
const char *device_id_get(void);

// Short 4-char uppercase-hex suffix from the SoftAP MAC (last two bytes),
// e.g. "3FA8". Shared by the provisioning AP SSID and the one-shot
// node-name suffix so a freshly burned "tower8v2" boots as "tower8v2-3FA8"
// and its AP is called exactly the same.
#define DEVICE_SUFFIX_LEN 4
void device_id_suffix(char *out, size_t out_len);
