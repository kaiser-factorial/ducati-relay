#!/usr/bin/env bash
set -euo pipefail

OPENBLT_TAG="${OPENBLT_TAG:-openblt_v012200}"
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PACKAGE_DIR="$(cd -- "$SCRIPT_DIR/.." && pwd)"
TOOLS_DIR="${CAN_FLASHER_TOOLS_DIR:-$PACKAGE_DIR/tools}"
OPENBLT_DIR="$TOOLS_DIR/openblt"

if [[ "$(uname -s)" != "Linux" ]]; then
  echo "ERROR: setup_linux.sh must run inside Linux." >&2
  exit 1
fi

if [[ "$EUID" -eq 0 ]]; then
  SUDO=()
elif command -v sudo >/dev/null 2>&1; then
  SUDO=(sudo)
else
  echo "ERROR: run as root or install sudo." >&2
  exit 1
fi

echo "Installing Linux build and CAN tools..."
"${SUDO[@]}" apt-get update
"${SUDO[@]}" env DEBIAN_FRONTEND=noninteractive apt-get install -y \
  build-essential \
  can-utils \
  cmake \
  git \
  libusb-1.0-0-dev \
  python3

mkdir -p "$TOOLS_DIR"
if [[ ! -d "$OPENBLT_DIR/.git" ]]; then
  git clone --depth 1 --branch "$OPENBLT_TAG" \
    https://github.com/feaser/openblt.git "$OPENBLT_DIR"
else
  git -C "$OPENBLT_DIR" fetch --depth 1 origin "refs/tags/$OPENBLT_TAG:refs/tags/$OPENBLT_TAG"
  git -C "$OPENBLT_DIR" checkout --detach "$OPENBLT_TAG"
fi

echo "Building LibOpenBLT..."
cmake -S "$OPENBLT_DIR/Host/Source/LibOpenBLT" \
  -B "$OPENBLT_DIR/Host/Source/LibOpenBLT/build"
cmake --build "$OPENBLT_DIR/Host/Source/LibOpenBLT/build" --parallel

echo "Building BootCommander..."
cmake -S "$OPENBLT_DIR/Host/Source/BootCommander" \
  -B "$OPENBLT_DIR/Host/Source/BootCommander/build"
cmake --build "$OPENBLT_DIR/Host/Source/BootCommander/build" --parallel

BOOT_COMMANDER="$OPENBLT_DIR/Host/BootCommander"
if [[ ! -x "$BOOT_COMMANDER" ]]; then
  echo "ERROR: BootCommander was not produced at $BOOT_COMMANDER" >&2
  exit 1
fi

echo
echo "BootCommander installed: $BOOT_COMMANDER"
echo "Add your Linux user to dialout for serial access:"
if [[ "$EUID" -ne 0 ]]; then
  echo "  sudo usermod -aG dialout $USER"
  echo "Then log out and back in (or reboot the VM)."
else
  echo "  (not needed for this root-owned environment)"
fi
