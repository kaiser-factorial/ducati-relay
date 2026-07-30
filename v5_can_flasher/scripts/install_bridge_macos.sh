#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "Usage: $0 /dev/cu.usbserial-YOUR_PORT" >&2
  echo "Find the port with: arduino-cli board list" >&2
  exit 2
fi

SERIAL_PORT="$1"
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PACKAGE_DIR="$(cd -- "$SCRIPT_DIR/.." && pwd)"
SKETCH_DIR="$PACKAGE_DIR/firmware/v5_can_flasher"

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "ERROR: install_bridge_macos.sh is intended for macOS." >&2
  exit 1
fi
if ! command -v arduino-cli >/dev/null 2>&1; then
  echo "ERROR: arduino-cli is not installed." >&2
  echo "Install it with Homebrew: brew install arduino-cli" >&2
  exit 1
fi
if [[ ! -c "$SERIAL_PORT" ]]; then
  echo "ERROR: serial port not found: $SERIAL_PORT" >&2
  arduino-cli board list >&2 || true
  exit 1
fi

echo "Compiling and uploading the v5 SLCAN bridge..."
arduino-cli compile --upload \
  --fqbn esp32:esp32:esp32 \
  -p "$SERIAL_PORT" \
  "$SKETCH_DIR"

echo
echo "Bridge firmware installed on $SERIAL_PORT."
echo "Close all serial tools, then attach this ESP32 USB device to the Linux VM."
