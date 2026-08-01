# PlaiiinLightOS — Technical Specs

The full technical reference for PlaiiinLightOS. New here? Start with the
[README](README.md).

Open firmware for ESP32 microcontrollers driving addressable LED
strips and matrices. AI-aware, scriptable, OTA-updatable, with HTTP,
WebSocket, and BLE control surfaces.

Supported LED chipsets (config value → unknown values fall back to `ws2812`):

| Config value | Chipset(s) | Wiring | Format |
|---|---|---|---|
| `ws2812` | WS2812 / WS2812B / NeoPixel | one-wire | GRB, 3 bytes/LED |
| `sk6812` | SK6812 (RGB) | one-wire | GRB, 3 bytes/LED |
| `sk6812w` | SK6812 (RGBW) | one-wire | GRBW, 4 bytes/LED |
| `sk9822` | SK9822 / APA102 | two-wire (data + clock) | BGR + brightness |

### What's in this repository

- `components/`, `main/`, `portal/` — the firmware source.
- `scripts/` — build, flash, and profile-burn tooling.
- `hardcoded/<form>/` — form-specific effects compiled into the binary
  for that form (`tower`, `display`, `cube`, `rocket`, …).
- `effects/<form>/` — form-specific JavaScript (`shade()`) effects flashed
  to the device's SPIFFS `storage` partition on a `--full` profile burn
  (not carried by OTA).
- `profiles/<family>/<device>` — per-device NVS identity (LED count, type,
  GPIO, lamp form) written by `profile-burn`.
- `docs/` — the wire contract every client targets (HTTP, WebSocket,
  BLE), plus role-based access, wormhole render-mode, and BLE-share specs.
- `LICENSE` — Apache 2.0 for everything in this repository.

### Prerequisites
- ESP-IDF v5.3+ (uses Python 3.12 env — Python 3.14 breaks `idf_tools`; prefix builds with `IDF_PYTHON_ENV_PATH=$HOME/.espressif/python_env/idf5.3_py3.12_env`)

### Setup
```bash
./scripts/setup.sh    # Installs ESP-IDF v5.3.2 and tools
```

### Build (per form)

There is no universal binary — each release ships one binary per lamp **form**.
`scripts/build.sh --form <name>` compiles the matching `hardcoded/<form>/`
effects into the image, stamps the version from `version.properties`, and drops
two artifacts into `build/dist/`:

- `plaiiinlight_os-<ver>-<form>-app.bin` — OTA payload (app only, ~1 MB)
- `plaiiinlight_os-<ver>-<form>-flash.bin` — merged full-flash image
  (bootloader + partitions + otadata + app, for USB first-flash at offset 0)

Available forms:

| Form | Typical hardware | Example lamp type |
|---|---|---|
| `tower` | cylindrical matrix table lamp | `matrix16x8` / `matrix16x32` |
| `display` | flat 16×16 wall matrix | `matrix16x16` |
| `cube` | 5-sided LED cube | `matrix8x8` |
| `rocket` | stacked ring segments | `strip` |
| `wormhole` | stacked 24-LED rings (tunnel) | `strip` |
| `strip` | single 1-D LED chain | `strip` |

```bash
./scripts/build.sh --form display
# → build/dist/plaiiinlight_os-<ver>-display-app.bin
#   build/dist/plaiiinlight_os-<ver>-display-flash.bin
```

`/api/ota` refuses a binary whose embedded form doesn't match the device (409),
so pushing a `tower` binary to a `display` lamp is rejected, not a brick.

Per-device hardware identity (LED count, GPIO, lamp type, form) lives in
`profiles/<family>/<device>` and is written to NVS with `profile-burn`:

```bash
./scripts/profile-burn.sh /dev/cu.usbserial-0001 tower/tower8v2          # NVS only (~2 s)
./scripts/profile-burn.sh --full /dev/cu.usbserial-0001 tower/tower8v2   # erase + flash + NVS + byForm effects
```

### Configure
```bash
source ~/esp/esp-idf/export.sh
idf.py menuconfig     # Set LED pin, count, WiFi, node name
```

Key settings under "PlaiiinLightOS Configuration":
- **LED Strip**: GPIO pin, LED count, LED type (`ws2812`, `sk6812`, `sk6812w`, `sk9822`), lamp type, lamp form
- **WiFi**: SoftAP SSID prefix, HTTP port
- **Node**: Name, vendor, API version

All settings can also be changed at runtime via the web UI at `/config` (stored in NVS, survives reboots and OTA updates).

### Build & Flash (USB) - First Time

The initial flash must be done via USB. After that, use OTA for wireless updates.

```bash
./scripts/build.sh --form display
./scripts/profile-burn.sh --full /dev/cu.usbserial-0001 display/<device>
```

`profile-burn --full` erases the chip, writes the merged flash image, burns the
device's NVS identity, and installs its byForm effects in one shot.

