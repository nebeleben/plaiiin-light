# PlaiiinLightOS protocol contract

This document is the **canonical wire contract** between any client and a
PlaiiinLightOS lamp. The firmware is licensed under Apache 2.0
(`lampos/LICENSE`); the protocol below is the surface anyone can target to
build their own client app, command-line tool, or home-automation bridge.

If you find a behavior in the official apps that isn't documented here, it
is **not** part of the contract — please open an issue.

> **Status.** Stable. Breaking changes will bump `apiVersion` (currently
> `"1"`) and be announced in this document's changelog.

## Companion docs (authoritative for each subsystem)

- [`sharing-api.md`](sharing-api.md) — role-based access control, share-key
  endpoints, multi-user model.
- [`wormhole-api.md`](wormhole-api.md) — render-mode contract for
  `wormhole`-form lamps, ring geometry, `/api/wormhole` endpoints.
- [`ble-share.md`](ble-share.md) — client-to-client BLE share-key handoff.
  Does not involve the firmware.

The current file is the entry point; the specs above are normative for
their respective topics.

## Transports

A lamp speaks three transports. Every client uses some subset.

| Transport | Default port | When to use |
|---|---|---|
| HTTP (REST) | 80 | Discovery, config, state, scripts, OTA. |
| WebSocket   | 80 at `/ws` | Continuous pixel streaming. Binary frames. |
| BLE GATT    | n/a | Onboarding (no WiFi yet), low-power control. |

mDNS service: `_plaiiinlight._tcp` (Bonjour / DNS-SD). The advertised TXT
record carries `nodeName`, `apiVersion`, `paired` (`0`/`1`), `form`,
`type`, and `ledCount`.

## Authentication

A lamp is in one of two pairing modes:

- **unpaired** (default) — no auth. Any client on the LAN may call any
  endpoint.
- **paired** — every request must carry the bearer token from
  `POST /api/pair`. WebSocket clients pass the token via the
  `?token=<…>` query parameter on the upgrade request.

[`sharing-api.md`](sharing-api.md) defines a finer-grained role model (`user`,
`creator`, `admin`) layered on top, plus the multi-key store and
`/api/whoami`. New clients should target the role model; legacy "admin
only" behavior is what a single `pair_token` gets you.

## HTTP API

Stable endpoints. Newer endpoints are added in the companion specs.

### Discovery & state

| Method | Path | Returns / Body |
|---|---|---|
| GET  | `/api`              | Device info — `vendor`, `apiVersion`, `firmwareVersion`, `nodeName`, `ledCount`, `ledType`, `lampType`, `lampForm`, `physicalW/H`, `logicalW/H`, `pixelGroupW/H`. |
| GET  | `/api/state`        | `{on, color:[r,g,b], mode, brightness, currentScript?}` |
| GET  | `/api/whoami`       | `{role, paired}` — see sharing-api. |
| GET  | `/api/pair`         | `{paired:bool, hasToken:bool}` (no auth). |
| POST | `/api/pair`         | Mint or rotate the admin token. Body empty when unpaired (bootstrap), or `Authorization: Bearer <current>` when paired (rotate). Returns `{token}`. |
| DELETE | `/api/pair`       | Unpair the lamp. Requires the current admin token. |

### Control

| Method | Path | Body |
|---|---|---|
| POST | `/api/power`       | `{on: bool}` |
| POST | `/api/color`       | `{colors: [[r,g,b], …]}` — one entry per logical LED (clamped 0–255). |
| POST | `/api/brightness`  | `{brightness: 0..255}` (persisted to NVS). |
| GET  | `/api/brightness`  | `{brightness}` |
| POST | `/api/mode`        | `{mode: "stream" \| "api" \| "js"}` |
| GET  | `/api/limits`      | `{maxBrightness, maxCurrentMa, pixelGroupW, pixelGroupH}` |
| POST | `/api/limits`      | Any subset of the four fields. |

### Local JS scripts

| Method | Path | Body / Returns |
|---|---|---|
| GET    | `/api/js`             | `{scripts:[…], playing:<name>\|null}` |
| GET    | `/api/js/<name>`      | Raw JS source. |
| PUT    | `/api/js/<name>`      | Raw JS source. Validated + eval'd once before write. |
| DELETE | `/api/js/<name>`      | — |
| POST   | `/api/play`           | `{file:"<name>", fps:<n>}` — load and run. |
| POST   | `/api/stop`           | Stop JS playback. |

