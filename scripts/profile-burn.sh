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
        echo "$nvs_key,data,$enc,$val"
    done
} > "$CSV"

echo "--- profile $PROFILE → NVS ---"
sed 's/^/  /' "$CSV"

# --- Generate NVS partition image --------------------------------------------
echo "=== Generating NVS image ($NVS_SIZE bytes) ==="
"$PYTHON" "$NVS_GEN" generate "$CSV" "$BIN" "$NVS_SIZE"

# --- Optional full erase + firmware flash ------------------------------------
if [ "$FULL" -eq 1 ]; then
    FLASH_BIN="$(ls -1t "$PROJECT_DIR/build/dist/"plaiiinlight_os-*-flash.bin 2>/dev/null | head -1)"
    [ -f "$FLASH_BIN" ] || { echo "no flash.bin in build/dist — run scripts/build.sh first" >&2; exit 1; }
    echo "=== --full: erase_flash ==="
    "$PYTHON" "$ESPTOOL" --chip esp32 --port "$PORT" --baud "$BAUD" erase_flash
    echo "=== --full: write_flash 0x0 $(basename "$FLASH_BIN") ==="
    "$PYTHON" "$ESPTOOL" --chip esp32 --port "$PORT" --baud "$BAUD" \
        write_flash 0x0 "$FLASH_BIN"

    # --- byForm effects: SPIFFS image of effects/<FORM>/*.js -----------------
    # Phase 25 — form-specific effects ship per-device, not embedded in the
    # firmware. We build a SPIFFS image of the form's .js files and flash it
    # to the `storage` partition; the firmware compiles each to .bc on boot.
    FORM_VAL="$(get FORM)"
    EFFECTS_DIR="$PROJECT_DIR/effects/$FORM_VAL"
    if [ -n "$FORM_VAL" ] && compgen -G "$EFFECTS_DIR/*.js" >/dev/null 2>&1; then
        [ -f "$SPIFFS_GEN" ] || { echo "no spiffsgen.py at $SPIFFS_GEN (set IDF_PATH)" >&2; exit 1; }
        # storage partition geometry — parsed from partitions.csv so a layout
        # change doesn't silently flash effects to the wrong offset.
        read -r ST_OFF ST_SIZE < <(awk -F, '
            { gsub(/[ \t]/,"",$1) }
            $1=="storage" { gsub(/[ \t]/,"",$4); gsub(/[ \t]/,"",$5); print $4, $5; exit }
        ' "$PROJECT_DIR/partitions.csv")
        [ -n "${ST_OFF:-}" ] && [ -n "${ST_SIZE:-}" ] || {
            echo "could not parse 'storage' partition from partitions.csv" >&2; exit 1; }

        echo "=== --full: byForm effects (form '$FORM_VAL') → SPIFFS @ $ST_OFF ==="
        ls "$EFFECTS_DIR"/*.js | sed 's|.*/|  + |'

        SPIFFS_SRC="$TMPDIR/effects_src"
        SPIFFS_IMG="$TMPDIR/storage.bin"
        mkdir -p "$SPIFFS_SRC"
        cp "$EFFECTS_DIR"/*.js "$SPIFFS_SRC/"
        # spiffsgen.py's defaults (page 256 / block 4096 / magic) match the
        # firmware's esp_spiffs defaults — no extra flags needed.
        "$PYTHON" "$SPIFFS_GEN" "$((ST_SIZE))" "$SPIFFS_SRC" "$SPIFFS_IMG"
        "$PYTHON" "$ESPTOOL" --chip esp32 --port "$PORT" --baud "$BAUD" \
            write_flash "$ST_OFF" "$SPIFFS_IMG"
    else
        echo "=== --full: no byForm effects for form '${FORM_VAL:-?}' (effects/$FORM_VAL absent or empty) ==="
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
