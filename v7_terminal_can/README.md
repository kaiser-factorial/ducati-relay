# Ducati v7 serial CAN terminal

This firmware turns the same classic ESP32 DevKit + SN65HVD230 hardware used by
the v5 USB sender into a general-purpose, interactive classic-CAN terminal. It
has no hardcoded light IDs or payloads: type an ID and zero to eight bytes, and
the ESP32 sends that frame. Received frames are printed continuously.

## Hardware and bus settings

- Serial terminal: 115200 baud, newline ending
- CAN bitrate: 500 kbit/s
- ESP32 GPIO21 -> transceiver `CAN TX`
- ESP32 GPIO22 <- transceiver `CAN RX`
- ESP32 3.3 V and GND -> transceiver 3.3 V and GND
- CAN_H/CAN_L and a common ground connect to the other CAN node
- Use 120-ohm termination at each physical end of the bus (about 60 ohms
  between CAN_H and CAN_L with power off)

## Terminal commands

All IDs, bytes, and remote-frame DLC values are entered as hexadecimal. A
leading `0x` is accepted but not required. Spaces or commas may separate bytes.

| Command | Action |
|---|---|
| `send 300 01` | Send standard ID `0x300`, one byte `0x01` |
| `send 123 DE AD BE EF` | Send a four-byte standard frame |
| `send 321` | Send a valid zero-byte standard frame |
| `sendx 18DAF110 02 10 03` | Send a 29-bit extended frame |
| `rtr 123 8` | Send a standard remote frame with DLC 8 |
| `rtrx 18DAF110 8` | Send an extended remote frame with DLC 8 |
| `listen on` / `listen off` | Enable or silence received-frame output |
| `single on` / `single off` | Enable or disable single-shot transmission |
| `status` | Show controller state and error/queue counters |
| `recover` | Start recovery after the controller enters bus-off |
| `help` | Print command help |

Single-shot mode is enabled by default. This makes a disconnected bench bus
fail once instead of retrying indefinitely. `TX QUEUED` means the controller
accepted the frame; the later `TX ACK` confirms that another active CAN node
acknowledged it. An ACK does not mean the receiving application understood or
acted on the message.

Example receive output:

```text
[RX      12345 ms] STD ID=0x300 DLC=1 DATA=01
[RX      12402 ms] EXT ID=0x18DAF110 DLC=3 DATA=02 10 03
```

## Build, upload, and use

From the repository root:

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 v7_terminal_can
arduino-cli board list
arduino-cli upload -p /dev/cu.usbserial-0001 \
  --fqbn esp32:esp32:esp32 v7_terminal_can
arduino-cli monitor -p /dev/cu.usbserial-0001 -c baudrate=115200
```

Replace the example serial port with the one reported by `arduino-cli board
list`. Close the monitor before uploading again so it does not hold the port.

## Safety and scope

Arbitrary CAN messages can activate outputs or change controller state. Start
on the isolated, fused bench bus, keep vehicle drive power disabled, and confirm
the intended CAN ID and payload before transmitting on a motorcycle network.

This is an interactive classic-CAN terminal, not the rusEFI firmware updater.
It can help inspect and reproduce captured frames, but reliable flashing needs
the transport, timing, sequencing, validation, and recovery safeguards in
`v5_can_flasher` / `v5_macos_updater`.
