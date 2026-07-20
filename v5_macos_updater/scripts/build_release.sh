#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PACKAGE_DIR="$(cd -- "$SCRIPT_DIR/.." && pwd)"
DIST_DIR="$PACKAGE_DIR/dist"

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "ERROR: this release target is for macOS." >&2
  exit 1
fi
if ! command -v swift >/dev/null 2>&1; then
  echo "ERROR: Swift is unavailable. Install Apple's Command Line Tools with: xcode-select --install" >&2
  exit 1
fi

cd "$PACKAGE_DIR"
swift build -c release --product ducati-flasher
swift build -c release --product ducati-flasher-selftest
mkdir -p "$DIST_DIR"
cp .build/release/ducati-flasher "$DIST_DIR/ducati-flasher"
cp .build/release/ducati-flasher-selftest "$DIST_DIR/ducati-flasher-selftest"

echo "Native binaries created:"
echo "  $DIST_DIR/ducati-flasher"
echo "  $DIST_DIR/ducati-flasher-selftest"
