# Wormhole render-mode API contract

Frozen contract that every wormhole workstream builds against.

A **wormhole lamp** is N rings of 24 LEDs. The chain is `ring r` = physical
indices `r*24 .. r*24+23`. Two render modes:

- **strip** (default, out-of-the-box) — the effect renders the whole construct
  as a **flat strip**. Render grid = `(24·ringCount) × 1`; a script sees `idx`
  = `0 .. 24·rings-1` and, if it wants ring structure, derives
  `ring = floor(idx / 24)` and `position-on-ring = idx mod 24` itself (24 is the
  fixed wormhole ring size). `x = idx`, `y = 0`.
- **mirror** — the effect renders **one** 24-LED ring; firmware tiles that ring
  onto every physical ring with per-ring transforms. Firmware-only; the VM and
  `shade()` never see it.

## Geometry: render vs physical

The one architectural change: **render geometry ≠ physical geometry**.

| Mode   | Render grid       | Render pixels   | Physical LEDs    |
|--------|-------------------|-----------------|------------------|
| strip  | `(24·rings) × 1`  | `24*rings`      | `led_count`      |
| mirror | `24 × 1`          | `24`            | `led_count`      |

Strip mode is a flat strip — `h` is always `1`. A ring-aware effect derives the
rings from `idx` (`ring = floor(idx/24)`); a flat effect just uses `idx`. An
effect built for strip mode that is played in mirror mode sees `w = 24`, so
`rings` collapses to `1` and it degrades to a single ring rather than breaking.

`led_count == 24 * rings` always holds for a wormhole. The player renders a
**render buffer** of `renderPixels` pixels, then `wormhole_expand()` produces
the **physical buffer** of `led_count` pixels that goes to `led_control`.

## `wormhole_expand()` — the single tiling function

One function, used by **both** the JS player and the WebSocket stream path.

```
void wormhole_expand(const led_color_t *render, int render_pixels,
                     led_color_t *physical, int rings, bool mirror);
```

For each ring `r` (0..rings-1) and local position `p` (0..23):

```
srcRing  = mirror ? 0 : r
reverse  = face[r] ^ direction[r] ^ (mirror ? creativeReverse[r] : 0)
shift    = (physOffset[r] + (mirror ? creativeOffset[r] : 0)) mod 24
bright   = mirror ? creativeBrightness[r] : 1.0

q = reverse ? (23 - p) : p
s = (q + shift) mod 24
physical[r*24 + p] = render[srcRing*24 + s] * bright
```

All index arithmetic is mod-24 and in-bounds by construction — the only failure
mode (`led_count % 24 != 0`) is caught by the geometry gate, never reached here.

**Identity guarantee:** in strip mode with default physical config (all rings
`face=0,direction=0,offset=0`), `wormhole_expand` is a byte-identical copy.
Existing wormhole builds are unaffected. `wormhole_expand` only runs when
`form == "wormhole"`; all other forms keep the exact current code path.

### The three physical fields (per ring)

Set-once mounting facts, applied in **both** modes. NVS-stored, see below.

- `face` (0|1) — the half-turn the ring is mounted at. A ring facing the
  opposite way has its angular sweep mirrored to a fixed observer →
  contributes a **reversal** bit.
- `direction` (0|1) — winding direction the LEDs were soldered in →
  contributes a **reversal** bit.
- `offset` (0..23) — physical index sitting at the ring's canonical 12-o'clock
  → contributes an **additive** bit.

Net `reverse` is the XOR of `face`, `direction` (and creative). Net `shift` is
the mod-24 sum. `face` and `direction` are kept distinct because they are two
independent physical facts, even though both reduce to a reversal bit.

### The three creative fields (per ring)

**Mirror mode only**, **per-lamp** (not per-effect). NVS-stored.

- `reverse` (bool) — extra reversal bit, XORed in.
- `offset` (0..23) — extra additive bit, summed in.
- `brightness` (0.0..1.0) — per-ring output scale. Range is **0..1 only**.

