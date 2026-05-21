#!/usr/bin/env bash
# Manufacturing-time profile burn over USB. Writes only the NVS partition
# (no WiFi roundtrip, no full reflash) so a fresh chip boots with its
# hardware identity baked in. The runtime WiFi/BLE onboarding is a separate
# step the user does later via the captive portal or the BLE sheet.
#
#   Usage:
#     ./scripts/profile-burn.sh <port> <family>/<device>
#       — write only the NVS partition (~24 KB, ~2 s)
#
#     ./scripts/profile-burn.sh --full <port> <family>/<device>
#       — erase_flash + write_flash <flash.bin> + write NVS, for a brand-new
#         chip. Implies that scripts/build.sh has been run; uses the latest
#         build/dist/plaiiinlight_os-<ver>-flash.bin.
#       — also installs the byForm effects for this device's FORM: a SPIFFS
#         image of effects/<FORM>/*.js is flashed to the storage partition.
#         (Plain non-full burns leave storage — and any user scripts — alone.)
#
#     ./scripts/profile-burn.sh --baud <rate> ...    (default: 460800)
#
# Examples:
#     ./scripts/profile-burn.sh /dev/cu.usbserial-0001 tower/tower8v2
#     ./scripts/profile-burn.sh --full /dev/cu.usbserial-0001 tower/tower8v2
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

# --- Defaults -----------------------------------------------------------------
BAUD=460800
FULL=0

# Locate ESP-IDF tools — we don't require the user to source export.sh first.
PYTHON="${IDF_PYTHON:-/Users/$USER/.espressif/python_env/idf5.3_py3.12_env/bin/python}"
ESPTOOL="${IDF_PYTHON_DIR:-$(dirname "$PYTHON")}/esptool.py"
NVS_GEN="${IDF_PATH:-$HOME/esp/esp-idf}/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py"
SPIFFS_GEN="${IDF_PATH:-$HOME/esp/esp-idf}/components/spiffs/spiffsgen.py"

# Partition layout from lampos/partitions.csv. If you ever resize NVS, update.
NVS_OFFSET=0x9000
NVS_SIZE=0x6000
NVS_NAMESPACE=plaiiin_cfg

# --- Argument parsing ---------------------------------------------------------
while [ $# -gt 0 ]; do
    case "$1" in
        --full)        FULL=1; shift ;;
        --baud)        BAUD="$2"; shift 2 ;;
        -h|--help)     sed -n '2,22p' "$0"; exit 0 ;;
        --)            shift; break ;;
        -*)            echo "unknown flag: $1" >&2; exit 2 ;;
        *)             break ;;
    esac
done

PORT="${1:-}"
PROFILE="${2:-}"
if [ -z "$PORT" ] || [ -z "$PROFILE" ]; then
    echo "usage: $0 [--full] [--baud N] <port> <family>/<device>" >&2
    exit 2
fi

