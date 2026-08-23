# Ducati v7.2 ESP32-S3 native-USB CAN terminal

v7.2 is the ESP32-S3 version of the interactive CAN sender. It uses the S3's
native USB Serial/JTAG CDC connection, fixed-size command parsing, compact
machine-friendly messages, and larger TWAI transmit/receive queues.

## Wiring

The ESP32-S3 still requires an external 3.3 V CAN transceiver such as an
SN65HVD230.

| ESP32-S3 | SN65HVD230-type module |
|---|---|
| GPIO4 | CAN TX / transceiver D input |
| GPIO5 | CAN RX / transceiver R output |
| 3.3 V | 3.3 V |
| GND | GND |

Connect CAN_H, CAN_L, and a common ground to the bus. The bus remains classic
CAN at 500 kbit/s and needs correct termination. GPIO19 and GPIO20 are left
alone because they carry native USB D- and D+ on the S3.

If GPIO4/5 are unavailable on a particular S3 board, change `CAN_TX_PIN` and
`CAN_RX_PIN` at the top of the sketch before building.

## Compact serial protocol

The format mirrors Linux `cansend` notation and avoids tokenizing many words:

```text
300#01
123#DEADBEEF
18DAF110#021003
123#R8
```

- Exactly 3 ID digits select a standard 11-bit frame.
- Exactly 8 ID digits select an extended 29-bit frame.
- The data after `#` is 0-16 hex digits (0-8 bytes) without spaces.
- `R0` through `R8` after `#` sends a remote frame.
- Received frames print as `rx 300#01`; queued frames print as `tx 300#01`.
- `ack` confirms that another CAN node acknowledged the transmission.
- `err no-ack` means no node acknowledged the frame.

Control commands are `:help`, `:status`, `:listen on|off`, `:single on|off`,
and `:recover`.

## Build and upload

The USB options are important: Hardware CDC supplies both flashing and the
native serial terminal, and CDC-on-boot makes Arduino `Serial` use that port.

```bash
arduino-cli compile \
  --fqbn esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc \
  v7_2_terminal_can_s3

arduino-cli board list

arduino-cli upload -p /dev/cu.usbmodem1101 \
  --fqbn esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc \
  v7_2_terminal_can_s3

arduino-cli monitor -p /dev/cu.usbmodem1101 -c baudrate=115200
```

Replace the example port with the port reported on the local Mac. Native USB
CDC does not use a physical UART baud rate; `115200` is retained as a familiar
monitor setting.

Arbitrary CAN frames can activate equipment. Test on the isolated, fused bench
bus before connecting this terminal to a motorcycle network.
