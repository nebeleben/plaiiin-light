# PlaiiinLightOS

Open firmware for ESP32 microcontrollers driving WS2812 addressable LED
strips and matrices. AI-aware, scriptable, OTA-updatable, with HTTP,
WebSocket, and BLE control surfaces.

### What's in this repository

- `components/`, `main/`, `portal/` — the firmware source.
- `scripts/` — build, flash, and profile-burn tooling.
- `profiles/` — per-device build profiles (tower, wall, wormhole, …).
- `effects/` — form-specific JavaScript effects, flashed alongside the
  firmware on a `--full` profile burn.
- `docs/` — the wire contract every client targets (HTTP, WebSocket,
  BLE), plus role-based access, wormhole render-mode, and BLE-share specs.
- `LICENSE` — Apache 2.0 for everything in this repository.

> **Note for external contributors.** This repository is a public mirror
> of an internal source-of-truth and is **force-pushed** on every upstream
> change. PRs opened here cannot merge — we read each one, re-land it
> upstream with attribution to the original author, and your change ships
> on the next mirror push. Issues are the primary inbound channel —
> please file them here.

### Prerequisites
- ESP-IDF v5.3+ (uses Python 3.12 env — Python 3.14 breaks `idf_tools`; prefix builds with `IDF_PYTHON_ENV_PATH=$HOME/.espressif/python_env/idf5.3_py3.12_env`)

### Setup
```bash
./scripts/setup.sh    # Installs ESP-IDF v5.3.2 and tools
```

### Build profiles

`scripts/build.sh <profile>` composes `SDKCONFIG_DEFAULTS` from `sdkconfig.defaults` plus `profiles/<profile>/sdkconfig.defaults`, builds into `build/`, and drops two artifacts into `build/dist/`:

- `<profile>-app.bin` — OTA payload (app only, ~1 MB)
- `<profile>-flash.bin` — merged full-flash image (bootloader + partitions + otadata + app, for USB first-flash at offset 0)

Available profiles:

| Profile | Target | LEDs | GPIO | lamp type / form |
|---|---|---|---|---|
| `tower` | 8×8 matrix table lamp | 64 | 26 | `matrix8x8` / `tower` |
| `wall` | 16×16 matrix display | 256 | 26 | `matrix16x16` / `display` |

```bash
./scripts/build.sh wall
# → build/dist/wall-app.bin   build/dist/wall-flash.bin
```

### Configure
```bash
source ~/esp/esp-idf/export.sh
idf.py menuconfig     # Set LED pin, count, WiFi, node name
```

Key settings under "PlaiiinLightOS Configuration":
- **LED Strip**: GPIO pin, LED count, LED type (WS2812), lamp type, lamp form
- **WiFi**: SoftAP SSID prefix, HTTP port
- **Node**: Name, vendor, API version

All settings can also be changed at runtime via the web UI at `/config` (stored in NVS, survives reboots and OTA updates).

### Build & Flash (USB) - First Time

The initial flash must be done via USB. After that, use OTA for wireless updates.

```bash
./scripts/build.sh
./scripts/flash.sh /dev/ttyUSB0    # or /dev/cu.usbserial-*
```

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
3. Drop `build/plaiiinlight_os.bin` onto the upload zone (or click to browse)
4. Upload progress bar shows transfer status
5. Device flashes to the inactive OTA partition, reboots, and the page auto-reloads

**Via command line:**
```bash
# Build
./scripts/build.sh

# Upload (replace IP with your device's address)
curl -X POST http://<device-ip>/api/ota \
  -H "Content-Type: application/octet-stream" \
  --data-binary @build/plaiiinlight_os.bin
```

The partition table uses dual OTA slots (`ota_0` + `ota_1`), alternating on each update. NVS config (WiFi, LED settings, brightness, last color) is preserved across all OTA updates.

### AI Compose

The `/compose` page lets you describe LED patterns in natural language. An AI generates the pattern as either pixel data or a JavaScript animation function, which is rendered on the grid preview and streamed to the physical LEDs via WebSocket.