if [[ "$PROFILE" != */* ]]; then
    echo "profile must be <family>/<device>, e.g. tower/tower8v2" >&2; exit 2
fi
FAMILY="${PROFILE%%/*}"
DEVICE="${PROFILE##*/}"
DEFAULTS="$PROJECT_DIR/profiles/$FAMILY/$DEVICE.defaults"
[ -f "$DEFAULTS" ] || { echo "missing $DEFAULTS" >&2; exit 1; }

# Catch the copy-paste mistake where a new profile inherits its predecessor's
# NODE_NAME — the chip then advertises under the wrong name over BLE/mDNS and
# silently collides with the real device. We've hit this three times; refuse
# to burn rather than discover it after the fact.
file_node_name=$(awk -F= '$1=="CONFIG_PLAIIIN_NODE_NAME"{sub(/^"/,"",$2);sub(/"$/,"",$2);print $2;exit}' "$DEFAULTS")
if [ "$file_node_name" != "$DEVICE" ]; then
    echo "refusing to burn: $DEFAULTS sets NODE_NAME='$file_node_name' but filename is '$DEVICE'" >&2
    echo "fix CONFIG_PLAIIIN_NODE_NAME in the .defaults to match the filename, then retry." >&2
    exit 1
fi

[ -e "$PORT" ]    || { echo "no device at $PORT" >&2; exit 1; }
[ -x "$PYTHON" ]  || { echo "no python at $PYTHON (set IDF_PYTHON)" >&2; exit 1; }
[ -f "$ESPTOOL" ] || { echo "no esptool.py at $ESPTOOL" >&2; exit 1; }
[ -f "$NVS_GEN" ] || { echo "no nvs_partition_gen.py at $NVS_GEN (set IDF_PATH)" >&2; exit 1; }

# --- Schema: CONFIG_PLAIIIN_<KEY>  →  (nvs_key, encoding) --------------------
# The encoding must match what the firmware reads (config_store_get_str vs
# config_store_get_i32). All numeric fields go through i32 today; bools are
# stored as i32(0|1) too, so SERPENTINE=y normalises to 1.
#
# Phase 29 wormhole: WH_MODE/WH_RINGS are flat keys handled by this table; the
# per-ring physical config is carried as readable CONFIG_PLAIIIN_WH_RING<n>
# lines in the profile and assembled into the wh_phys JSON string below.
declare -a SCHEMA=(
    "NODE_NAME      node_name      string"
    "VENDOR_NAME    vendor_name    string"
    "LED_PIN        led_pin        i32"
    "LED_CLK_PIN    led_clk_pin    i32"
    "LED_COUNT      led_count      i32"
    "LED_TYPE       led_type       string"
    "LAMP_TYPE      lamp_type      string"
    "FORM           lamp_form      string"
    "PX_GROUP_W     px_group_w     i32"
    "PX_GROUP_H     px_group_h     i32"
    "ROTATION       rotation       i32"
    "ORIGIN         origin         i32"
    "SERPENTINE     serpentine     i32"
    "SERP_AXIS      serp_axis      i32"
    "BTN_PWR_PIN    btn_pwr_pin    i32"
    "BTN_NEXT_PIN   btn_next_pin   i32"
    "BTN_PREV_PIN   btn_prev_pin   i32"
    # Phase 29 wormhole render config — only present on wormhole profiles; for
    # every other profile `get` returns empty and the row is skipped.
    "WH_MODE        wh_mode        string"
    "WH_RINGS       wh_rings       i32"
    # Phase 33 on/off fade durations (ms). Optional — omit from a profile to
    # let the firmware fall back to its defaults (600 / 800). 0 = instant snap.
    "FADE_ON_MS     fade_on_ms     i32"
    "FADE_OFF_MS    fade_off_ms    i32"
    # Phase 33 AP-mode JS script name (script on SPIFFS played while the
    # lamp is in onboarding/captive-portal mode). Omit to keep the built-in
    # blue-pulse-on-first-3-LEDs fallback. Pair with a corresponding
    # effects/<FORM>/<NAME>.js that --full will flash into the byForm image.
    "AP_JS          ap_js          string"
    # Phase 35 — hardcoded-effect mask source for the FirePattern port.
    # Profile picks one of adaptations/fire/<name>.pattern (e.g. "fire8x8");
    # the synthesis loop below parses the JSON and writes fire_hue_mask +
    # fire_value_mask as CSV-of-CSV strings to NVS. Omit on lamps that
    # don't use the fire effect — synthesis is then skipped entirely.
    "FIRE_PATTERN   fire_pattern   string"
)

# Pull a CONFIG_PLAIIIN_<KEY> value from the .defaults file; strips quotes.
get() {
    local key="$1"
    awk -F= -v k="CONFIG_PLAIIIN_$key" \
        '$1==k{sub(/^"/,"",$2);sub(/"$/,"",$2);print $2;exit}' "$DEFAULTS"
}

# Normalise Kconfig truthiness ("y"/"Y"/"true") → 1, everything else → as-is.
norm_bool() {
    case "$1" in
        y|Y|yes|true|TRUE) echo 1 ;;
        n|N|no|false|FALSE) echo 0 ;;
        *) echo "$1" ;;
    esac
}

# Emit a value as a CSV field. nvs_partition_gen.py parses the CSV with
# Python's csv module, so a value containing commas (the wh_phys JSON) must be
# double-quoted with internal quotes doubled. Comma/quote-free values pass
# through bare, so every existing profile produces byte-identical CSV.
csv_field() {
    local v="$1"
    case "$v" in
        *,*|*\"*) printf '"%s"' "${v//\"/\"\"}" ;;
        *)        printf '%s' "$v" ;;
    esac
}

# --- Build NVS CSV ------------------------------------------------------------
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT
CSV="$TMPDIR/nvs.csv"
BIN="$TMPDIR/nvs.bin"

