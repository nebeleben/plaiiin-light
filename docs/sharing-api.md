# Sharing & role API contract

Frozen contract every workstream builds against. Roles: **user < creator < admin**
(levels 1 < 2 < 3; 0 = invalid/none). A fourth role, **ai** (firmware 2.8.0 / API
2.7.0), sits beside the ladder: see [AI keys](#ai-keys).

## Device key storage (NVS, namespace `plaiiin_cfg`)

- `pair_token` — existing. The lamp's single **admin** key. Mechanism unchanged.
- `share_keys` — new. JSON array, max 32 entries, stored as one NVS string:
  ```json
  [{"id":"a1b2c3d4","key":"<43-char base64url>","role":"user",
    "label":"Guest","revoked":false,"created":1747000000}]
  ```
  - `id` — 8 random hex chars, stable.
  - `key` — 43-char base64url secret (same alphabet as `pair_token`).
  - `role` — `"user"`, `"creator"` or `"ai"` (never `admin`).
  - `label` — optional, ≤32 chars.
  - AI entries also store `p` (permission bitmask), `mb` (brightness cap, -1 =
    none) and `ow` (only-while-on); the HTTP API renders them as below.
  - `revoked` — revoked keys stay listed (for the UI) but fail auth.
- Wiped by factory reset alongside `pair_token`.

## Auth resolution

`pairing_resolve_role(req)` → role:
- unpaired **or** Wi-Fi AP mode → `admin` (open, as today).
- `Bearer` == `pair_token` → `admin`.
- `Bearer` matches a non-revoked `share_keys` entry → that entry's role
  (`user`, `creator` or `ai`).
- else → none.

`pairing_http_check(req, min_role)`: role ≥ min_role → OK; no/bad token → 401;
valid token but role < min_role → 403. An `ai` key skips the role ladder: its
endpoint's permission decides (see [AI keys](#ai-keys)).

## Endpoint → minimum role

- **open** (no auth): `GET /api`, `GET /api/pair`, `POST /api/pair` (unpaired
  bootstrap only), static assets, `/config` + `/network` pages in AP mode.
- **user**: `/api/state`, `/api/power`, `/api/color`, `/api/brightness`,
  `GET /api/base_color`, `/api/mode`, `/api/play`, `/api/play/next|prev|current`,
  `/api/stop`, `GET /api/js`, `GET /api/js/<n>`, `/api/js/<n>/params`,
  `GET /api/grid`, `GET /api/orientation`, `GET /api/storage`,
  `GET /api/form-prompt`, `GET /api/bt`, `GET /api/limits`, `GET /api/fade`,
  `GET /api/ap_js`, WS `/ws`, `GET /api/whoami`.
- **creator**: `PUT|DELETE /api/js/<n>`, `/api/js/validate`,
  `/api/js/compile`, `/api/bench`, `PUT|DELETE /api/form-prompt`,
  `POST /api/fade`, `POST /api/ap_js`, `POST /api/wormhole/creative`.
- **admin**: `POST /api/name` (also an AI key with `rename`), `POST /api/limits`, `POST /api/grid`, `POST /api/orientation`,
  `POST /api/bt`, `POST /api/wormhole`, `/api/mqtt`, `POST /api/ota`,
  `POST /api/reset`, `/api/ai/key`, `POST|DELETE /api/pair`, `/api/keys*`,
  `POST /config`, `POST /network`.

## New endpoints

- `GET /api/whoami` → `{"role":"admin|creator|user","paired":true|false}`; for an
  AI key `{"role":"ai","paired":true,"perms":["control",…],"maxBrightness":N|null,
  "onlyWhenOn":bool}`.
- `GET /api/keys` (admin) → `{"keys":[{id,key,role,label,revoked,created}, …]}`
  — includes the secrets so an admin app can re-render a QR.
- `POST /api/keys` (admin) — body `{"role":"user|creator","label":"…"}` →
  the created entry. `role=="admin"` (or anything else unknown) → 400
  `role must be user, creator or ai`. For `ai` see [AI keys](#ai-keys).
- `PATCH /api/keys/<id>` (admin) — edit an AI key, see [AI keys](#ai-keys).
- `POST /api/keys/<id>/revoke` (admin) → `{"status":"ok"}` (sets `revoked`).
- `DELETE /api/keys/<id>` (admin) → `{"status":"ok"}` (drops the entry).

## AI keys

Role `ai` for agents (the Claude MCP server and the like). Made by an admin,
handed over as the usual share QR/link. Instead of the role ladder the key
carries a permission list chosen per key, plus two optional extras.

| Permission | Covers |
|---|---|
| `control` | `POST /api/power\|color\|brightness\|play\|play/next\|play/prev\|stop\|fade`, `PUT\|POST /api/mode`, `POST /api/scenes/<n>/activate` |
| `effects` | `PUT\|DELETE /api/js/<n>`, `PUT /api/js/<n>/params`, `POST /api/js/validate`, `POST /api/js/compile` |
| `images` | `POST /api/frame`, `POST /api/images`, `POST /api/images/<id>/show\|label`, `DELETE /api/images/<id>` |
| `scenes` | `PUT\|DELETE /api/scenes/<n>`, `POST /api/scenes/<n>/capture` (activate is `control`) |
| `stream` | WebSocket `/ws` |
| `ota` | `POST /api/ota` — the one admin action that can be granted |
| `rename` | `POST /api/name` |

- **Reads**: a `GET` that a `user` may make (`/api`, `/api/state`, js, images,
  scenes, grid, form-prompt, limits, storage, ota/info,
  `/api/whoami`, …) always works, with no permission. Admin/creator-only
  reads (e.g. `GET /api/keys`) stay closed.
- **Never grantable** (always 403 `insufficient role`): pair/unpair, keys,
  Wi-Fi/network, MQTT, limits/grid/orientation, BT, factory reset, AI
  provider key, and any write not listed above.
- A write whose permission the key lacks → 403
  `not permitted for this AI key`.
- **`WS /ws`**: a refusal (any role, not just `ai`) completes the upgrade and
  the lamp then closes the socket immediately; there is no HTTP 403 on `/ws`.
- **Checked at connect time**: for `/ws`, the permission and only-while-on
  are checked when the connection opens. An open stream keeps running after
  the key is revoked, deleted or edited, until it disconnects.
- **`stream` can switch power**: a stream connection can also send the power
  command, so granting `stream` lets the key turn the lamp on and off.
- **`ota` is effectively full control**: custom firmware can read every secret
  on the lamp. Apps should warn when this switch is turned on.
- **`onlyWhenOn`**: while the lamp is off, everything except reads and
  `POST /api/power` is refused with 403 `lamp is off`.
- **`maxBrightness`** (0..255): `POST /api/brightness` made with this key is
  clamped to the cap and the response echoes the capped value. It applies only to
  requests made with that key — not buttons, swarm, MQTT, BLE or other clients.
- **Limit**: at most 4 AI keys (`400 ai-key limit reached`); revoked AI keys
  still count until deleted. The overall 24-key limit applies too.
- `POST /api/name` `{"name":"…"}` (admin or `rename`): 1..32 **bytes**, no quotes, backslashes or control characters (else 400); stores
  the name, replies `{"status":"ok","rebooting":true}`, and the lamp reboots
  ~800 ms later.

`POST /api/keys` for an AI key:
`{"role":"ai","label":"Claude","perms":["control","effects"],"maxBrightness":128,"onlyWhenOn":true}`.
`perms` is required, a non-empty list of the names above (unknown name → 400
`unknown permission: <name>`); `maxBrightness` is 0..255 or `null`/absent for no
cap; `onlyWhenOn` defaults to false.

`PATCH /api/keys/<id>` (admin) takes any of `perms`, `maxBrightness` (`null`
clears the cap), `onlyWhenOn`, `label`, and returns the updated entry. Editing
needs no new key. 400 `not an ai key` for user/creator entries, 404 `no such key`.

Rendered entry (create, list, patch): the usual `{id,key,role:"ai",label,
revoked,created}` plus `perms` (names, in the table's order), `maxBrightness`
(number or `null`) and `onlyWhenOn`.

## Share QR

A plain URL: `http://<host>/config?t=<key>`
- Browser: opens it; the existing `auth.js` reads `?t=` into localStorage.
- Mobile app: scans, extracts `<host>` + `<key>`, then
  `GET /api/whoami` (Bearer key) for the role and `GET /api` for identity,
  and adds a `KnownDevice(transport=wifi, role, shared=true)`.

## Client model

- `KnownDevice.role` — `admin|creator|user|ai`, persisted; default `admin` for
  self-onboarded/unpaired lamps. Refreshed via `/api/whoami` on connect.
- `KnownDevice.shared` — true when added via a received share.
- *owned-elsewhere* (derived): saved, mDNS `paired=1`, not `shared`, our token
  401s → show offline + red lock "owned".

## Role-aware UI

- **user**: dashboard control (power/color/select-script); LampDetail Scripts
  pane = select/play only. No Compose/Stream/Settings tabs.
- **creator**: + Compose, Stream, full Scripts edit, Form tab. No
  Config/MQTT/API tabs, no Share-admin.
- **admin**: everything, incl. the Share tab, Config/MQTT/API, OTA.
