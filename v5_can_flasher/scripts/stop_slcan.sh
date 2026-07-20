#!/usr/bin/env bash
set -euo pipefail

CAN_INTERFACE="${1:-slcan0}"
PID_FILE="/tmp/ducati-${CAN_INTERFACE}-slcand.pid"

if [[ -e "/sys/class/net/$CAN_INTERFACE" ]]; then
  sudo ip link set "$CAN_INTERFACE" down || true
fi

if [[ -f "$PID_FILE" ]]; then
  SLCAND_PID="$(sudo cat "$PID_FILE")"
  if [[ "$SLCAND_PID" =~ ^[0-9]+$ ]]; then
    sudo kill "$SLCAND_PID" 2>/dev/null || true
  fi
  sudo rm -f "$PID_FILE"
fi

for _ in {1..20}; do
  [[ ! -e "/sys/class/net/$CAN_INTERFACE" ]] && break
  sleep 0.1
done

if [[ -e "/sys/class/net/$CAN_INTERFACE" ]]; then
  echo "WARNING: $CAN_INTERFACE still exists; reboot the VM before retrying." >&2
else
  echo "$CAN_INTERFACE stopped."
fi