**Setup:**
1. Get an API key from [console.anthropic.com](https://console.anthropic.com) (Claude) or [platform.openai.com](https://platform.openai.com) (OpenAI)
2. Go to `http://<device-ip>/config`, scroll to **AI Integration**
3. Select your provider, paste your API key, and set the model:
    - Claude: `claude-haiku-4-5-20251001` (fast/cheap), `claude-sonnet-4-6`, `claude-opus-4-6`
    - OpenAI: `gpt-4o-mini` (fast/cheap), `gpt-4o`
4. The API key is stored in your browser's localStorage (never sent to the ESP32)

**Usage:**
1. Open `http://<device-ip>/compose`
2. Type a prompt: *"draw a rainbow"*, *"create a fire animation"*, *"pixel art of a heart"*
3. The AI generates the pattern and displays it on the grid preview
4. Click **Play** to stream it to the LEDs (auto-connects WebSocket)
5. Refine iteratively: *"make it scroll faster"*, *"add blue sparkles"*, *"change background to black"*
6. Adjust **FPS** slider for animation speed, **Brightness** slider for intensity
7. Click **Stop** to turn off the LEDs

The AI supports two output formats automatically:
- **Pixel data** (JSON) — for static images and short frame sequences
- **Generator code** (JavaScript) — for complex animations, gradients, and math patterns. The AI writes a `render(frame, width, height)` function that runs in the browser to generate up to 300 frames.

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

PlaiiinLightOS ships with an embedded ES5 JS engine (mJS, `components/mjs`). User scripts define a `render(frame, w, h)` that returns a `[r,g,b][]` array of `w * h` pixels per frame. The device runs the script on a dedicated FreeRTOS task at the configured FPS and pushes each frame through `led_control_set_logical`, so pixel-grouping + guardrails apply automatically.

Scripts are stored on a dedicated 256 KB SPIFFS `storage` partition (see partition table below). Manage them at `http://<device-ip>/js` — list, edit, save, play/stop, upload, delete. The `/compose` page now has a **Save to device** button that wraps the current AI-generated JS into a `render(...)` function and PUTs it as a named script.

Script API (same shape on device + in `/compose`):

```js
function render(frame, w, h) {
  var out = [];
  for (var y = 0; y < h; y++)
    for (var x = 0; x < w; x++)
      out.push([r, g, b]);  // 0..255 each
  return out;  // length === w*h
}
```

On save, the server eval's the script once with `frame=0` to surface parse/runtime errors before it's written to flash.

### Multi-size pixels (pixel grouping)

A pixel group combines `pixelGroupW × pixelGroupH` physical LEDs into one logical pixel. A 16×16 matrix with group `2×2` renders as an 8×8 logical grid — `render()` returns 64 pixels and each one is tiled onto a 2×2 physical block.

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
| Control | `/control` | Power on/off, color presets, color picker, brightness slider |
| Stream | `/stream` | Drag & drop GIF/image streaming, matrix rotation |
| Compose | `/compose` | AI-powered LED pattern generation with chat, brightness, FPS, matrix rotation, Save-to-device |
| Scripts | `/js` | List/edit/save/play/upload/delete local JS scripts + guardrail knobs (max brightness, max current, pixel group) |
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
| `/api/mode` | POST | `{"mode": "stream/api"}` — Switch between API and WebSocket mode |
| `/api/js` | GET | `{scripts:[...], playing:<name>\|null}` |
| `/api/js/{name}` | GET | Raw JS source |
| `/api/js/{name}` | PUT | Save JS source (validated + eval'd once before write) |
| `/api/js/{name}` | DELETE | Remove a saved script |
| `/api/play` | POST | `{"file":"<name>","fps":<n>}` — Load + run script at FPS |
| `/api/stop` | POST | Stop JS playback |
| `/api/ota` | POST | Upload firmware binary (application/octet-stream) |
| `/api/ota/info` | GET | Current firmware version, build date, partition, ESP-IDF version |
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

PlaiiinLightOS has two operating modes for access control. The **default is `unpaired`** — anyone on the local WiFi (and anyone in BLE range) can control the lamp, exactly like earlier firmware. Opt into `paired` to lock control down to clients holding a per-device 32-byte token.

The token lives in NVS (`pair_token` key, base64-url, 43 chars), is generated on first `POST /api/pair`, and is wiped by `factory_reset_full`. Each transport proves possession in its own way, summarised below.

| Surface | Unpaired | Paired |
|---|---|---|
| **HTTP `/api/*`** (color, power, mode, scripts, OTA, reset, ai/key) | Open — anyone on WiFi | Requires `Authorization: Bearer <token>`. Wrong/missing → `401 Unauthorized`. |
| **HTTP captive portal** (`/`, `/config`, `/network`) | Open | Requires the same Bearer token, **except** when the device is in AP mode (provisioning) — recovery would otherwise be impossible after a WiFi reset. |
| **HTTP `/api/pair`** | `POST` is open (any client may bootstrap) | `POST` requires the *current* token (rotates it); `DELETE` requires the token (unpairs); `GET` always returns mode info. |
| **WebSocket `/ws`** (pixel streaming) | Open | Token in query: `ws://lamp/ws?token=<token>`. Validated at the upgrade handshake. |
| **Device-served portal pages** (`/compose`, `/control`, etc.) | HTML loads; JS calls `/api/*` directly | HTML loads; the page is server-rendered with `<meta name="plk-token">` (only when the request itself was authenticated) and a tiny inline `auth.js` that wraps `fetch` to add the header. First-time browsers bootstrap from `?t=<token>` in the URL — see "Pair this browser" QR in the macOS app. |
| **AI key on `/compose`** | Server-rendered into `<meta name="plk-aikey">` (page request only). `GET /api/ai/key` returns `{hasKey, len}` only — never the raw key. | Same — but the page is only served to authenticated requests, so the key reaches the page only via a paired session. |
| **BLE GATT** (`device_info`, `wifi_*`, `power`, `color`, `mode`, `current_script`, `play_*`, `script_upload_*`) | Open — any peer can read/write | Writes require `BLE_GATT_CHR_F_WRITE_ENC` → forces an encrypted/bonded link. macOS surfaces a Just-Works pairing prompt on first connect. Sensitive reads (`current_script`, new `pair_token` characteristic) require `READ_ENC`. |
| **BLE `pair_token` characteristic** | Not advertised | Present, `READ_ENC` only — bonded peers can read it to learn the HTTP token without re-typing it. |
| **`factory_reset_wifi`** (10s long-press, or `POST /api/reset {scope:"wifi"}`) | n/a | Requires Bearer token; clears WiFi creds only — pairing is **kept** so a re-onboarded lamp stays under the same client's control. |
| **`factory_reset_full`** (15s long-press, or `POST /api/reset {scope:"full"}`) | n/a | Requires Bearer token; also clears `pair_token` + flips back to `unpaired`. |

**Threat model.** The unpaired default matches the trust posture of a hobby device on a private home network (anyone already on the WiFi is "trusted"). Pairing closes the gaps that matter when that assumption breaks — guests on the same SSID, BLE-range neighbors, and accidental discovery from other apps. It is **not** designed against a determined attacker on your LAN: there is no TLS, the bearer token is sent in cleartext over HTTP, and BLE Just-Works bonding has no MITM protection. A future LE Secure Connections + passkey upgrade is planned; the LE passkey will be shown on the LED panel for matrix lamps and via a one-shot OOB code over BLE for non-matrix forms (towers, wormholes, etc.) where there's no display to read it from.

**Bootstrapping a paired lamp from a browser:** the macOS app's "Show pair-browser QR…" item generates `http://<lamp-ip>/?t=<token>` as a QR. Scanning it on a phone or another laptop opens the device portal, the inlined `auth.js` reads `?t=`, persists it in `localStorage`, and strips the parameter from the URL. From that point the browser is treated as a paired client until you clear its storage.

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
| AI API key | NVS (`ai_api_key`) — wiped by `factory_reset_full`. Pre-1.5 browser-localStorage entries are migrated on first portal load. | Same as above | Yes |
| AI provider / model / base URL | Browser localStorage (per browser) | N/A | N/A |
| Pairing token + mode | NVS (`pair_token`, `pair_mode`) — wiped by `factory_reset_full`; kept by `factory_reset_wifi`. | Same as above | Yes |

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

> OTA updates only replace an app partition. Adding the `storage` partition (introduced with local JS support) requires a **one-time full USB flash** (`build/dist/<profile>-flash.bin`) to rewrite the partition table. Until then, `/api/js` returns empty and the feature is a no-op.

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

For partnership inquiries, contact plaiiin.com.
