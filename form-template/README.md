# Form prompt templates

One file per lamp form. Each holds the **physical-form descriptor** that the
apps and the on-device `/compose` page append to the AI system prompt so
generated `shade()` scripts are aware of the lamp's real physical arrangement
(cylinder seam, cube faces, rings, segments). See Phase 26.

These are **not embedded in firmware**. `scripts/profile-burn.sh --full`
flashes the device's own form file into the SPIFFS `storage` partition as
`_form_template.txt` (alongside any `effects/<FORM>/` byForm effects). The
firmware reads it at runtime via `GET /api/form-prompt`; if the file is absent
(device never `--full`-burned) it falls back to a hardcoded template in
`main/form_prompt.c`. Reword a descriptor here and re-burn — no firmware
release needed.

## Placeholders

The firmware substitutes these tokens with live geometry:

| Token        | Value                                  |
|--------------|----------------------------------------|
| `{w}` `{h}`  | logical grid width / height            |
| `{wmax}` `{hmax}` | `w-1` / `h-1`                      |
| `{wh}`       | `w * h` (logical pixel count)          |
| `{count}`    | physical LED count                     |
| `{panel}`    | matrix panel side (e.g. 8 or 16)       |
| `{panelsq}`  | `panel * panel`                        |
| `{faces}`    | `count / panelsq` (cube)               |
| `{rings}`    | `count / 24` (wormhole)                |

`bare`/unknown forms have no file here — they use the firmware fallback.
