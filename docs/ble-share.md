# BLE share — client-to-client lamp sharing

A way to hand a lamp share key from one app install to another **over BLE**
instead of scanning a QR code. The lamp/firmware is **not involved** — this is
purely a client-to-client transfer. The admin still mints the share key the
usual way (`POST /api/keys`, see [`sharing-api.md`](sharing-api.md)); BLE
just replaces the camera as the transport.

Frozen here so every client implements the identical wire format and crypto.

## Roles

- **Admin** (the sharer) is the BLE **peripheral**: it advertises a GATT
  service and serves the encrypted payload.
- **User** (the receiver) is the BLE **central**: it scans, connects, and
  reads the payload.

## GATT service

Base UUID `B1E5A9E0-7C3D-4F1A-8E2B-9D0C1A2B3C0?` — last nibble distinguishes
the service from each characteristic.

| UUID suffix | Name      | Properties | Contents |
|-------------|-----------|------------|----------|
| `…0`        | `service` | —          | the primary service |
| `…1`        | `meta`    | read       | plaintext JSON, non-secret (see below) |
| `…2`        | `payload` | read       | `salt ‖ AES-GCM sealed box` (see below) |
| `…3`        | `ack`     | write      | UTF-8 `"ok"` or `"err"` from the user back to the admin |

The admin advertises `service` in the advertising packet, plus the lamp name as
the local name where the platform allows it. iOS routes a 128-bit service UUID
through the scan-response / overflow area — a central scanning with an explicit
service-UUID filter still discovers it.

Platform note: Android cannot set a custom advertised local name without
renaming the whole Bluetooth adapter, so an Android admin advertises the
service UUID only. The lamp name is non-critical pre-connect — every receiver
reads the authoritative name from `meta` after connecting; the advertised name
is just a nicety for the scan list.

## `meta` — plaintext

```json
{ "name": "<lamp name>", "role": "user|creator", "label": "<share label>" }
```

Non-secret: lets the user's UI show which lamp / what access is on offer
before the code is entered. Read straight after connecting.

## `payload` — encrypted

`payload = salt (16 bytes) ‖ combined`

where `combined` is a CryptoKit `AES.GCM.SealedBox.combined` (`nonce(12) ‖
ciphertext ‖ tag(16)`) sealing this JSON:

```json
{ "host": "...", "port": 80, "key": "<share key>",
  "name": "...", "role": "...", "label": "..." }
```

Once decrypted the user runs the same post-receive logic as the QR flow
(`DeviceStore.addSharedLamp` → `/api/whoami` + `/api/info`).

## Crypto

- The admin generates a random **6-digit code** (`000000`–`999999`) and a
  random 16-byte salt.
- Key derivation: `K = PBKDF2-HMAC-SHA256(code, salt, 200 000 iterations, 32 bytes)`.
- The payload JSON is sealed with `AES-GCM` under `K`.
- The admin **displays the code**; the user **types it**. A wrong code fails
  the GCM tag check — the user simply retries.

**Security note.** The 6-digit space is small (1 M). The high PBKDF2 iteration
count makes a brute-force costly but not impossible: anyone who sniffs the
`payload` blob during the exchange can brute-force it offline. The real
mitigations are the short advertising window and that the admin can revoke the
share key at any time (`POST /api/keys/<id>/revoke`). For a stronger guarantee
an ECDH-based variant would be needed — out of scope for now.

## Flow

1. Admin taps "advertise over BLE" on a share-key row → app generates the
   code, seals the payload, starts advertising; the admin UI shows the code.
2. User opens Devices ▸ "Add shared lamp" ▸ receive over Bluetooth → scans,
   sees the advertised admin, taps it.
3. User's app connects, reads `meta`, then reads `payload` (cached locally).
4. User types the code → app derives `K`, opens the sealed box locally
   (no extra round-trip per attempt). Wrong code → retry.
5. On success the user's app writes `ack = "ok"`, then adds the shared lamp.
6. The admin sees `ack = "ok"` → shows confirmation → stops advertising.
