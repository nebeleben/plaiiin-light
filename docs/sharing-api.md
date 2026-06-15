# Sharing & role API contract

Frozen contract every workstream builds against. Roles: **user < creator < admin**
(levels 1 < 2 < 3; 0 = invalid/none).

## Device key storage (NVS, namespace `plaiiin_cfg`)

- `pair_token` — existing. The lamp's single **admin** key. Mechanism unchanged.
- `share_keys` — new. JSON array, max 32 entries, stored as one NVS string:
  ```json
  [{"id":"a1b2c3d4","key":"<43-char base64url>","role":"user",
    "label":"Guest","revoked":false,"created":1747000000}]
  ```
  - `id` — 8 random hex chars, stable.
  - `key` — 43-char base64url secret (same alphabet as `pair_token`).
  - `role` — `"user"` or `"creator"` only (never `admin`).
  - `label` — optional, ≤32 chars.
  - `revoked` — revoked keys stay listed (for the UI) but fail auth.
- Wiped by factory reset alongside `pair_token`.

## Auth resolution

`pairing_resolve_role(req)` → role:
- unpaired **or** Wi-Fi AP mode → `admin` (open, as today).
- `Bearer` == `pair_token` → `admin`.
- `Bearer` matches a non-revoked `share_keys` entry → that entry's role.
- else → none.

`pairing_http_check(req, min_role)`: role ≥ min_role → OK; no/bad token → 401;
valid token but role < min_role → 403.

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
- **admin**: `POST /api/limits`, `POST /api/grid`, `POST /api/orientation`,
  `POST /api/bt`, `POST /api/wormhole`, `/api/mqtt`, `POST /api/ota`,
  `POST /api/reset`, `/api/ai/key`, `POST|DELETE /api/pair`, `/api/keys*`,
  `POST /config`, `POST /network`.

## New endpoints

- `GET /api/whoami` → `{"role":"admin|creator|user","paired":true|false}`.
- `GET /api/keys` (admin) → `{"keys":[{id,key,role,label,revoked,created}, …]}`
  — includes the secrets so an admin app can re-render a QR.
- `POST /api/keys` (admin) — body `{"role":"user|creator","label":"…"}` →
  the created entry. 403 if `role=="admin"`.
- `POST /api/keys/<id>/revoke` (admin) → `{"status":"ok"}` (sets `revoked`).
- `DELETE /api/keys/<id>` (admin) → `{"status":"ok"}` (drops the entry).

## Share QR

A plain URL: `http://<host>/config?t=<key>`
- Browser: opens it; the existing `auth.js` reads `?t=` into localStorage.
- Mobile app: scans, extracts `<host>` + `<key>`, then
  `GET /api/whoami` (Bearer key) for the role and `GET /api` for identity,
  and adds a `KnownDevice(transport=wifi, role, shared=true)`.

## Client model

- `KnownDevice.role` — `admin|creator|user`, persisted; default `admin` for
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
