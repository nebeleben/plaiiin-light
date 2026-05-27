#!/usr/bin/env bash
# Apply a profile preset to a freshly-flashed device (first-boot onboarding).
#
#   Usage: ./scripts/onboard.sh <family>/<device> <host>
#     e.g. ./scripts/onboard.sh tower/tower8v2 192.168.4.1
#          ./scripts/onboard.sh display/matrix16v14 192.168.178.111
#
# Reads the profile's .defaults and POSTs the hardware fields to /config
# (then the device reboots into STA if WiFi creds are provided).
#
# This replaces per-profile firmware builds — the firmware itself is
# universal; profiles now only configure a device at onboarding.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

PROFILE="${1:-}"
HOST="${2:-}"
if [ -z "$PROFILE" ] || [ -z "$HOST" ]; then
    echo "Usage: $0 <family>/<device> <host>"
    echo "Available profiles:"
    for f in "$PROJECT_DIR/profiles"/*/; do
        fname="$(basename "$f")"
        for def in "$f"*.defaults; do
            [ -e "$def" ] || continue
            dname="$(basename "$def" .defaults)"
            [ "$dname" = "sdkconfig" ] && continue
            echo "  $fname/$dname"
        done
    done
    exit 2
fi

if [[ "$PROFILE" == */* ]]; then
    FAMILY="${PROFILE%%/*}"
    DEVICE="${PROFILE##*/}"
    DEFAULTS="$PROJECT_DIR/profiles/$FAMILY/$DEVICE.defaults"
    # WiFi creds intentionally not read from the profile any more — onboarding
    # the lamp onto a network is the user's job (BLE wifiConfig from the apps,
    # or the captive-portal /network page from the lamp's AP). Profiles burn
    # hardware-only config; secrets stay out of the repo.
    WIFI_LOCAL=""
else
    DEFAULTS="$PROJECT_DIR/profiles/$PROFILE/sdkconfig.defaults"
    WIFI_LOCAL=""
fi
[ -f "$DEFAULTS" ] || { echo "missing $DEFAULTS" >&2; exit 1; }

# Strip CONFIG_PLAIIIN_FOO="bar" → FOO=bar for easy lookup.
# WiFi creds prefer .wifi.local (gitignored) over the committed .defaults so
# secrets don't end up in the repo. The local file is optional.
get() {
    local key="$1"
    local v=""
    if [ -n "$WIFI_LOCAL" ] && [ -f "$WIFI_LOCAL" ]; then
        v=$(awk -F= -v k="CONFIG_PLAIIIN_$key" '$1==k{sub(/^"/,"",$2);sub(/"$/,"",$2);print $2;exit}' "$WIFI_LOCAL")
    fi
    if [ -z "$v" ]; then
        v=$(awk -F= -v k="CONFIG_PLAIIIN_$key" '$1==k{sub(/^"/,"",$2);sub(/"$/,"",$2);print $2;exit}' "$DEFAULTS")
    fi
    printf '%s' "$v"
}

NODE_NAME="$(get NODE_NAME)"
LED_PIN="$(get LED_PIN)"
LED_CLK_PIN="$(get LED_CLK_PIN)"
LED_COUNT="$(get LED_COUNT)"
LED_TYPE="$(get LED_TYPE)"
LAMP_TYPE="$(get LAMP_TYPE)"
FORM="$(get FORM)"
PX_GROUP_W="$(get PX_GROUP_W)"
PX_GROUP_H="$(get PX_GROUP_H)"
ROTATION="$(get ROTATION)"
ORIGIN="$(get ORIGIN)"
SERPENTINE="$(get SERPENTINE)"
SERP_AXIS="$(get SERP_AXIS)"
BTN_PWR_PIN="$(get BTN_PWR_PIN)"
BTN_NEXT_PIN="$(get BTN_NEXT_PIN)"
BTN_PREV_PIN="$(get BTN_PREV_PIN)"
echo "Onboarding $PROFILE → http://$HOST"
echo "  node=$NODE_NAME led=${LED_COUNT}×${LED_TYPE} data=$LED_PIN clk=${LED_CLK_PIN:--1} lamp=$LAMP_TYPE form=$FORM"

