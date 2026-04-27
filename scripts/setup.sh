#!/usr/bin/env bash
set -euo pipefail

# PlaiiinLightOS - ESP-IDF setup script
# Installs ESP-IDF v5.3.x and required tools

IDF_VERSION="v5.3.2"
IDF_DIR="${HOME}/esp/esp-idf"

echo "=== PlaiiinLightOS ESP-IDF Setup ==="

# Prerequisites check
if [[ "$(uname)" == "Darwin" ]]; then
    if ! command -v brew &>/dev/null; then
        echo "Error: Homebrew required. Install from https://brew.sh"
        exit 1
    fi
    # Install dependencies
    brew install cmake ninja dfu-util python3 2>/dev/null || true
elif [[ "$(uname)" == "Linux" ]]; then
    sudo apt-get update
    sudo apt-get install -y git wget flex bison gperf python3 python3-pip python3-venv \
        cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0
fi

# Clone ESP-IDF
if [ -d "$IDF_DIR" ]; then
    echo "ESP-IDF already exists at $IDF_DIR"
    cd "$IDF_DIR"
    git fetch --tags
    git checkout "$IDF_VERSION"
    git submodule update --init --recursive
else
    echo "Cloning ESP-IDF $IDF_VERSION..."
    mkdir -p "$(dirname "$IDF_DIR")"
    git clone --recursive --branch "$IDF_VERSION" https://github.com/espressif/esp-idf.git "$IDF_DIR"
fi

# Install tools
cd "$IDF_DIR"
./install.sh esp32

echo ""
echo "=== Setup complete ==="
echo ""
echo "Before building, run:"
echo "  source ${IDF_DIR}/export.sh"
echo ""
echo "Then build PlaiiinLightOS:"
echo "  cd lampos"
echo "  idf.py build"
echo ""
echo "Flash to device:"
echo "  idf.py -p /dev/ttyUSB0 flash"