{
    echo "key,type,encoding,value"
    echo "$NVS_NAMESPACE,namespace,,"
    for row in "${SCHEMA[@]}"; do
        # shellcheck disable=SC2086
        set -- $row
        cfg_key="$1"; nvs_key="$2"; enc="$3"
        val="$(get "$cfg_key")"
        [ -z "$val" ] && continue
        if [ "$enc" = "i32" ] || [ "$enc" = "u32" ] || [ "$enc" = "u8" ]; then
            val="$(norm_bool "$val")"
        fi
        echo "$nvs_key,data,$enc,$(csv_field "$val")"
    done

    # --- Wormhole physical per-ring config → wh_phys JSON (Phase 29) ---------
    # The profile carries one readable "face,direction,offset" line per ring
    # (CONFIG_PLAIIIN_WH_RING<n>); assemble them into the JSON array string the
    # firmware's wormhole module reads from NVS (see docs/wormhole-api.md).
    # Rings with no line default to all-zero, matching the firmware fallback.
    if [ "$(get FORM)" = "wormhole" ]; then
        wh_rings="$(get WH_RINGS)"
        [ -z "$wh_rings" ] && wh_rings=0
        phys_json="["
        r=0
        while [ "$r" -lt "$wh_rings" ]; do
            ring="$(get "WH_RING$r")"
            [ -z "$ring" ] && ring="0,0,0"
            IFS=, read -r face dir off _ <<<"$ring"
            [ "$r" -gt 0 ] && phys_json="$phys_json,"
            phys_json="$phys_json{\"face\":${face:-0},\"direction\":${dir:-0},\"offset\":${off:-0}}"
            r=$((r + 1))
        done
        phys_json="$phys_json]"
        echo "wh_phys,data,string,$(csv_field "$phys_json")"
    fi

    # --- Phase 35: FirePattern masks → fire_hue_mask + fire_value_mask -------
    # The profile names a pattern file (e.g. fire8x8) via CONFIG_PLAIIIN_FIRE_PATTERN.
    # We pull the matching adaptations/fire/<name>.pattern JSON, extract the
    # hue_mask and value_mask strings, and write them as NVS strings the
    # firmware's fire.c parses at init(). No HTTP endpoint to set masks —
    # they're physical-lamp config, like the wormhole per-ring data above.
    fire_pattern_name="$(get FIRE_PATTERN)"
    if [ -n "$fire_pattern_name" ]; then
        fire_pattern_file="$PROJECT_DIR/adaptations/fire/${fire_pattern_name}.pattern"
        if [ ! -f "$fire_pattern_file" ]; then
            echo "FIRE_PATTERN='$fire_pattern_name' but $fire_pattern_file missing" >&2
            exit 1
        fi
        # .pattern files are *almost* JSON but their hue_mask/value_mask
        # strings contain raw newlines — strict json parsers reject them.
        # Pull the string values out with a regex that handles embedded
        # newlines and escapes. (Original C++ used Arduino's lenient parser.)
        fire_hue="$("$PYTHON" -c "
import re, sys
src = open(sys.argv[1]).read()
m = re.search(r'\"hue_mask\"\s*:\s*\"((?:[^\"\\\\]|\\\\.)*)\"', src, re.DOTALL)
print(m.group(1) if m else '')
" "$fire_pattern_file")"
        fire_value="$("$PYTHON" -c "
import re, sys
src = open(sys.argv[1]).read()
m = re.search(r'\"value_mask\"\s*:\s*\"((?:[^\"\\\\]|\\\\.)*)\"', src, re.DOTALL)
print(m.group(1) if m else '')
" "$fire_pattern_file")"
        if [ -z "$fire_hue" ] || [ -z "$fire_value" ]; then
            echo "could not extract hue_mask/value_mask from $fire_pattern_file" >&2
            exit 1
        fi
        # Strip embedded newlines/CR — fire.c's parser tolerates them but the
        # NVS-gen CSV must not contain raw newlines outside the quoted field.
        fire_hue="$(printf '%s' "$fire_hue" | tr -d '\r\n')"
        fire_value="$(printf '%s' "$fire_value" | tr -d '\r\n')"
        echo "fire_hue_mask,data,string,$(csv_field "$fire_hue")"
        echo "fire_value_mask,data,string,$(csv_field "$fire_value")"
    fi
} > "$CSV"

echo "--- profile $PROFILE → NVS ---"
sed 's/^/  /' "$CSV"

# --- Generate NVS partition image --------------------------------------------
echo "=== Generating NVS image ($NVS_SIZE bytes) ==="
"$PYTHON" "$NVS_GEN" generate "$CSV" "$BIN" "$NVS_SIZE"

