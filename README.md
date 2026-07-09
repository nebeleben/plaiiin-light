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
[latest release](https://github.com/nebeleben/plaiiin-light/releases/latest),
or flash straight from the browser (Chrome/Edge) using the release's
ESP Web Tools manifest.

Building from source is three commands:

```bash
git clone https://github.com/nebeleben/plaiiin-light.git
cd plaiiin-light
./scripts/setup.sh                    # installs ESP-IDF v5.3.2 + tools
./scripts/build.sh --form display     # tower | display | cube | rocket | wormhole | strip
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
- **Full web portal** — control, compose, stream, scripts, config,
  network, MQTT, and OTA pages served straight from the lamp. Day/night
  theme included.
- **GIF & image streaming** — drag a GIF onto `/stream` and it plays on
  the matrix, serpentine mapping and rotation handled for you.
- **WebSocket pixel streaming** — a compact binary protocol for real-time
  frames from any client you write.
- **HTTP API** — power, color, brightness, modes, scripts, limits, OTA:
  everything the portal does, a `curl` away.
- **MQTT** — power, color, brightness, mode, and effect next/prev topics
  for Home Assistant, Node-RED, and friends.
- **BLE onboarding & control** — claim a fresh lamp and hand it WiFi
  credentials over an encrypted Bluetooth link, no captive-portal hopping.
- **Ownership & sharing** — lamps are open until claimed; claiming mints
  a bearer token, and owners can share role-scoped keys (`user` /
  `creator` / `admin`) — revocable any time.
- **OTA updates** — dual-slot, form-checked (a `tower` binary can't brick
  a `display`), with config and scripts preserved.
- **Guardrails** — hard caps on brightness and estimated current draw
  protect your hardware on every frame path.
- **Pixel grouping & rotation** — gang physical LEDs into logical pixels
  and mount the matrix in any orientation.
- **Six lamp forms** — `tower`, `display`, `cube`, `rocket`, `wormhole`,
  `strip` — each with hand-tuned built-in effects.
- **Multi-chipset** — WS2812/NeoPixel, SK6812 (RGB + RGBW), SK9822/APA102;
  SPI+DMA driver for flicker-free rendering with WiFi active.
- **Native client apps** — macOS, iOS, Android, Windows, and Linux apps
  at [plaiiin-light.com](https://plaiiin-light.com), all speaking the
  open protocol in `docs/`.

## Specs

Want the deep end? [`SPECS.md`](SPECS.md) is the full technical
reference: build system and per-form binaries, the complete HTTP API and
WebSocket protocol, the `shade()` script contract, MQTT topics, the
security and pairing model, persistence, partition table, and error
indicators. Client authors should start with the canonical wire contract
in [`docs/protocol.md`](docs/protocol.md) — third-party clients are
explicitly welcome.

## Latest release

**Current firmware: v1.9.10** — grab it from the
[releases page](https://github.com/nebeleben/plaiiin-light/releases/latest).
Every release ships, per form: a `flash.bin` (USB first-flash), an
`app.bin` (OTA payload), an `effects.bin` (per-form effects image), an
ESP Web Tools manifest for browser flashing, and `SHA256SUMS`.

What's new in the 1.9 line:

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
