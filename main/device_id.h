#pragma once
// Stable per-lamp identifier: the factory (eFuse) base MAC as 12 lowercase
// hex chars, no separators — e.g. "a4cf12e3f5b8". Survives renames, OTA,
// reflash and factory reset. Emitted in the mDNS TXT record (`id`),
// GET /api (`deviceId`) and the BLE device-info characteristic (`id`) so
// clients can recognise a lamp after its node name changes.
#define DEVICE_ID_LEN 12
const char *device_id_get(void);