# 1. Device/LED config — reboots the device.
FORM_ARGS=()
[ -n "$NODE_NAME" ]    && FORM_ARGS+=(--data-urlencode "node_name=$NODE_NAME")
[ -n "$LED_PIN" ]      && FORM_ARGS+=(--data-urlencode "led_pin=$LED_PIN")
[ -n "$LED_CLK_PIN" ]  && FORM_ARGS+=(--data-urlencode "led_clk_pin=$LED_CLK_PIN")
[ -n "$LED_COUNT" ]    && FORM_ARGS+=(--data-urlencode "led_count=$LED_COUNT")
[ -n "$LED_TYPE" ]     && FORM_ARGS+=(--data-urlencode "led_type=$LED_TYPE")
[ -n "$LAMP_TYPE" ]    && FORM_ARGS+=(--data-urlencode "lamp_type=$LAMP_TYPE")
[ -n "$FORM" ]         && FORM_ARGS+=(--data-urlencode "lamp_form=$FORM")
[ -n "$BTN_PWR_PIN" ]  && FORM_ARGS+=(--data-urlencode "btn_pwr_pin=$BTN_PWR_PIN")
[ -n "$BTN_NEXT_PIN" ] && FORM_ARGS+=(--data-urlencode "btn_next_pin=$BTN_NEXT_PIN")
[ -n "$BTN_PREV_PIN" ] && FORM_ARGS+=(--data-urlencode "btn_prev_pin=$BTN_PREV_PIN")

echo "  → POST /config (device will reboot)"
curl -sS -X POST "http://$HOST/config" "${FORM_ARGS[@]}" -o /dev/null -w "    HTTP=%{http_code}\n"

# 2. Wait for the device to come back up at the same host. WiFi setup is no
#    longer part of profile onboarding — use BLE wifiConfig (apps) or the
#    captive-portal /network page after the lamp boots into AP mode.
echo "  → waiting for device to reboot..."
for i in $(seq 1 30); do
    if curl -sf -m 2 "http://$HOST/api" >/dev/null 2>&1; then
        echo "    online after ${i}s"
        break
    fi
    sleep 1
done

# 2b. Pixel grouping + orientation — live-apply endpoints, no reboot.
if [ -n "$PX_GROUP_W" ] && [ -n "$PX_GROUP_H" ]; then
    echo "  → POST /api/grid  (pixel group ${PX_GROUP_W}x${PX_GROUP_H})"
    curl -sS -X POST "http://$HOST/api/grid" -H "Content-Type: application/json" \
        -d "{\"pixelGroupW\":${PX_GROUP_W},\"pixelGroupH\":${PX_GROUP_H}}" \
        -o /dev/null -w "    HTTP=%{http_code}\n"
fi
if [ -n "$ROTATION$ORIGIN$SERPENTINE$SERP_AXIS" ]; then
    serp_bool="true"
    case "$SERPENTINE" in y|Y|yes|true|1) serp_bool="true";; n|N|no|false|0) serp_bool="false";; esac
    echo "  → POST /api/orientation  (rot=${ROTATION:-0} origin=${ORIGIN:-0} serp=$serp_bool axis=${SERP_AXIS:-0})"
    curl -sS -X POST "http://$HOST/api/orientation" -H "Content-Type: application/json" \
        -d "{\"rotation\":${ROTATION:-0},\"origin\":${ORIGIN:-0},\"serpentine\":$serp_bool,\"serpentineAxis\":${SERP_AXIS:-0}}" \
        -o /dev/null -w "    HTTP=%{http_code}\n"
fi

echo ""
echo "Onboarding done. Final state (may be stale if the device moved networks):"
curl -sS -m 3 "http://$HOST/api" || true
echo
