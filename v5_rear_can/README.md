# Ducati rear lighting controller — v5

This firmware runs on the v4 **ESP32_Relay X4** board and drives three relay channels from
CAN messages. It keeps the v4 BLE provisioning and ArduinoOTA support.

## Relay test mode (before installing CAN)

Set `TEST_MODE = true` when using this mode. In test mode the MCP2515 is disabled,
and three temporary switches directly test the relays and light wiring:

| Temporary switch | ESP32 connection | Result while closed |
|---|---|---|
| Left | GPIO 18 to GND | Relay 1 held ON |
| Right | GPIO 19 to GND | Relay 2 held ON |
| Brake | GPIO 13 to GND | Relay 3 held ON |

No external pull-up resistor is needed; the firmware enables the ESP32's internal pull-ups.
An open switch means OFF, and closing it to GND means ON. Multiple switches may be closed at
the same time. While held closed, the left and right switches blink their respective relays
with the same non-blocking 500 ms half-period used in CAN mode. The brake relay remains steady.

GPIO 18, 19, and 13 are also the future MCP2515 SPI pins. **Disconnect all three test switches
before installing the MCP2515.** Then change this line in `v5_rear_can.ino` and reflash:

```cpp
constexpr bool TEST_MODE = false;
```

The checked-in firmware is now set to `false` for CAN bench testing.

Do not connect a test switch to +12 V or +5 V. Each switch connects only its assigned GPIO
to an ESP/relay-board GND terminal.

## CAN protocol

500 kbit/s, standard 11-bit CAN frames:

| CAN ID | Payload byte 0 | Action |
|---|---|---|
| `0x300` | `0x01` / `0x00` | Set left indicator ON / OFF |
| `0x301` | `0x01` / `0x00` | Set right indicator ON / OFF |
| `0x303` | `0x01` / `0x00` | Brake ON / OFF |
| `0x304` | `0x01` / `0x00` | Set both indicators ON / OFF atomically |

Left and right are independent. ID `0x304` changes both together in one CAN frame.
The blink period is 500 ms ON and 500 ms OFF. All relays default OFF at boot.

These IDs describe the custom controller-to-controller protocol; they are not claimed to be
the Ducati/rusEFI's native brake-switch message. The actual ECU brake frame still needs to be
captured and decoded. Once known, update `CAN_BRAKE_ID` and, if necessary, the payload decode
in `handleCanFrame()`.

## Low-voltage control wiring

| MCP2515 | ESP32 relay board |
|---|---|
| SCK | GPIO 18 |
| SO / MISO | GPIO 19 |
| SI / MOSI | GPIO 13 |
| CS | GPIO 5 |
| GND | GND |
| VCC | Match the specific module's requirements |

The CAN transceiver and ESP32 must share a ground reference. Connect `CAN_H` to `CAN_H` and
`CAN_L` to `CAN_L`. The whole bus should have exactly two 120-ohm terminators, one at each
physical end; measure about 60 ohms between CAN_H and CAN_L with all power removed.

The firmware is configured for the verified `8.000` MHz MCP2515 crystal. The earlier
NiRen/TJA1050 module in this project required
5 V for its transceiver; verify the exact module before powering it. Never put 5 V logic
directly into an ESP32 GPIO.

GPIO 23 drives the board's D14 status LED and is deliberately left out of the CAN wiring.
Using GPIO13 for MOSI avoids loading the SPI signal through that onboard LED.

### D14 CAN diagnostics

| D14 behavior | Meaning |
|---|---|
| Solid on | MCP2515 initialization failed; check VCC, GND, CS/SPI wiring, and clock setting |
| Off | MCP2515 initialized and is waiting for traffic |
| Brief 100 ms flash | A CAN frame was received (including an unrelated CAN ID) |

The receive flash uses `millis()` and never blocks CAN, Wi-Fi, or OTA processing.

## Relay/load wiring

| Light wire | Function | Relay channel | ESP32 GPIO |
|---|---|---|---|
| Blue | Left indicator | Relay 1 | 32 |
| Orange | Right indicator | Relay 2 | 33 |
| Red | Brake | Relay 3 | 25 |
| Yellow | Always-on/running lights | Not controlled by relay | — |
| Black | Light ground | Battery/chassis negative | — |

Use the relay's isolated screw contacts for the 12 V load side. A typical positive-side
switching arrangement is fused/switched +12 V into each relay `COM`, with `NO` going to the
corresponding blue, orange, or red light wire. Connect black to negative. Connect yellow only
to the intended fused/switched running-light +12 V after verifying the light manufacturer's
voltage and current specification.

Do not connect any light wire or motorcycle 12 V directly to an ESP32 GPIO. Bench-test the
logic with a current-limited supply before connecting it to the motorcycle harness.

## Build and flash

Install the CAN library once:

```bash
arduino-cli lib install "Adafruit MCP2515"
```

Compile with the same classic ESP32 target and OTA-compatible partition used by v4:

```bash
cd v5_rear_can
arduino-cli compile \
  --fqbn esp32:esp32:esp32:PartitionScheme=min_spiffs \
  v5_rear_can.ino
```

For the first wired upload, replace the port as needed:

```bash
arduino-cli compile --upload \
  --fqbn esp32:esp32:esp32:PartitionScheme=min_spiffs \
  -p /dev/cu.usbserial-BG03U2R7 \
  v5_rear_can.ino
```

For OTA after the board is connected to Wi-Fi:

```bash
arduino-cli upload \
  -p ducati-rear.local \
  --fqbn esp32:esp32:esp32:PartitionScheme=min_spiffs \
  --upload-field password= \
  v5_rear_can.ino
```