Physical `offset` and creative `offset` are not in conflict — just two addends
of one mod-24 sum.

## Config keys (NVS, namespace `plaiiin_cfg`)

All wiped by factory reset alongside the rest of the namespace.

- `wh_mode` — string, `"strip"` (default) or `"mirror"`.
- `wh_rings` — i32, explicit ring count. Default `led_count / 24`. v1=2, v2=4.
- `wh_phys` — JSON array string, one object per ring, physical fields:
  ```json
  [{"face":0,"direction":0,"offset":0},
   {"face":1,"direction":1,"offset":0}]
  ```
  Fewer entries than `wh_rings` → missing rings default to all-zero.
- `wh_creative` — JSON array string, one object per ring, creative fields:
  ```json
  [{"reverse":false,"offset":0,"brightness":1.0},
   {"reverse":false,"offset":0,"brightness":1.0}]
  ```
  Missing rings default to `{reverse:false, offset:0, brightness:1.0}`.

Both arrays follow the `share_keys` pattern: a JSON array packed into one NVS
string. Unset keys fall back to the defaults above.

## Geometry gate

`mirror` mode is **allowed only** when all of:

- `lamp_form == "wormhole"`, **and**
- `led_count % 24 == 0`, **and**
- `wh_rings == led_count / 24`.

`wormhole_mirror_allowed()` returns this boolean. A request to set
`mode=="mirror"` while it is false is rejected (see endpoints). On boot, if
`wh_mode=="mirror"` but the gate fails, firmware falls back to `strip` and logs
a warning — it never plays into an invalid geometry.

## HTTP endpoints

JSON style matches `/api/grid` / `/api/orientation`.

### `GET /api/wormhole` — role: **user**

```json
{
  "mode": "strip",
  "rings": 2,
  "mirrorAllowed": true,
  "renderPixels": 48,
  "streamPixels": 48,
  "physical": [
    {"face":0,"direction":0,"offset":0},
    {"face":1,"direction":1,"offset":0}
  ],
  "creative": [
    {"reverse":false,"offset":0,"brightness":1.0},
    {"reverse":false,"offset":0,"brightness":1.0}
  ]
}
```

`renderPixels` / `streamPixels` = `24` in mirror mode, `24*rings` in strip mode.
A streaming client reads `streamPixels` to know the frame size to send.

For non-wormhole lamps the endpoint returns `404`.

### `POST /api/wormhole` — role: **admin**

Body: any subset of `{"mode","rings","physical"}`.

- `mode` — `"strip"` | `"mirror"`. `"mirror"` when `mirrorAllowed` is false →
  `409 Conflict`, body `{"error":"mirror not allowed for this geometry"}`.
- `rings` — i32 ≥ 1.
- `physical` — full JSON array as in `wh_phys`.

Side effects on a `mode` or `rings` change:
- the JS player is re-initialised (render geometry changes) — a brief
  re-init flicker is expected and acceptable;
- if a WebSocket stream is active it is **closed** with close code `4002`
  (see below).

Returns the same body as `GET /api/wormhole`.

### `POST /api/wormhole/creative` — role: **creator**

Body: `{"creative":[ … ]}` (full array) or `{"ring":N,"reverse":…,"offset":…,
"brightness":…}` (single ring patch). Stored regardless of current mode; only
applied in mirror mode. No player re-init, no stream close — creative knobs
take effect on the next frame. Returns the `GET /api/wormhole` body.

## WebSocket streaming (`/ws`)

Binary protocol unchanged (`CMD_COLOR_FRAME=0x01`, `CMD_POWER=0x02`,
`CMD_CLEAR=0x03`). What changes for a wormhole lamp:

- A streaming client must first `GET /api/wormhole` and send `CMD_COLOR_FRAME`
  frames carrying exactly `streamPixels` pixels.
- The firmware routes wormhole stream frames through the **same**
  `wormhole_expand()` the player uses, then to `led_control`.
- **strip** mode: `streamPixels == 24*rings` — the frame is the full render
  grid (firmware still applies the physical per-ring transform).
