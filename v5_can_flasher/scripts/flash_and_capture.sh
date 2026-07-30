#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 2 || $# -gt 3 ]]; then
  echo "Usage: $0 /dev/ttyUSB0 firmware.srec [output-directory]" >&2
  exit 2
fi

SERIAL_DEVICE="$1"
FIRMWARE_FILE="$(realpath "$2")"
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PACKAGE_DIR="$(cd -- "$SCRIPT_DIR/.." && pwd)"
CAN_INTERFACE="${CAN_FLASHER_INTERFACE:-slcan0}"
BOOT_COMMANDER="${BOOT_COMMANDER:-$PACKAGE_DIR/tools/openblt/Host/BootCommander}"
TIMESTAMP="$(date -u +%Y%m%dT%H%M%SZ)"
OUTPUT_DIR="${3:-$PACKAGE_DIR/captures/$TIMESTAMP}"

if [[ "$(uname -s)" != "Linux" ]]; then
  echo "ERROR: flash_and_capture.sh must run inside Linux." >&2
  exit 1
fi
if [[ ! -f "$FIRMWARE_FILE" ]]; then
  echo "ERROR: firmware file not found: $FIRMWARE_FILE" >&2
  exit 1
fi
if [[ "$FIRMWARE_FILE" != *.srec && "$FIRMWARE_FILE" != *.s19 && "$FIRMWARE_FILE" != *.mot ]]; then
  echo "ERROR: OpenBLT requires a Motorola S-record (.srec/.s19/.mot) firmware file." >&2
  exit 1
fi
if [[ ! -x "$BOOT_COMMANDER" ]]; then
  echo "ERROR: BootCommander not found: $BOOT_COMMANDER" >&2
  echo "Run scripts/setup_linux.sh first or export BOOT_COMMANDER=/path/to/BootCommander." >&2
  exit 1
fi

mkdir -p "$OUTPUT_DIR"
EXCHANGE_LOG="$OUTPUT_DIR/openblt_exchange.log"
BOOT_LOG="$OUTPUT_DIR/bootcommander.log"
METADATA="$OUTPUT_DIR/metadata.txt"
SUMMARY="$OUTPUT_DIR/summary.txt"

SLCAN_STARTED=0
CANDUMP_PID=""
cleanup() {
  if [[ -n "$CANDUMP_PID" ]]; then
    kill "$CANDUMP_PID" 2>/dev/null || true
    wait "$CANDUMP_PID" 2>/dev/null || true
  fi
  if [[ "$SLCAN_STARTED" -eq 1 ]]; then
    "$SCRIPT_DIR/stop_slcan.sh" "$CAN_INTERFACE" || true
  fi
}
trap cleanup EXIT INT TERM

{
  echo "capture_utc=$TIMESTAMP"
  echo "host=$(hostname)"
  echo "kernel=$(uname -a)"
  echo "serial_device=$SERIAL_DEVICE"
  echo "can_interface=$CAN_INTERFACE"
  echo "can_bitrate=500000"
  echo "openblt_tx_id=0x667"
  echo "openblt_rx_id=0x7E1"
  echo "bootcommander=$BOOT_COMMANDER"
  sha256sum "$FIRMWARE_FILE"
} > "$METADATA"

"$SCRIPT_DIR/start_slcan.sh" "$SERIAL_DEVICE" "$CAN_INTERFACE"
SLCAN_STARTED=1

# Preserve the raw successful exchange in candump's replayable absolute-time format.
# Filters use exact 11-bit IDs and intentionally exclude unrelated motorcycle traffic.
stdbuf -oL candump -L "$CAN_INTERFACE,667:7FF,7E1:7FF" > "$EXCHANGE_LOG" &
CANDUMP_PID=$!
sleep 0.25

echo
echo "Flashing $FIRMWARE_FILE"
echo "Do not disconnect ECU power, USB, CAN wiring, or suspend the VM."
echo

set +e
"$BOOT_COMMANDER" \
  -s=xcp \
  -t=xcp_can \
  -d="$CAN_INTERFACE" \
  -b=500000 \
  -tid=667 \
  -rid=7E1 \
  -xid=0 \
  "$FIRMWARE_FILE" 2>&1 | tee "$BOOT_LOG"
BOOT_STATUS=${PIPESTATUS[0]}
set -e

sleep 0.25
kill "$CANDUMP_PID" 2>/dev/null || true
wait "$CANDUMP_PID" 2>/dev/null || true
CANDUMP_PID=""

set +e
python3 "$SCRIPT_DIR/summarize_capture.py" "$EXCHANGE_LOG" --output "$SUMMARY"
SUMMARY_STATUS=$?
set -e

{
  echo "bootcommander_exit_code=$BOOT_STATUS"
  echo "capture_summary_exit_code=$SUMMARY_STATUS"
} >> "$METADATA"

echo
echo "Capture package: $OUTPUT_DIR"
echo "  Exchange: $EXCHANGE_LOG"
echo "  Updater:  $BOOT_LOG"
echo "  Summary:  $SUMMARY"

if [[ "$BOOT_STATUS" -ne 0 ]]; then
  echo "ERROR: BootCommander reported failure; the capture was retained for diagnosis." >&2
  exit "$BOOT_STATUS"
fi
if [[ "$SUMMARY_STATUS" -ne 0 ]]; then
  echo "ERROR: updater exited successfully, but both request and response frames were not captured." >&2
  exit 1
fi

echo "SUCCESS: BootCommander completed and a bidirectional OpenBLT exchange was captured."