The script contract is `function shade(x, y, idx, frame, base, params)` —
each call emits one pixel via `emit()` / `emitBright()` / `emitHSV()`. The
shared `lampos/web/shade-runtime.js` runtime is the reference
implementation used by the firmware and every client's preview.

### Updates & reset

| Method | Path | Body |
|---|---|---|
| POST | `/api/ota`          | `application/octet-stream` firmware image. |
| GET  | `/api/ota/info`     | `{version, buildDate, partition, idfVersion}` |
| POST | `/api/reset`        | `{scope: "wifi" \| "full"}` |

### Config pages (HTML)

The lamp also serves HTML pages for direct browser use: `/`, `/control`,
`/compose`, `/stream`, `/js`, `/config`, `/network`, `/mqtt`, `/ota`.
These are convenience UIs, not part of the protocol contract.

## WebSocket streaming (`/ws`)

Connect to `ws://<host>/ws` (or `wss://` if you add a reverse proxy).
For paired lamps, the token goes in the URL: `ws://<host>/ws?token=<…>`.

Binary protocol — each frame is one command:

| Byte(s) | Description |
|---|---|
| `0`     | Command. `0x01` color frame · `0x02` power · `0x03` clear. |
| `1..2`  | (`0x01` only) LED count, big-endian uint16. |
| `3..`   | (`0x01` only) RGB data, 3 bytes per LED. |

Power command: `[0x02, 0x00]` = off · `[0x02, 0x01]` = on.
Clear command: `[0x03]` (no payload).

LEDs auto-enable on the first color frame (no flicker on connect).

**Wormhole lamps** route streamed frames through a per-ring expand step;
see [`wormhole-api.md`](wormhole-api.md) for the frame-size contract and the close
codes the server uses on mode changes.

## BLE GATT

Used primarily for onboarding (no WiFi yet) and low-power control.

| Characteristic | UUID suffix | Access | Purpose |
|---|---|---|---|
| `device_info`     | `…01` | read           | JSON shape matching `GET /api`. |
| `wifi_ssid`       | `…02` | read / write   | SoftAP onboarding. |
| `wifi_pass`       | `…03` | write          | — |
| `power`           | `…04` | read / write   | Single byte. |
| `color`           | `…05` | write          | `r g b` (3 bytes). |
| `mode`            | `…06` | read / write   | One of `stream` · `api` · `js`. |
| `current_script`  | `…07` | read           | UTF-8 script name. |
| `play_<n>`        | `…08…` | write         | Script-specific play knobs. |
| `script_upload_*` | `…09…` | write         | Chunked script upload. |
| `pair_token`      | `…0a` | read (encrypted) | Hand the HTTP token to a bonded peer. |

Paired lamps require an encrypted/bonded link for any write
(`BLE_GATT_CHR_F_WRITE_ENC`); sensitive reads (`current_script`,
`pair_token`) require `READ_ENC`. Just-Works pairing is supported today;
a passkey upgrade is on the roadmap.

[`ble-share.md`](ble-share.md) documents a separate **client-to-client** BLE flow
for handing share keys between phones — that one does not touch the
firmware at all.

## Form factors & geometry

A lamp's `lampForm` field is one of `tower`, `wall`, `wormhole`,
`strip`, `cube` (and a small open set of others). For matrix lamps,
`logicalW/H` and `physicalW/H` may differ when pixel grouping is set —
clients build frames at the **logical** size, the firmware tiles each
logical pixel onto a `pixelGroupW × pixelGroupH` block of physical LEDs.

Wormholes are special: render geometry and physical geometry differ by
construction. See [`wormhole-api.md`](wormhole-api.md).

## Errors

The HTTP API uses standard status codes:

- `400` — bad request body.
- `401` — missing / invalid token (paired mode).
- `403` — token valid but role insufficient ([`sharing-api.md`](sharing-api.md)).
- `404` — endpoint not applicable for this lamp form.
- `409` — request conflicts with current state (e.g. mirror mode on a
  non-wormhole, OTA in progress).
- `5xx` — firmware error; usually transient.

WebSocket close codes follow RFC 6455 (1000 normal) plus
application-specific codes documented in [`wormhole-api.md`](wormhole-api.md).

## Versioning

`GET /api` returns `apiVersion` (currently `"1"`). The contract is
additive within a major version — new fields may appear, existing ones
will not change meaning. A breaking change increments the major.

## Reference implementation

The firmware in this repository implements every endpoint described above
— start in `main/` and follow the route registrations there. The shared
`shade()` runtime that powers JS scripts is in `portal/shade-runtime.js`
and `components/plbc/`.