To pre-configure WiFi and LED settings without the captive portal, generate an NVS image:

```bash
# Create nvs_data.csv with your settings:
#   key,type,encoding,value
#   plaiiin_cfg,namespace,,
#   wifi_ssid,data,string,YourSSID
#   wifi_pass,data,string,YourPassword
#   led_pin,data,i32,26
#   led_count,data,i32,256
#   lamp_type,data,string,matrix16x16
#   lamp_form,data,string,display

python3 $IDF_PATH/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py \
  generate nvs_data.csv nvs.bin 0x6000

esptool.py --chip esp32 -p /dev/cu.usbserial-* write_flash 0x9000 nvs.bin
```

### First Boot (without pre-configured NVS)
1. ESP32 starts a WiFi access point: `PlaiiinLight-XXXX`
2. Connect to it, open `http://192.168.4.1/network`
3. Enter your WiFi credentials, click Save & Reboot
4. Device reboots and connects to your WiFi
5. Configure device/LED settings at `http://<device-ip>/config`

### OTA Update (Wireless)

After the initial USB flash, all subsequent firmware updates are done wirelessly:

**Via web browser:**
1. Open `http://<device-ip>/ota`
2. The page shows current firmware version, build date, and active partition
3. Drop the matching `build/dist/plaiiinlight_os-<ver>-<form>-app.bin` onto the upload zone (or click to browse)
4. Upload progress bar shows transfer status
5. Device flashes to the inactive OTA partition, reboots, and the page auto-reloads

**Via command line:**
```bash
# Build the binary for this device's form
./scripts/build.sh --form display

# Upload (replace IP with your device's address)
curl -X POST http://<device-ip>/api/ota \
  -H "Content-Type: application/octet-stream" \
  -H "Expect:" \
  --data-binary @build/dist/plaiiinlight_os-<ver>-display-app.bin
```

The `-H "Expect:"` is required: curl auto-adds `Expect: 100-continue` for large
request bodies, and the ESP-IDF httpd doesn't complete that handshake — the
upload resets mid-stream (often after ~128 KB, sometimes surfacing as a
spurious 401). Suppressing the header lets the binary stream straight through.

The binary's embedded form must match the device — a cross-form upload is
rejected with `409 Conflict`. OTA carries the app only; byForm JS effects on the
`storage` partition are left untouched.

The partition table uses dual OTA slots (`ota_0` + `ota_1`), alternating on each update. NVS config (WiFi, LED settings, brightness, last color) is preserved across all OTA updates.

### AI Compose

The `/compose` page lets you describe LED patterns in natural language. An AI generates the pattern as either pixel data or a JavaScript animation function, which is rendered on the grid preview and streamed to the physical LEDs via WebSocket.

