#!/usr/bin/env bash
# Apply a profile preset to a freshly-flashed device (first-boot onboarding).
#
#   Usage: ./scripts/onboard.sh <family>/<device> <host>
#     e.g. ./scripts/onboard.sh tower/tower8v2 192.168.4.1
#          ./scripts/onboard.sh wall/matrix16v14 192.168.178.111
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
else
    DEFAULTS="$PROJECT_DIR/profiles/$PROFILE/sdkconfig.defaults"
fi
[ -f "$DEFAULTS" ] || { echo "missing $DEFAULTS" >&2; exit 1; }

# Strip CONFIG_PLAIIIN_FOO="bar" → FOO=bar for easy lookup.
get() {
    local key="$1"
    awk -F= -v k="CONFIG_PLAIIIN_$key" '$1==k{sub(/^"/,"",$2);sub(/"$/,"",$2);print $2;exit}' "$DEFAULTS"
}

NODE_NAME="$(get NODE_NAME)"
LED_PIN="$(get LED_PIN)"
LED_CLK_PIN="$(get LED_CLK_PIN)"
LED_COUNT="$(get LED_COUNT)"
LED_TYPE="$(get LED_TYPE)"
LAMP_TYPE="$(get LAMP_TYPE)"
FORM="$(get FORM)"
SSID="$(get NETWORK_SSID)"
PW="$(get NETWORK_PW)"

echo "Onboarding $PROFILE → http://$HOST"
echo "  node=$NODE_NAME led=${LED_COUNT}×${LED_TYPE} data=$LED_PIN clk=${LED_CLK_PIN:--1} lamp=$LAMP_TYPE form=$FORM"

# 1. Device/LED config first — this reboots the device, so send it before WiFi.
FORM_ARGS=()
[ -n "$NODE_NAME" ]    && FORM_ARGS+=(--data-urlencode "node_name=$NODE_NAME")
[ -n "$LED_PIN" ]      && FORM_ARGS+=(--data-urlencode "led_pin=$LED_PIN")
[ -n "$LED_CLK_PIN" ]  && FORM_ARGS+=(--data-urlencode "led_clk_pin=$LED_CLK_PIN")
[ -n "$LED_COUNT" ]    && FORM_ARGS+=(--data-urlencode "led_count=$LED_COUNT")
[ -n "$LED_TYPE" ]     && FORM_ARGS+=(--data-urlencode "led_type=$LED_TYPE")
[ -n "$LAMP_TYPE" ]    && FORM_ARGS+=(--data-urlencode "lamp_type=$LAMP_TYPE")
[ -n "$FORM" ]         && FORM_ARGS+=(--data-urlencode "lamp_form=$FORM")

echo "  → POST /config (device will reboot)"
curl -sS -X POST "http://$HOST/config" "${FORM_ARGS[@]}" -o /dev/null -w "    HTTP=%{http_code}\n"

# 2. Wait for the device to come back up at the same host. If WiFi creds are
#    supplied the final IP will only be stable after /network — give some time.
echo "  → waiting for device to reboot..."
for i in $(seq 1 30); do
    if curl -sf -m 2 "http://$HOST/api" >/dev/null 2>&1; then
        echo "    online after ${i}s"
        break
    fi
    sleep 1
done

# 3. Optional WiFi join. Only POST /network if we have creds.
if [ -n "$SSID" ]; then
    echo "  → POST /network  (ssid=$SSID)"
    curl -sS -X POST "http://$HOST/network" \
        --data-urlencode "ssid=$SSID" \
        --data-urlencode "pass=$PW" \
        -o /dev/null -w "    HTTP=%{http_code}\n"
    echo "  device will reboot and reconnect on the STA network — then find its new IP."
else
    echo "  (no WiFi creds in profile — keeping current network settings)"
fi

echo ""
echo "Onboarding done. Final state (may be stale if the device moved networks):"
curl -sS -m 3 "http://$HOST/api" || true
echo
