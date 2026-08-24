# PlaiiinLightOS

**Make any LED lamp think.** PlaiiinLightOS is open firmware for ESP32
microcontrollers driving addressable LED strips and matrices — AI-aware,
scriptable, OTA-updatable, with HTTP, WebSocket, BLE, and MQTT control
surfaces. Describe a pattern in plain words, let an AI write the effect,
and watch it light up.

Runs the whole [PlaiiinLight](https://plaiiin-light.com) lamp family —
towers, cubes, wall displays, rockets, wormholes, and plain strips — and
any WS2812 / SK6812 / SK9822-class LED hardware you wire up yourself.

## Getting started

The fastest way to a running lamp — no toolchain needed — is a prebuilt
binary: grab your form's `flash.bin` from the
[latest release](https://github.com/nebeleben/plaiiin-light/releases/latest)
(`<form>-flash.bin` for classic ESP32 boards, `<form>-esp32c3-flash.bin`
for the ESP32-C3),
or flash straight from the browser (Chrome/Edge) using the release's
ESP Web Tools manifest.

Building from source is three commands:

```bash
git clone https://github.com/nebeleben/plaiiin-light.git
cd plaiiin-light
./scripts/setup.sh                    # installs ESP-IDF v5.3.2 + tools
./scripts/build.sh --form display     # tower | display | cube | rocket | wormhole | strip
                                      # add --chip esp32c3 (or esp32s3) for those boards
```

That drops two artifacts into `build/dist/`: a merged `…-flash.bin` for
USB first-flash and an `…-app.bin` OTA payload for wireless updates.

Burn it to the device over USB — `profile-burn` erases the chip, writes
the firmware, burns the device's hardware identity (LED count, GPIO,
lamp type, form) from `profiles/`, and installs its per-form effects in
one shot:

```bash
./scripts/profile-burn.sh --full /dev/cu.usbserial-0001 display/<device>
```

First boot: the lamp opens a WiFi access point `PlaiiinLight-XXXX` —
connect, enter your home WiFi at `http://192.168.4.1/network`, and after
the reboot the lamp is on your network. Everything else happens in the
built-in web portal at `http://<device-ip>/` — and every update from
then on is wireless via `/ota`.

## Features

All the features you ever wanted from a lamp OS — and a few you didn't
know you did:

- **AI Compose** — describe an effect in natural language on `/compose`;
  the AI writes it, the browser previews it, one click saves it to the
  lamp. Bring your own key (Claude, GPT, or a local LLM) — it never
  touches the device.
- **Scriptable effects** — effects are tiny JavaScript `shade()` shaders,
  compiled to PLBC bytecode and played on-device by a purpose-built VM at
  your chosen FPS.
- **Script library & tunes** — list, edit, save, and play scripts on the
  lamp; every script exposes parameter knobs and switches you can tweak
  live.
- **Full web portal** — control, compose, stream, draw, scripts, config,
  network, MQTT, and OTA pages served straight from the lamp. Day/night
  theme included.
- **GIF & image streaming** — drag a GIF onto `/stream` and it plays on
  the matrix, serpentine mapping and rotation handled for you.
- **WebSocket pixel streaming** — a compact binary protocol for real-time
  frames from any client you write.
- **Draw / frame mode** — paint pixel art on a grid (or drop an image) in
  the app or the on-device `/draw` editor, push it once, and the lamp holds
  the still frame reboot-safe until you change it — no client needs to stay
  connected to keep it lit.
- **Saved images & carousel** — keep up to 30 drawings in the lamp's own
  image library (`/api/images`), browse them as thumbnails in the apps or
  `/draw`, and play them as a slideshow with the built-in `carousel`
  effect — dwell, crossfade, and shuffle are live-tunable knobs.
- **HTTP API** — power, color, brightness, modes, scripts, limits, OTA:
  everything the portal does, a `curl` away.
- **MQTT** — power, color, brightness, mode, and effect next/prev topics
  for Home Assistant, Node-RED, and friends.
- **Swarm mode** — group nearby lamps into a masterless ESP-NOW mesh:
  change one lamp's color, brightness, or effect and the whole group
  follows. HMAC-authenticated, de-duplicated multi-hop flood, and it works
  with no router at all — AP-less lamps included.
- **BLE onboarding & control** — claim a fresh lamp and hand it WiFi
  credentials over an encrypted Bluetooth link, no captive-portal hopping.
- **Ownership & sharing** — lamps are open until claimed; claiming mints
  a bearer token, and owners can share role-scoped keys (`user` /
  `creator` / `admin`) — revocable any time.
- **OTA updates** — dual-slot, form-checked (a `tower` binary can't brick
  a `display`), with config and scripts preserved.
- **Guardrails** — hard caps on brightness and estimated current draw
  protect your hardware on every frame path.
- **Plug reset** — no button, no network, no problem: five fast power
  cycles factory-reset the lamp (blue flash confirms), keeping its
  hardware identity and recovery key.
- **Pixel grouping & rotation** — gang physical LEDs into logical pixels
  and mount the matrix in any orientation.
- **Six lamp forms** — `tower`, `display`, `cube`, `rocket`, `wormhole`,
  `strip` — each with hand-tuned built-in effects.
- **Multi-chipset** — WS2812/NeoPixel, SK6812 (RGB + RGBW), SK9822/APA102;
  SPI+DMA driver for flicker-free rendering with WiFi active.
- **Native client apps** — macOS, iOS
  ([App Store](https://apps.apple.com/app/plaiiin-light/id6787092509)),
  Android, Windows, and Linux apps at
  [plaiiin-light.com](https://plaiiin-light.com), all speaking the
  open protocol in `docs/`.

## Supported Devices

### Microcontrollers

Any ESP32 development board with at least 4 MB flash works. Three SoC
targets are supported by the build system (`./scripts/build.sh --chip …`):

| Chip | Boards (examples) | Notes |
| --- | --- | --- |
| **ESP32** (classic) | ESP32 Mini / D1 Mini ESP32, ESP32 DevKit | Dual-core Xtensa @ 240 MHz — the fleet default; prebuilt `<form>-*.bin` release binaries |
| **ESP32-C3** | C3 SuperMini, C3 DevKit | Single-core RISC-V @ 160 MHz — prebuilt `<form>-esp32c3-*.bin` release binaries, or build with `--chip esp32c3` |
| **ESP32-S3** | S3 Mini, S3 DevKit | Dual-core Xtensa @ 240 MHz — build with `--chip esp32s3` |

Every lamp form builds for every chip; the C3 and S3 differ only in
bootloader offset and (C3) CPU clock, which the build script handles.

### LEDs

All common addressable strip and matrix chipsets, one- and two-wire:

| Chipset | Wiring | Notes |
| --- | --- | --- |
| **WS2812 / WS2812B** (NeoPixel) | 1 wire (data) | The default; unknown types fall back to it |
| **SK6812** | 1 wire (data) | RGB variant |
| **SK6812W / SK6812 RGBW** | 1 wire (data) | 4-channel RGBW, dedicated white LEDs driven natively |
| **SK9822 / APA102** (DotStar) | 2 wire (data + clock) | SPI+DMA driver — flicker-free even with WiFi busy |

The chipset, data/clock GPIOs, and LED count are per-device hardware
identity (burned via `profile-burn`, editable on `/config`) — no firmware
rebuild needed to switch strips.

## Specs

Want the deep end? [`SPECS.md`](SPECS.md) is the full technical
reference: build system and per-form binaries, the complete HTTP API and
WebSocket protocol, the `shade()` script contract, MQTT topics, the
security and pairing model, persistence, partition table, and error
indicators. Client authors should start with the canonical wire contract
in [`docs/protocol.md`](docs/protocol.md) — third-party clients are
explicitly welcome.

## Latest release

**Current firmware: v2.1.0** — grab it from the
[releases page](https://github.com/nebeleben/plaiiin-light/releases/latest).
Every release ships, per form and per chip (classic ESP32 and ESP32-C3,
the latter with an `-esp32c3` infix): a `flash.bin` (USB first-flash),
an `app.bin` (OTA payload), an `effects.bin` (per-form effects image),
an ESP Web Tools manifest for browser flashing, and `SHA256SUMS`.

What's new in the 2.x line:

- **v2.1.0** — saved draw images:
  - **Image library on the lamp** — save up to 30 drawn frames on the
    lamp itself (`/api/images`: list, save, show, delete); thumbnails
    and one-tap show in the apps and the on-device `/draw` page.
  - **Carousel effect** — a built-in effect that cycles the saved images
    as a slideshow, with tunable dwell, crossfade, and shuffle.
- **v2.0.1** — point release to exercise in-app firmware updates end to
  end; no on-device changes.
- **v2.0.0** — the big one:
  - **Swarm mode** — masterless ESP-NOW mesh; change one lamp and the
    group follows, HMAC-secured, works even with no router.
  - **Draw / frame mode** — push a still pixel-art frame (`POST /api/frame`)
    that survives reboot, plus an on-device `/draw` pixel editor.
  - **Plug factory reset** — five fast power cycles recover a buttonless /
    off-network lamp (a blue flash confirms), keeping its hardware identity
    and recovery key.
  - **In-app Direct OTA** — a form-aware `/api/ota/info` lets the macOS /
    iOS / Android apps update a lamp wirelessly, form- and chip-checked so
    the wrong image can't be applied.

Earlier — the 1.9 line:

- **v1.9.14** — smoother onboarding: the WiFi network scan works before
  the lamp has a connection, slow WiFi at boot no longer costs you
  mDNS/MQTT or the portal, and the panel and `/api/state` stay in
  agreement after setup.
- **v1.9.13** — prebuilt ESP32-C3 binaries for every lamp form now ship
  with each release, alongside the classic ESP32 set.
- **v1.9.12** — ESP32-C3 support lands: build any form with
  `--chip esp32c3`; `/api/ota/info` now reports the chip target so
  updaters can't cross-flash; new shrumLight (24-LED strip) profile.
- **v1.9.11** — web server no longer wedges when clients leave
  connections half-open: stale sockets are purged (LRU) so the portal
  and API stay reachable.
- **v1.9.10** — the factory-reset recovery key is now durable: it
  survives reset and unpair, so a lost lamp can always be recovered.
- **v1.9.9** — release-pipeline rebuild; no firmware changes.
- **v1.9.8** — recovery-key endpoints (`/api/reset-key`); effect
  next/prev no longer stalls on an uncompilable script; fire effect
  fixed on pixel-grouped displays.
- **v1.9.7** — MQTT learned mode switching (color/js) and effect
  next/prev topics; portal root now redirects to `/control`; knob and
  switch tunes on the Scripts and Compose pages.

## License

Everything in this repository is **Apache License, Version 2.0** — see
[`LICENSE`](LICENSE). Audit it, fork it, run it on your own hardware,
ship products that include it; the only requirement is the standard
Apache 2.0 attribution + notice retention.