**Setup:**
1. Get an API key from [console.anthropic.com](https://console.anthropic.com) (Claude) or [platform.openai.com](https://platform.openai.com) (OpenAI)
2. Go to `http://<device-ip>/config`, scroll to **AI Integration**
3. Select your provider, paste your API key, and set the model:
    - Claude: `claude-haiku-4-5` (fast/cheap), `claude-sonnet-4-6`, `claude-opus-4-8`
    - OpenAI: `gpt-4o-mini` (fast/cheap), `gpt-4o`
4. The API key is stored in your browser's localStorage (never sent to the ESP32)

**Usage:**
1. Open `http://<device-ip>/compose`
2. Type a prompt: *"draw a rainbow"*, *"create a fire animation"*, *"pixel art of a heart"*
3. The AI generates the pattern and displays it on the grid preview
4. Click **Play** to stream it to the LEDs (auto-connects WebSocket)
5. Refine iteratively: *"make it scroll faster"*, *"add blue sparkles"*, *"change background to black"*
6. Tweak the script's **tunes** (parameter knobs/switches) and the **Brightness** slider — the preview re-bakes live
7. Click **Stop** to turn off the LEDs

The AI supports two output formats automatically:
- **Pixel data** (JSON) — for static images and short frame sequences
- **Generator code** (JavaScript) — for complex animations, gradients, and math patterns. The AI writes a per-pixel `shade(x, y, idx, frame, base, params)` shader — the same contract the device runs — which the browser previews via the shared `shade-runtime.js` emulator (up to 300 frames). **Save to device** stores it as a named script.

Chat history is maintained, so follow-up prompts can reference and modify previous results.

### GIF/Image Streaming

The `/stream` page lets you stream images and animated GIFs to the LED matrix:

1. Open `http://<device-ip>/stream`
2. Drag & drop a GIF or image onto the drop zone
3. Animated GIFs are decoded frame-by-frame (pure JS, works in all browsers)
4. The image is automatically resized and mapped to the LED grid
5. For matrices, serpentine row mapping is applied (odd rows reversed)
6. Click **Switch to Stream** to connect WebSocket, then **Play** to start
7. Adjust FPS and brightness as needed

### Guardrails (current + brightness caps)

To protect the hardware, `led_control` clamps RGB to `[0, 255]`, rejects frames with the wrong length, and enforces two user-configurable limits (persisted in NVS, editable from `/js → Guardrails` or `POST /api/limits`):

- **Max brightness** (`maxBrightness`, 0–255) — absolute ceiling on requested brightness.
- **Max current** (`maxCurrentMa`, mA, `0` = disabled) — the renderer estimates draw at 20 mA per channel at 255 and scales brightness down so total draw for the current frame never exceeds this cap.

Both caps apply to every frame path: `POST /api/color`, WebSocket streams, and local JS playback.

### Local JS scripts

User scripts are per-pixel **shaders**, not whole-frame renderers. Each script
defines a `shade(x, y, idx, frame, base, params)` function that emits one colour
per LED. Scripts are compiled to **PLBC** (PlaiiinLight Bytecode — a compact
stack VM purpose-built for fast per-pixel evaluation on the ESP32; it replaced
the earlier JerryScript/mJS engines, whose per-call overhead was too high). The
device plays the bytecode on a dedicated FreeRTOS task at the configured FPS and
pushes each frame through `led_control_set_logical`, so pixel-grouping +
guardrails apply automatically.

Scripts are stored on a dedicated 256 KB SPIFFS `storage` partition (see
partition table below) — both the `.js` source (source of truth for editing)
and the compiled `.bc`. Manage them at `http://<device-ip>/js` — list, edit,
save, play/stop, upload, delete, plus per-script **tunes** (parameter knobs).
The `/compose` page's **Save to device** button stores the current AI-generated
`shade()` script as a named script.

Script API (same contract on device + in `/compose`):

```js
// @param speed 0.3..6 = 1.8 Average fall speed (rows per second)
// @param headWhite 0..1 = 0.7 How much the leading pixel washes to white

function shade(x, y, idx, frame, base, params) {
  let bright = ...;          // per-pixel maths
  emit(r, g, b);             // raw RGB 0..255, OR
  emitBright(bright);        // scale the base colour by 0..1
  // call exactly one emit*() per pixel
}
```

In scope: `x, y, idx` (pixel), `frame`, `w, h` (logical grid), `time` (ms since
play started), `playStart` (seed), `base` (`.r/.g/.b`), `params` (declared
`@param` values). Built-ins: `sinLUT`, `cosLUT`, `floor`, `ceil`, `round`,
`abs`, `sqrt`, `pow`, `min`, `max`, `clamp01`, `hash`, `random`, `emit`,
`emitBright`.

Parameters are declared with `// @param NAME MIN..MAX = DEF [desc]` (a tune
knob) or `// @param NAME switch = DEF [desc]` (a toggle); they surface as the
`/js` and `/compose` tunes and over `GET/PUT /api/js/<name>/params`. On save the
server compiles the script to bytecode to surface parse/semantic errors before
it's written to flash. The browser-side `shade-runtime.js` emulator mirrors the
VM exactly so previews on `/compose`, `/js`, and the mobile/desktop clients
match the hardware.

### Draw mode (frame)

`frame` is a fourth lamp mode (alongside `api`, `js`, `stream`) that paints
and remembers a single still image drawn by a client. Enter it via the
existing mode endpoint, `POST /api/mode {"mode":"frame"}`; the JS player
stops immediately if it was running. Like the other modes it's persisted
to NVS, so a reboot resumes the stored frame (repainted on power-on if the
lamp was on when it lost power).

- **Content**: `POST /api/frame` uploads a raw RGB image sized exactly to
  the lamp's **logical** grid (`logicalW × logicalH × 3` bytes, row-major,
  no header) and saves it to flash; it repaints immediately when the lamp
  is already in frame mode, on, and not mid-stream. `GET /api/frame` reads
  the stored image back — useful for re-opening a drawing in an editor.
  Pixel grouping, rotation, and serpentine wiring are applied by the
  firmware at paint time, same as every other content path.
- **baseColor contract**: identical to `js` mode — `POST /api/color`
  (solid or per-pixel body) still updates and persists `baseColor` but
  does **not** repaint the panel, so a connected color picker doesn't
  fight the drawn image.
