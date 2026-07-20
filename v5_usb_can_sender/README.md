# Ducati v5 USB-to-CAN bench sender

This turns a USB-capable classic ESP32 DevKit plus an SN65HVD230 transceiver into a simple CAN test
sender. The computer talks to the ESP32 over USB serial; the ESP32 transmits CAN frames to the
v5 rear relay controller.

The sender uses the ESP32's built-in TWAI/CAN controller, so it does not need an MCP2515.

## Bench topology

```text
Computer --USB--> ESP32 DevKit --TX/RX--> SN65HVD230
                                             |
                                        CAN_H/CAN_L
                                             |
Relay X4 board --SPI (v5 CAN mode)-----> MCP2515
```

## Computer-side ESP32 wiring

| SN65HVD230 | USB-capable ESP32 DevKit |
|---|---|
| 3.3V | 3.3V |
| GND | GND |
| CAN TX | GPIO21 (TWAI TX) |
| CAN RX | GPIO22 (TWAI RX) |

The Relay X4 receiver uses GPIO18 SCK, GPIO19 MISO, **GPIO13 MOSI**, and GPIO5 CS, as documented
in `../v5_rear_can/README.md`. Its MOSI pin differs because GPIO23 drives that board's D14 LED.

Connect the SN65HVD230 `CAN_H` to the MCP2515 module's `CAN_H`, and `CAN_L` to `CAN_L`. Connect the grounds
of both ESP/CAN assemblies. With power removed, the completed bus should measure about 60 ohms
between CAN_H and CAN_L, produced by one 120-ohm terminator at each physical end.

Both nodes use 500 kbit/s. The relay-side MCP2515 is configured for its verified 8 MHz crystal.
The SN65HVD230 sender module is powered from 3.3 V. If its fixed onboard resistor measures about
120 ohms between CAN_H and CAN_L, it supplies the terminator at the sender end.

## Prepare the relay receiver

Before connecting CAN, remove the three temporary test switches. In
`../v5_rear_can/v5_rear_can.ino`, set:

```cpp
constexpr bool TEST_MODE = false;
```

Then compile and OTA-upload it again. Test mode disables the receiver's MCP2515, so CAN frames
will do nothing until this flag is false.

## Compile and upload the sender

```bash
cd v5_usb_can_sender
arduino-cli compile --upload \
  --fqbn esp32:esp32:esp32 \
  -p /dev/cu.usbserial-YOUR_PORT \
  v5_usb_can_sender.ino
```

Find the sender's USB port with `arduino-cli board list`.

Open its serial console:

```bash
arduino-cli monitor \
  -p /dev/cu.usbserial-YOUR_PORT \
  -c baudrate=115200
```

Enter a command followed by Return:

| Command | Result |
|---|---|
| `l` | Toggle left by sending its explicit new ON/OFF state |
| `r` | Toggle right by sending its explicit new ON/OFF state |
| `h` | Set both ON/OFF atomically using CAN ID `0x304` |
| `bon` | Brake ON |
| `boff` | Brake OFF |
| `status` | Print sender's tracked state |
| `help` | Print commands |

## Physical left-test switch

Connect a momentary or maintained switch between sender ESP32 **GPIO18** and **GND**. The
firmware enables the internal pull-up, so no external resistor or positive-voltage connection
is needed. Each transition from open to closed sends the left indicator's explicit new state;
opening the switch sends no CAN command. The input uses a non-blocking 35 ms debounce timer.

The sender tracks turn-signal state locally for display, but the CAN receiver remains the
actual source of relay state. Reset both boards together if their displayed states become
out of sync during bench experiments.

The serial log distinguishes a frame merely accepted into the ESP32 transmit queue from a
frame acknowledged on the physical bus. A successful test prints `[TX QUEUED]` followed by
`[CAN ACK]`. Repeated `no ACK`, bus-error, or bus-off messages indicate a wiring, bitrate,
transceiver-power, or termination problem. A two-second heartbeat prints the controller state
and cumulative error counters even when no command is entered.

Frames use TWAI single-shot mode during bench diagnostics. A broken or unacknowledged bus
therefore produces one clear failure instead of retrying the same frame indefinitely.