# --- Optional full erase + firmware flash ------------------------------------
if [ "$FULL" -eq 1 ]; then
    # Phase 35 — per-form firmware. The binary name now embeds the FORM so a
    # `--full tower` burn picks tower-app, not whatever was built last. Profiles
    # without a FORM (unlikely) fall back to the legacy '*-flash.bin' glob.
    PROFILE_FORM="$(get FORM)"
    if [ -n "$PROFILE_FORM" ]; then
        FLASH_BIN="$(ls -1t "$PROJECT_DIR/build/dist/"plaiiinlight_os-*-"$PROFILE_FORM"-flash.bin 2>/dev/null | head -1)"
    else
        FLASH_BIN="$(ls -1t "$PROJECT_DIR/build/dist/"plaiiinlight_os-*-flash.bin 2>/dev/null | head -1)"
    fi
    if [ ! -f "$FLASH_BIN" ]; then
        if [ -n "$PROFILE_FORM" ]; then
            echo "no flash.bin for form '$PROFILE_FORM' in build/dist — run scripts/build.sh --form $PROFILE_FORM first" >&2
        else
            echo "no flash.bin in build/dist — run scripts/build.sh --form <name> first" >&2
        fi
        exit 1
    fi
    echo "=== --full: erase_flash ==="
    "$PYTHON" "$ESPTOOL" --chip esp32 --port "$PORT" --baud "$BAUD" erase_flash
    echo "=== --full: write_flash 0x0 $(basename "$FLASH_BIN") ==="
    "$PYTHON" "$ESPTOOL" --chip esp32 --port "$PORT" --baud "$BAUD" \
        write_flash 0x0 "$FLASH_BIN"

    # --- byForm effects + form template: SPIFFS image -----------------------
    # Phase 25/26 — form-specific effects and the AI form-prompt template ship
    # per-device, not embedded in the firmware. We build one SPIFFS image of
    # the form's .js files plus its prompt-inject template and flash it to the
    # `storage` partition. The firmware compiles each .js to .bc on boot and
    # serves the template via GET /api/form-prompt.
    FORM_VAL="$(get FORM)"
    EFFECTS_DIR="$PROJECT_DIR/effects/$FORM_VAL"
    TEMPLATE_FILE="$PROJECT_DIR/form-template/$FORM_VAL.txt"
    SPIFFS_SRC="$TMPDIR/effects_src"
    mkdir -p "$SPIFFS_SRC"
    if [ -n "$FORM_VAL" ] && compgen -G "$EFFECTS_DIR/*.js" >/dev/null 2>&1; then
        cp "$EFFECTS_DIR"/*.js "$SPIFFS_SRC/"
    fi
    # Phase 26 — the form's prompt-inject template, flashed as _form_template.txt.
    if [ -n "$FORM_VAL" ] && [ -f "$TEMPLATE_FILE" ]; then
        cp "$TEMPLATE_FILE" "$SPIFFS_SRC/_form_template.txt"
    fi

    if compgen -G "$SPIFFS_SRC/*" >/dev/null 2>&1; then
        [ -f "$SPIFFS_GEN" ] || { echo "no spiffsgen.py at $SPIFFS_GEN (set IDF_PATH)" >&2; exit 1; }
        # storage partition geometry — parsed from partitions.csv so a layout
        # change doesn't silently flash to the wrong offset.
        read -r ST_OFF ST_SIZE < <(awk -F, '
            { gsub(/[ \t]/,"",$1) }
            $1=="storage" { gsub(/[ \t]/,"",$4); gsub(/[ \t]/,"",$5); print $4, $5; exit }
        ' "$PROJECT_DIR/partitions.csv")
        [ -n "${ST_OFF:-}" ] && [ -n "${ST_SIZE:-}" ] || {
            echo "could not parse 'storage' partition from partitions.csv" >&2; exit 1; }

        echo "=== --full: form '$FORM_VAL' → SPIFFS @ $ST_OFF ==="
        ls "$SPIFFS_SRC" | sed 's|^|  + |'

        SPIFFS_IMG="$TMPDIR/storage.bin"
        # spiffsgen.py's defaults (page 256 / block 4096 / magic) match the
        # firmware's esp_spiffs defaults — no extra flags needed.
        "$PYTHON" "$SPIFFS_GEN" "$((ST_SIZE))" "$SPIFFS_SRC" "$SPIFFS_IMG"
        "$PYTHON" "$ESPTOOL" --chip esp32 --port "$PORT" --baud "$BAUD" \
            write_flash "$ST_OFF" "$SPIFFS_IMG"
    else
        echo "=== --full: no byForm effects or template for form '${FORM_VAL:-?}' ==="
    fi
fi

# --- Write NVS partition (no app touch) --------------------------------------
echo "=== Writing NVS @ $NVS_OFFSET ==="
"$PYTHON" "$ESPTOOL" --chip esp32 --port "$PORT" --baud "$BAUD" \
    --after hard_reset write_flash "$NVS_OFFSET" "$BIN"

echo "=== Done — $DEVICE booted with profile applied. ==="
if [ "$FULL" -eq 1 ]; then
    echo "On first boot the firmware compiles every stored .js to .bc — the"
    echo "general built-ins plus any byForm effects flashed above."
fi
echo "Next: do the WiFi/BLE onboarding step (BLE sheet on the tablet, or"
echo "      connect to the lamp's AP and POST /network with SSID + password)."
