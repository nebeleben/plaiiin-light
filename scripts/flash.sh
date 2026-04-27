#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
IDF_DIR="${HOME}/esp/esp-idf"
PORT="${1:-/dev/ttyUSB0}"

if [ ! -d "$IDF_DIR" ]; then
    echo "Error: ESP-IDF not found at $IDF_DIR"
    echo "Run: ./scripts/setup.sh"
    exit 1
fi

source "$IDF_DIR/export.sh"
cd "$PROJECT_DIR"

echo "=== Flashing PlaiiinLightOS to $PORT ==="
idf.py -p "$PORT" flash

echo ""
echo "=== Flash complete ==="
echo "Monitor: idf.py -p $PORT monitor"
