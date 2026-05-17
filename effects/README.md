# Built-in effects

Two kinds of built-in `shade()` effects ship with PlaiiinLightOS:

- **General** — `noop, fade, plasma, breath, heartbeat, shootingstar,
  particles`. They live in `lampos/portal/`, are `EMBED_TXTFILES`'d into the
  firmware, and `main.c` preinstalls any that are missing to every device on
  boot. Built-in on all devices regardless of form.

- **byForm** — form-specific effects, one subfolder per `CONFIG_PLAIIIN_FORM`
  value (`cube/ rocket/ tower/ display/ wormhole/`). These are **not** in the
  firmware. `scripts/profile-burn.sh --full` reads the device's FORM from its
  profile, builds a SPIFFS image of `effects/<FORM>/*.js`, and flashes it to
  the `storage` partition. On boot the firmware compiles every stored `.js`
  that lacks a `.bc` (the "6a-bis" pass in `main.c`).

## Adding a byForm effect

1. Drop a `shade()` script into `effects/<FORM>/<name>.js` (same contract as
   the general built-ins — see `lampos/components/plbc/compile.c`).
2. `--full`-burn devices of that form with `scripts/profile-burn.sh`.

`count.js` is a debug effect (cumulative index sweep for direction /
serpentine checking) and is intentionally present in every form folder.