- **Streaming**: a live WebSocket connection suspends frame mode (the
  stream owns the panel for as long as it's connected); the stored frame
  is repainted automatically the moment the stream closes.
- **State**: `GET /api/state` reports `"mode":"frame"` whenever the lamp
  is in frame mode and not streaming — it reports `"mode":"stream"` for
  the duration of a live WebSocket connection, same as the other modes.
- **Wormhole lamps**: frame mode paints the logical grid directly in every
  render mode — there's no per-ring mirror-tiling special case the way
  there is for streamed/`js` content on wormhole forms.

The intended producer is the **Draw** tab in the macOS/iOS/Android apps
(creator role or above), but the contract is open — any client can save
and read frames via `/api/frame`. See
[protocol.md](docs/protocol.md#draw-mode-apiframe) for the exact byte
layout, status codes, and on-flash format.

### Swarm mode

Masterless lamp-to-lamp state propagation over **ESP-NOW** — the
connectionless ESP32-to-ESP32 broadcast riding the WiFi radio. Change one
lamp (any surface: HTTP, BLE, MQTT, a physical button) and every other
member of its swarm follows, typically within a couple hundred
milliseconds. There is no hub, no cloud, and no "master" lamp — every
member broadcasts its own state changes and relays what it hears, once.

A lamp belongs to **at most one swarm at a time**. Membership is
provisioned by a client over the lamp's existing authenticated HTTP
surface (`/api/swarm`, admin role): the client generates a random 16-hex
swarm id and a 32-byte (64-hex) swarm key and pushes both to every member
lamp. Swarm mode can be toggled on/off without losing that provisioning —
`POST /api/swarm/enable` stops send/receive while leaving the NVS block
intact; only `DELETE /api/swarm` (leave) or a full factory reset erases
it.

**Security model.** Swarm packets are authenticated, not encrypted —
anyone in radio range can see a packet's shape, but only holders of the
swarm key can forge one a member will accept:

- Every packet carries an **HMAC-SHA256** over the whole packet (magic
  through effect name), truncated to 16 bytes, keyed with the
  client-provisioned 32-byte swarm key, verified with a constant-time
  compare.
- Each origin (identified by its STA MAC) stamps a **strictly increasing
  sequence number**; a receiver tracks the last-seen seq per origin (up
  to 16 origins, LRU-evicted) and accepts only `seq` strictly greater
  than the last one seen from that origin — a captured packet can never
  be replayed at a receiver that already saw it or anything newer from
  the same origin.
- Sequence numbers survive reboot: the counter is persisted to NVS every
  64 increments, and boot restores **persisted + 64** — verified on
  hardware that a rebooted origin never reuses a sequence number a peer
  has already accepted.
- Every member **relays what it hears exactly once** (hop 0 →
  re-broadcast as hop 1 after a random 10–50 ms jitter; hop 1 is never
  relayed further), giving multi-hop range without needing any topology.
  A lamp ignores its own packets on receive.
- Packets that don't decode, don't match the configured swarm id, or fail
  the HMAC check are dropped silently and counted (`dropAuth` /
  `dropReplay` in `/api/swarm/stats`) — never logged with key material.

**Propagation.** Packets carry a full state **snapshot** of the origin
(on/off, color, brightness, mode, effect name), not a delta. Local
changes are coalesced — a burst of rapid changes (a color-picker drag)
collapses to roughly one broadcast per 100 ms (≤10 Hz), not one packet
per change. Receivers always apply on/off, brightness, and color (color
lands on `baseColor`, the same blend semantics `js`/`frame` mode already
use); mode only ever propagates as `api` or `js` — a lamp locally in
`frame` or `stream` is switched by an incoming swarm packet (swarm wins),
but a lamp whose *own* current mode is something else when it originates
a change sends a third mode value telling receivers to apply on/color/
brightness only and leave mode + effect untouched. Effect propagation is
**by name**: a receiver with a saved script of that name plays it; one
without keeps its current effect/mode and still applies the rest of the
snapshot (the "color fallback") — no script content ever crosses the
swarm. A power-off snapshot never restarts a stopped effect, and a
receiver already running the named script isn't re-started.

**NVS block** (survives OTA and USB reflash unless the `nvs` partition
itself is skipped):

| Key | Type | Meaning |
|---|---|---|
| `sw_id` | string (16 hex) | Swarm id |
| `sw_key` | string (64 hex) | HMAC-SHA256 key — **secret at rest**, never returned by any GET, never logged |
| `sw_chan` | i32 | WiFi channel pinned for AP-less operation (0 = unset/current) |
| `sw_on` | i32 0\|1 | Swarm feature enabled |
| `sw_seq` | i32 | Last-persisted TX sequence number |

All five keys are wiped **only** by a full factory reset
(`factory_reset_full`) — a WiFi-only reset (10 s button hold, or
`POST /api/reset {"scope":"wifi"}`) leaves swarm membership untouched.
This matches the precedent set by share keys: swarm config is durable
device configuration, not network-transient state.

**Channel caveat (honesty note).** When a lamp is associated to its home
AP, ESP-NOW rides the AP's WiFi channel automatically — nothing to
configure. A lamp running AP-less (BLE-only onboarding, no WiFi
association) instead pins its radio to the swarm's stored `sw_chan`, set
at join time — this path is implemented and shipped, but has **not yet
been hardware-verified**; both bench lamps used for verification were
associated to the same home AP. Verifying the AP-less path is tracked as
a follow-up (PlanV3 Phase V2.5).

**Other caveats.** Multi-hop relay (hop 0 → 1) is implemented and
shipped, but hardware verification so far covers only a 2-lamp bench —
single-hop, no actual relay traversal exercised; a 3+-lamp multi-hop test
is a follow-up. Broadcast delivery is best-effort — ESP-NOW has no
application-layer ack/retry — but because every packet is a full state
snapshot rather than a delta, a dropped packet self-heals on the origin's
next change.

See [`docs/protocol.md`](docs/protocol.md#swarm-mode-esp-now-plsw-v1) for
the byte-precise packet format and the `/api/swarm*` wire contract.

### Multi-size pixels (pixel grouping)

A pixel group combines `pixelGroupW × pixelGroupH` physical LEDs into one logical pixel. A 16×16 matrix with group `2×2` renders as an 8×8 logical grid — `shade()` runs once per logical pixel (64 of them) and each result is tiled onto a 2×2 physical block.

Configure it at `/js → Guardrails → Pixel group` or `POST /api/limits`. `GET /api` reports both `physicalW/H` and `logicalW/H` alongside `pixelGroupW/H` so clients can size their frames correctly.

### Matrix Rotation

For matrix lamp types, both `/stream` and `/compose` show a **Rotate** dropdown letting you mount the physical matrix in any orientation while keeping content displayed upright.

**Options:** 0° (no rotation), 90° clockwise, 180°, 270° (90° counter-clockwise).

**Behavior:**
- Rotation applies to the **LED output only** — the onscreen grid preview always shows the source image in its logical orientation
- Setting is saved in browser localStorage (`rotation` key), shared between `/stream` and `/compose`
- Updates live — changing rotation during streaming immediately re-renders the current frame
- Works with both GIF frames (`/stream`) and AI-generated patterns (`/compose`)
- The dropdown is hidden for non-matrix lamp types (strips, rings, etc.)

Useful when the matrix is mounted sideways or upside-down without having to physically reorient the hardware or pre-rotate source images.

### MQTT

PlaiiinLightOS includes an MQTT client for integration with home automation systems (Home Assistant, Node-RED, etc.).

**Setup:**
1. Open `http://<device-ip>/mqtt`
2. Enable the toggle, enter your MQTT broker host and port (default 1883)
3. Click Save & Reboot

**Topics** (based on node name, e.g. `PlaiiinLight-01`):

| Direction | Topic | Payload |
|---|---|---|
| IN | `plaiiinlight/<node>/power/set` | `0` (off) or `1` (on) |
| OUT | `plaiiinlight/<node>/power/get` | `0` or `1` (retained) |
| IN | `plaiiinlight/<node>/color/set` | HSV: `h,s,v` (h: 0-360, s: 0-100, v: 0-100) |
| OUT | `plaiiinlight/<node>/color/get` | Current HSV `h,s,v` (retained) |
| IN | `plaiiinlight/<node>/brightness/set` | `0`-`255` |
| OUT | `plaiiinlight/<node>/brightness/get` | Current brightness (retained) |
| OUT | `plaiiinlight/<node>/status` | LWT: `online` / `offline` (retained) |

**HSV color format**: `hue,saturation,value` comma-separated integers.
- **Hue**: 0-360 (0=red, 120=green, 240=blue)
- **Saturation**: 0-100 (0=gray, 100=pure color)
- **Value/Brightness**: 0-100 (0=black, 100=full)

**Examples:**
```bash
# Turn on
mosquitto_pub -h broker -t 'plaiiinlight/PlaiiinLight-01/power/set' -m '1'

# Set pure red
mosquitto_pub -h broker -t 'plaiiinlight/PlaiiinLight-01/color/set' -m '0,100,100'

# Set dim cyan
mosquitto_pub -h broker -t 'plaiiinlight/PlaiiinLight-01/color/set' -m '180,100,30'

# Set brightness to 50%
mosquitto_pub -h broker -t 'plaiiinlight/PlaiiinLight-01/brightness/set' -m '128'
```

State is published on connect and after each change. The Last Will and Testament (LWT) message automatically sets status to `offline` if the device disconnects unexpectedly.

### Web UI Pages
| Page | Path | Description |
|---|---|---|
| Home | `/` | Redirects to `/control` (or `/network` in AP/provisioning mode) |
| Control | `/control` | Power on/off, color presets, color picker, brightness slider, script tunes |
| Stream | `/stream` | Drag & drop GIF/image streaming, matrix rotation |
| Compose | `/compose` | AI-powered LED pattern generation with chat, parameter tunes, brightness, matrix rotation, Save-to-device |
| Scripts | `/js` | List/edit/save/play/upload/delete local JS scripts, per-script tunes, guardrail knobs (max brightness, max current, pixel group) |
| Config | `/config` | Device settings, LED strip config, AI API key |
| Network | `/network` | WiFi SSID/password (captive portal redirects here in AP mode) |
| MQTT | `/mqtt` | MQTT broker config, enable/disable, topic reference |
| OTA | `/ota` | Wireless firmware update with progress bar and version info |

All pages share a consistent theme with day/night mode toggle (persisted in browser).

### HTTP API
| Endpoint | Method | Description |
|---|---|---|
| `/api` | GET | Device info (vendor, apiVersion, LED count, type, form, physicalW/H, logicalW/H, pixelGroupW/H) |
| `/api/power` | POST | `{"on": true/false}` — Turn LEDs on/off |
| `/api/color` | POST | `{"colors": [[r,g,b], ...]}` — Set LED colors (values clamped 0–255) |
| `/api/state` | GET | `{on, color:[r,g,b], mode, brightness}` — Current lamp state |
| `/api/brightness` | GET | `{"brightness": 255}` — Get current brightness |
| `/api/brightness` | POST | `{"brightness": 128}` — Set brightness (0-255, saved to NVS) |
| `/api/limits` | GET | `{maxBrightness, maxCurrentMa, pixelGroupW, pixelGroupH}` |
| `/api/limits` | POST | Any subset of the four fields — persisted to NVS |
| `/api/mode` | POST | `{"mode": "stream/api/js/frame"}` — Switch lamp mode |
| `/api/frame` | POST | Raw RGB body, `logicalW*logicalH*3` bytes (creator+) — save + display a drawn frame |
| `/api/frame` | GET | Raw RGB body + `X-Frame-W`/`X-Frame-H` headers (user+) — read back the stored frame |
| `/api/js` | GET | `{scripts:[...], playing:<name>\|null}` |
| `/api/js/{name}` | GET | Raw JS (`shade()`) source |
| `/api/js/{name}` | PUT | Save script (compiled to bytecode before write; errors surface here) |
| `/api/js/{name}` | DELETE | Remove a saved script |
| `/api/js/{name}/params` | GET | `{items:[{name,min,max,value,type,description}]}` — declared `@param` tunes |
| `/api/js/{name}/params` | PUT | `{name:value, …}` — update tune values |
| `/api/play` | POST | `{"file":"<name>","fps":<n>}` — Load + run script at FPS |
| `/api/play/next` `/api/play/prev` | POST | Cycle to the next/previous saved script |
| `/api/stop` | POST | Stop JS playback |
| `/api/pair` | GET | `{"mode":"paired"\|"unpaired"}` (no auth) |
| `/api/pair` | POST | Claim the lamp (unpaired) or rotate the token (paired) → `{mode,token}` |
| `/api/pair` | DELETE | Release ownership (Bearer token required) |
| `/api/reset` | POST | `{"scope":"wifi"\|"full"}` — factory reset (Bearer token required) |
| `/api/ai/key` | GET/PUT/DELETE | AI key presence (`{hasKey,len}`) / set / clear (raw key never returned) |
| `/api/form-prompt` | GET/PUT/DELETE | Per-lamp form descriptor injected into AI compose prompts |
| `/api/swarm` | GET | `{member, id, enabled, channel}` — swarm status (admin; key never returned) |
| `/api/swarm` | POST | `{"id":"<16hex>","key":"<64hex>","channel":N}` — join a swarm (admin, implies enable) |
| `/api/swarm` | DELETE | Leave the swarm (admin) |
| `/api/swarm/enable` | POST | `{"enabled":bool}` — toggle without losing membership (admin) |
| `/api/swarm/stats` | GET | Radio + protocol counters — tx/rx/drops/relayed/applied (admin) |
| `/api/swarm/ping` | POST | Debug: broadcast a plaintext test packet (admin) |
| `/api/ota` | POST | Upload firmware binary (application/octet-stream; cross-form → 409) |
| `/api/ota/info` | GET | Current firmware version, build date, partition, form, ESP-IDF version |
| `/config` | POST | Save device/LED config + reboot |
| `/network` | POST | Save WiFi config + reboot |

### WebSocket Streaming

Connect to `ws://<ip>/ws` for continuous LED color streaming. LEDs auto-enable on first color frame (no flicker).

**Binary protocol** (each frame):
| Byte(s) | Description |
|---|---|
| 0 | Command: `0x01` color frame, `0x02` power, `0x03` clear |
| 1-2 | (0x01 only) LED count, big-endian uint16 |
| 3+ | (0x01 only) RGB data, 3 bytes per LED (R, G, B) |

Power command: `[0x02, 0x00]` = off, `[0x02, 0x01]` = on
Clear command: `[0x03]` = all LEDs off

### Security & Pairing

**Onboarding.** A fresh lamp (never claimed) comes up reachable two ways at once:

- **WiFi** — an open provisioning SoftAP `PlaiiinLight-XXXX`; connect and enter your home WiFi at `/network`, and the lamp reboots onto your network.
- **BLE** — the lamp advertises over Bluetooth (default lifecycle policy `auto`); a client app can claim it and hand over WiFi credentials over the encrypted link.

These are **not** a mutually-exclusive mode you pick — both transports are live until the lamp is claimed. What *is* sticky is the **provisioned** flag (NVS `provisioned`): once a lamp has been claimed even once, it will **never re-open the open provisioning AP** again — reopening an unauthenticated captive portal would hand full control to any passer-by. A lamp whose owner later *releases* it stays BLE-only and re-claimable over BLE. Only a **factory reset** clears the provisioned flag and returns the lamp to fresh AP + BLE onboarding.

**Ownership — paired vs unpaired.** The **default is `unpaired`**: anyone on the local WiFi, and anyone in BLE range, has full (admin) control, exactly like earlier firmware. *Claiming* the lamp moves it to **`paired`** and mints a per-device 32-byte token (NVS `pair_token`, base64-url, 43 chars); from then on control requires that token.

| | Unpaired | Paired (ownership taken) |
|---|---|---|
| **WiFi mode** | On your network, **no owner** — anyone on the WiFi controls it | Control requires `Authorization: Bearer <token>` |
| **BLE mode** | **Anyone in BLE range can claim it** (first-claim-wins) and become the owner | Only the bonded owner controls it; the token is readable over the encrypted link |

Per-surface enforcement:

| Surface | Unpaired | Paired |
|---|---|---|
| **HTTP `/api/*`** (color, power, mode, scripts, OTA, reset, ai/key) | Open — anyone on WiFi | `Authorization: Bearer <token>`; wrong/missing → `401`. |
| **HTTP portal pages** (`/control`, `/config`, `/network`, …) | Open | Same Bearer token, **except** in AP mode (provisioning) — otherwise a WiFi-reset lamp couldn't be recovered. |
| **`POST /api/pair`** | Open — any client may bootstrap and becomes the owner | `POST` requires the *current* token (rotates it); `DELETE` unpairs (token required); `GET` always returns mode info. |
| **WebSocket `/ws`** (pixel streaming) | Open | Token in query: `ws://lamp/ws?token=<token>`, validated at the upgrade handshake. |
| **Device-served portal pages** | HTML loads; JS calls `/api/*` directly | Page is server-rendered with `<meta name="plk-token">` (only when the request was authenticated) plus an inline `auth.js` that adds the header to `fetch`. First-time browsers bootstrap from `?t=<token>` — see the "Pair this browser" QR in the macOS app. |
| **AI key on `/compose`** | Server-rendered into `<meta name="plk-aikey">` on the page request. `GET /api/ai/key` returns `{hasKey, len}` only — never the raw key. | Same, but the page is only served to authenticated requests. |
| **BLE GATT** (`device_info`, `wifi_*`, `power`, `color`, `mode`, `current_script`, `play_*`, `pair_claim`, `pair_unpair`, `pair_token`) | Reads/writes open, **except** the claim/unpair/token chars | Writes require `WRITE_ENC` and sensitive reads `READ_ENC` → an encrypted, Just-Works-bonded link (macOS shows a pairing prompt on first connect). |
| **BLE `pair_token` characteristic** | `READ_ENC` | Bonded owner can read it to learn the HTTP token without re-typing. |

**Factory reset.**

| Reset | Trigger | Clears | Keeps |
|---|---|---|---|
| **WiFi** | 10 s long-press (green), or `POST /api/reset {scope:"wifi"}` | WiFi creds, `pair_token` + `pair_mode`, share keys, `provisioned` flag | Hardware config, scripts, AI key |
| **Full** | 15 s long-press (blue), or `POST /api/reset {scope:"full"}` | All of the above **plus** selected script, lamp mode, base/last color, on/off state, AI key, wormhole config, swarm membership/config (`sw_id`/`sw_key`/`sw_chan`/`sw_on`/`sw_seq`) | Hardware config (form, pins, LED count), brightness |

Both reset paths release pairing and clear the `provisioned` flag, returning the lamp to fresh AP + BLE onboarding, claimable by whoever onboards it next. (Earlier firmware kept pairing across a WiFi reset; it no longer does.) The HTTP `POST /api/reset` form requires the Bearer token; the long-press is the physical recovery path.

**Threat model.** Pairing closes the gaps that matter once "everyone on the WiFi is trusted" stops holding — guests on the same SSID, BLE-range neighbours, and accidental discovery from other apps. It is **not** designed against a determined attacker on your LAN: there is **no TLS**, so the bearer token is sent in cleartext over HTTP and can be sniffed on an open network, and BLE Just-Works bonding has no MITM protection.

**Bootstrapping a paired lamp from a browser:** the macOS app's "Show pair-browser QR…" item generates `http://<lamp-ip>/?t=<token>` as a QR. Scanning it on a phone or another laptop opens the device portal; the inlined `auth.js` reads `?t=`, persists it in `localStorage`, and strips the parameter from the URL. From that point the browser is treated as a paired client until you clear its storage.

### Factory reset over power plug

Lamps with no button and no network can be recovered by power-cycling:
five consecutive fast power-cycles (each boot cut within 10 s) trigger the
same full factory reset as the 15 s button hold — the lamp double-flashes
blue, wipes WiFi credentials, pairing, share keys, mode/colors and the AI
key, and reboots into fresh onboarding. Hardware identity (form, type,
pins, LED count) and the durable recovery key are kept.

Only true power-on resets count (`ESP_RST_POWERON`): software reboots,
panics, watchdog resets and brownouts never advance the counter, so crash
loops and mains flicker cannot wipe a lamp. Any boot that stays powered
for 10 s ends the sequence.

### Technical Notes

- **LED Driver**: Uses SPI+DMA (not RMT) for flicker-free operation with WiFi active. DMA transfers run independently of the CPU, immune to WiFi interrupt interference.
- **WiFi**: Power save is disabled (`WIFI_PS_NONE`) to reduce interrupt jitter during LED updates.
- **Brightness**: Applied as a scale factor before writing to the strip. The original color values are preserved — changing brightness doesn't lose color precision.
- **Last Color**: The most recent solid color is saved to NVS and restored on power-on/reboot.
- **Matrix Mapping**: For matrix lamp types, serpentine row ordering is used (odd rows are reversed) to match common LED matrix wiring.

### Persistence
| Data | Storage | Survives USB reflash | Survives OTA |
|---|---|---|---|
| WiFi credentials | NVS | If you skip the `nvs` partition | Yes |
| LED pin, count, type | NVS | Same as above | Yes |
| Node name, vendor | NVS | Same as above | Yes |
| Brightness | NVS | Same as above | Yes |
| Max brightness / max current / pixel group | NVS | Same as above | Yes |
| Last color | NVS | Same as above | Yes |
| Saved JS scripts | SPIFFS `storage` partition | Yes (unless `storage` is rewritten) | Yes |
| Drawn frame (frame mode) | SPIFFS `storage` partition (`frame.bin`) | Yes (unless `storage` is rewritten) | Yes |
| AI API key | NVS (`ai_api_key`) — wiped by `factory_reset_full`. Pre-1.5 browser-localStorage entries are migrated on first portal load. | Same as above | Yes |
| AI provider / model / base URL | Browser localStorage (per browser) | N/A | N/A |
| Pairing token + mode | NVS (`pair_token`, `pair_mode`, `provisioned`) — wiped by **both** the WiFi and full factory reset. | Same as above | Yes |
| Swarm config (id/key/channel/enabled/seq) | NVS (`sw_id`, `sw_key`, `sw_chan`, `sw_on`, `sw_seq`) — wiped by `factory_reset_full` only, not the WiFi reset. | Same as above | Yes |

### Partition table

Flash layout (4 MB):

| Partition | Type | SubType | Offset | Size |
|---|---|---|---|---|
| `nvs` | data | nvs | 0x9000 | 24 KB |
| `otadata` | data | ota | 0xF000 | 8 KB |
| `phy_init` | data | phy | 0x11000 | 4 KB |
| `ota_0` | app | ota_0 | 0x20000 | 1.8 MB |
| `ota_1` | app | ota_1 | 0x1F0000 | 1.8 MB |
| `storage` | data | spiffs | 0x3C0000 | 256 KB |

> OTA updates only replace an app partition. Adding the `storage` partition (introduced with local JS support) requires a **one-time full USB flash** (`build/dist/plaiiinlight_os-<ver>-<form>-flash.bin`, e.g. via `profile-burn --full`) to rewrite the partition table. Until then, `/api/js` returns empty and the feature is a no-op.

### Error Indicators
- **Slow red blink**: No WiFi connection (retrying)
- **Fast red-yellow flash**: Configuration error
- **Slow blue pulse**: AP mode (waiting for WiFi setup)

## Build your own client

The wire contract between client and lamp is the canonical
[`docs/protocol.md`](docs/protocol.md), with companion specs for
[role-based sharing](docs/sharing-api.md),
[wormhole render modes](docs/wormhole-api.md), and
[BLE client-to-client share](docs/ble-share.md). Anything not documented
there is implementation detail — not a stable interface.

Third-party clients (CLI, Home Assistant integration, alternative apps,
language SDKs, …) are explicitly welcome. They target the protocol; the
firmware does not care which client speaks to it.

## Licensing

Everything in this repository is **Apache License, Version 2.0** — see
[`LICENSE`](LICENSE). Audit it, fork it, run it on your own hardware,
ship products that include it; the only requirement is the standard
Apache 2.0 attribution + notice retention.