- **mirror** mode: `streamPixels == 24` — the frame is one ring; firmware
  tiles it onto all rings.

### Stream rejection — server closes the WebSocket

No error frame, no queueing. The server sends a WebSocket **Close** frame with
an application close code and stops:

- `4001` — **frame size mismatch**: a `CMD_COLOR_FRAME` whose pixel count is
  not `streamPixels` for the current mode.
- `4002` — **render mode changed**: a `POST /api/wormhole` changed `mode` or
  `rings` while this stream was open; the next frame (or the change itself)
  closes the socket.

The client is expected to reconnect, re-`GET /api/wormhole`, and resume with
the new frame size.

## Form-prompt: mode-aware (`/api/form-prompt`)

For `form == "wormhole"`, `GET /api/form-prompt` gains two fields:

```json
{
  "form": "wormhole",
  "mode": "strip",
  "default": "<descriptor for the current mode>",
  "override": "",
  "hasOverride": false,
  "effective": "<override if set, else default-for-current-mode>",
  "byMode": {
    "strip":  "<strip-mode descriptor>",
    "mirror": "<mirror-mode descriptor>"
  }
}
```

- `byMode` always carries **both** firmware default descriptors so a client can
  preview/switch without a round-trip. Non-wormhole forms omit `byMode` and
  `mode` entirely (response shape unchanged for non-wormhole lamps).
- `default` and `effective` reflect the **current** `wh_mode`.
- A user `override` (PUT `/api/form-prompt`) applies **globally** — it is the
  `effective` value for both modes (v1 decision). `byMode` still shows the two
  defaults.
- The client picks `byMode.strip` vs `byMode.mirror` to inject into AI compose
  prompts based on the mode it wants to compose for.

### Form-template file (`form-template/wormhole.txt`)

The burned template gains two sections, split by line-leading markers:

```
@@strip
PHYSICAL FORM: wormhole (strip mode). … {rings} … {count} …
@@mirror
PHYSICAL FORM: wormhole (mirror mode). Render ONE 24-LED ring …
```

`form_prompt.c` parsing rule: if the file contains `@@strip` / `@@mirror`
markers, the section for the current mode is used (and both go into `byMode`);
if no markers are present the whole file is used for both modes. Non-wormhole
templates have no markers and are unaffected. `{placeholder}` substitution runs
per section as before.

## `@mode` script annotation

Effect authors add a leading comment `// @mode mirror` or `// @mode strip` to
declare which render mode a `shade()` script is written for. As of Phase 41:

- The PLBC compiler parses `@mode` into the program (`plbc_program_t.mode`:
  `-1` none / `0` strip / `1` mirror) and serializes it in the `.bc` header
  (format bumped to v2 — stale v1 `.bc` are recompiled on boot via
  `js_storage_bc_current`).
- `js_api_play()` auto-switches `wh_mode` to the effect's declared mode on a
  wormhole lamp before the player inits its render grid (reusing the geometry
  gate + strip fallback). Effects with no `@mode` leave `wh_mode` untouched.
- The declared mode is surfaced to clients as the top-level `"mode"` field of
  `GET /api/js/<name>/params`, so the dashboard tile shows a render-mode toggle
  (wired to `POST /api/wormhole`) for effects that carry one.
- The `POST /api/wormhole` reinit replay uses `js_api_play_ex(…, autoswitch=
  false)` so a hand-set mode isn't immediately reverted by the auto-switch.

A new tune type rides along: `// @param NAME switch = 0` declares a 0/1 toggle
(`plbc_param_t.type == PLBC_PARAM_SWITCH`), surfaced as `"type":"switch"` in the
params JSON and rendered as a switch (not a knob) by all clients.

## Out of scope

- "Rocket areas" / heterogeneous segment tiling — a separate, larger feature.
- Mirror-aware preview in `shade-runtime.js` — preview renders a single
  24-LED ring in mirror mode. Documented caveat, not a bug.
