#!/usr/bin/env bash
# Phase 36 — verify a profile defaults file (or every profile) for
# completeness. Run automatically by profile-burn.sh so a silent omission
# (missing FIRE_PATTERN, mismatched LAMP_TYPE, etc.) fails fast at flash
# time instead of producing a device that boots into a dim/broken state.
#
# Usage:
#   scripts/profile-verify.sh <family>/<device>          — verify one profile
#   scripts/profile-verify.sh --all                       — verify every profile
#   scripts/profile-verify.sh <path/to/file.defaults>     — verify a raw path
#
# Exit codes: 0 = all OK (warnings allowed), 1 = at least one hard failure.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
PROFILES_DIR="$PROJECT_DIR/profiles"
PATTERNS_DIR="$PROJECT_DIR/adaptations/fire"

# Required for every profile.
REQUIRED=(NODE_NAME LED_PIN LED_COUNT LED_TYPE LAMP_TYPE FORM AP_JS)

# Forms whose hardcoded effects depend on FIRE_PATTERN masks in NVS.
# (rocket uses 'flight' which synthesises its gradient internally; wormhole
# and display have no hardcoded fire at all.)
FIRE_FORMS=(tower cube)

# Read CONFIG_PLAIIIN_<key> from a profile, stripping surrounding quotes.
# Returns empty (exit 0) when the key isn't set — using awk because grep+pipefail
# would kill the calling function under set -e when there's no match.
read_key() {
    local file=$1 key=$2
    awk -F= -v k="CONFIG_PLAIIIN_$key" '
        $1 == k {
            v = substr($0, length(k) + 2)
            sub(/^"/, "", v); sub(/"$/, "", v)
            print v
            exit
        }
    ' "$file"
}

verify_one() {
    local file=$1
    local fail=0 warn=0
    local -a issues=()

    if [ ! -f "$file" ]; then
        echo "FAIL $file: not a file" >&2
        return 1
    fi

    for k in "${REQUIRED[@]}"; do
        if ! grep -q "^CONFIG_PLAIIIN_${k}=" "$file"; then
            issues+=("missing required key: $k")
            fail=1
        fi
    done

    local form lamp_type led_count fire_pattern wh_rings
    form=$(read_key "$file" FORM)
    lamp_type=$(read_key "$file" LAMP_TYPE)
    led_count=$(read_key "$file" LED_COUNT)
    fire_pattern=$(read_key "$file" FIRE_PATTERN)
    wh_rings=$(read_key "$file" WH_RINGS)

    # Form-specific FIRE_PATTERN rules.
    local needs_fire=0
    for f in "${FIRE_FORMS[@]}"; do
        if [ "$form" = "$f" ]; then needs_fire=1; break; fi
    done
    if [ "$needs_fire" = "1" ]; then
        if [ -z "$fire_pattern" ]; then
            issues+=("$form profile must set FIRE_PATTERN (matches LAMP_TYPE)")
            fail=1
        else
            if [ ! -f "$PATTERNS_DIR/${fire_pattern}.pattern" ]; then
                issues+=("FIRE_PATTERN='$fire_pattern' but $PATTERNS_DIR/${fire_pattern}.pattern not found")
                fail=1
            fi
            if [[ "$lamp_type" =~ ^matrix([0-9]+)x([0-9]+)$ ]]; then
                local w="${BASH_REMATCH[1]}" h="${BASH_REMATCH[2]}"
                local expected="fire${w}x${h}"
                # For multi-panel forms (cube) the per-panel matrix dim is the
                # fire grid dim — fire8x8 is correct for cube's matrix8x8 too.
                if [ "$fire_pattern" != "$expected" ]; then
                    issues+=("FIRE_PATTERN='$fire_pattern' doesn't match LAMP_TYPE='$lamp_type' (expected '$expected')")
                    warn=1
                fi
            fi
        fi
    elif [ -n "$fire_pattern" ]; then
        issues+=("$form profile sets FIRE_PATTERN but its hardcoded effects don't use NVS masks (ignored)")
        warn=1
    fi

    # Wormhole-specific structural rule: LED_COUNT must equal 24 * WH_RINGS.
    if [ "$form" = "wormhole" ]; then
        if [ -z "$wh_rings" ]; then
            issues+=("wormhole profile must set WH_RINGS")
            fail=1
        elif [ -n "$led_count" ]; then
            local expected_leds=$((wh_rings * 24))
            if [ "$led_count" != "$expected_leds" ]; then
                issues+=("LED_COUNT=$led_count but WH_RINGS=$wh_rings expects ${expected_leds}")
                fail=1
            fi
        fi
    fi

    if [ "$fail" = "1" ]; then
        echo "FAIL $file:"
        for i in "${issues[@]}"; do echo "  - $i"; done
        return 1
    elif [ "$warn" = "1" ]; then
        echo "WARN $file:"
        for i in "${issues[@]}"; do echo "  - $i"; done
        return 0
    else
        echo "OK   $file"
        return 0
    fi
}

resolve_path() {
    local arg=$1
    if [ -f "$arg" ]; then echo "$arg"; return; fi
    if [ -f "$PROFILES_DIR/$arg.defaults" ]; then
        echo "$PROFILES_DIR/$arg.defaults"; return
    fi
    echo "$arg"
}

main() {
    if [ "$#" -eq 0 ]; then
        echo "usage: $0 <family>/<device> | --all | <path>" >&2
        exit 2
    fi

    if [ "$1" = "--all" ]; then
        local failed=0
        while IFS= read -r f; do
            verify_one "$f" || failed=1
        done < <(find "$PROFILES_DIR" -name "*.defaults" | sort)
        exit "$failed"
    fi

    local path
    path=$(resolve_path "$1")
    verify_one "$path"
}

main "$@"
