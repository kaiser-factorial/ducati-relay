#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
  echo "Usage: $0 /dev/ttyUSB0 [interface-name]" >&2
  exit 2
fi

SERIAL_DEVICE="$1"
CAN_INTERFACE="${2:-slcan0}"
UART_BAUD="${SLCAN_UART_BAUD:-921600}"
PID_FILE="/tmp/ducati-${CAN_INTERFACE}-slcand.pid"

if [[ "$(uname -s)" != "Linux" ]]; then
  echo "ERROR: this script requires Linux." >&2
  exit 1
fi
if [[ ! -c "$SERIAL_DEVICE" ]]; then
  echo "ERROR: serial device not found: $SERIAL_DEVICE" >&2
  echo "Check UTM USB attachment and run: ls -l /dev/ttyUSB* /dev/ttyACM*" >&2
  exit 1
fi
if [[ -e "/sys/class/net/$CAN_INTERFACE" ]]; then
  echo "ERROR: CAN interface already exists: $CAN_INTERFACE" >&2
  echo "Run scripts/stop_slcan.sh $CAN_INTERFACE first." >&2
  exit 1
fi

sudo modprobe can
sudo modprobe can_raw
sudo modprobe slcan

# -F keeps slcand in the foreground so its exact PID can be stopped safely.
# -s6 selects 500 kbit/s and -S selects the USB serial line rate.
sudo slcand -F -o -c -f -s6 -S "$UART_BAUD" \
  "$SERIAL_DEVICE" "$CAN_INTERFACE" &
SLCAND_PID=$!
echo "$SLCAND_PID" | sudo tee "$PID_FILE" >/dev/null

for _ in {1..30}; do
  [[ -e "/sys/class/net/$CAN_INTERFACE" ]] && break
  sleep 0.1
done
if [[ ! -e "/sys/class/net/$CAN_INTERFACE" ]]; then
  sudo kill "$SLCAND_PID" 2>/dev/null || true
  sudo rm -f "$PID_FILE"
  echo "ERROR: slcand did not create $CAN_INTERFACE." >&2
  exit 1
fi

sudo ip link set "$CAN_INTERFACE" up
echo "$CAN_INTERFACE is up through $SERIAL_DEVICE at CAN 500000 bit/s (UART $UART_BAUD)."
ip -details link show "$CAN_INTERFACE"
