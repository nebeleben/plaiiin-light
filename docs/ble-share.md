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
| `…3`        | `ack`     | write      | UTF-8 `"ok"`, `"unreachable"` or `"err"` from the user back to the admin |

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
{ "name": "<lamp name>", "role": "user|creator", "label": "<share label>",
  "host": "<lamp host>", "port": 80 }
```

Non-secret: lets the user's UI show which lamp / what access is on offer
before the code is entered. Read straight after connecting.

`host` and `port` are optional (added after the first release; older admins
omit them, and older receivers ignore them). They are the same values as in
the encrypted payload. A receiver that gets them checks right away whether it
can reach the lamp, before the user types the code (see **Reachability**).

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
5. On success the user's app adds the shared lamp (`/api/whoami` +
   `/api/info`) while staying connected, **then** writes the ack:
   `"ok"` if the lamp was added, `"unreachable"` if the lamp couldn't be
   reached (see **Reachability**), `"err"` for any other failure.
6. The admin shows the result and stops advertising: `"ok"` → confirmation;
   `"unreachable"` → "the other device can't reach the lamp, it must be on
   the same Wi-Fi as the lamp"; anything else → a generic failure. (Admins
   built before `"unreachable"` existed show their generic failure for it.)

## Reachability

A share only works if the receiver is on the lamp's network: `host` is the
lamp's LAN address. Both ends make that visible.

- **Admin:** the share sheets (QR and BLE) say that the receiving device must
  be on the same Wi-Fi as the lamp, naming the lamp and its host.
- **Probe:** an HTTP `GET http://<host>:<port>/api/info` with a ~3 s timeout.
  *Any* HTTP response (including 401/403) means reachable; a connection
  error or timeout means unreachable. Demo hosts (`demo://…`) are always
  reachable and never probed.
- **Before the code (BLE):** when `meta` carries `host`, the receiver probes
  it. Unreachable → it says "This device can't reach <lamp> at <host>. Join
  the lamp's Wi-Fi network, then tap Retry." and keeps the code entry disabled
  until a retry succeeds.
- **After the code (BLE and QR):** if adding the lamp fails, the receiver
  probes again to tell "unreachable" (same message, and BLE writes
  `ack = "unreachable"`) from other errors (`ack = "err"`).
