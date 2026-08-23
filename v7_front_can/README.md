# Ducati front CAN controller — v7

This firmware is the front counterpart to `v5_rear_can`. It runs on an ESP32-S3,
reads the front controls, drives one four-channel relay board plus one separate
dashboard relay, and exchanges the existing lighting commands with the rear/EFI
CAN nodes.

## Behavior

| Label | Function | Input behavior | Local output | CAN behavior |
|---|---|---|---|---|
| A | High beam | Maintained switch by default | 4-board IN1 | None assigned |
| B | Low beam | Maintained switch by default | 4-board IN2 | None assigned |
| C | Left arrow | Toggle on each press | 4-board IN3, blinking | Sends `0x300` |
| D | Right arrow | Toggle on each press | 4-board IN4, blinking | Sends `0x301` |
| E | Dashboard | No local input | Single relay | Receives `0x305` |
| F | Brake | Follows switch position | No local relay | Sends `0x303` |

The recap assigns outgoing CAN only to the left arrow, right arrow, and brake.
High and low beam therefore remain local until CAN IDs are deliberately assigned
for them.

All switch inputs are active LOW with `INPUT_PULLUP`: connect the assigned GPIO
to ESP32 GND to make the input active. Never connect 5 V or motorcycle 12 V to an
ESP32 input.

### Indicator and hazard state machine

There is no separate hazard button:

| Starting state | Press | New state | Frame sent |
|---|---|---|---|
| Both off | Left | Left only | `0x300 01` |
| Left only | Left | Both off | `0x300 00` |
| Both off | Right | Right only | `0x301 01` |
| Right only | Right | Both off | `0x301 00` |
| Left only | Right | Both on, synchronized | `0x304 01` |
| Right only | Left | Both on, synchronized | `0x304 01` |
| Both on | Left | Right only | `0x300 00` |
| Both on | Right | Left only | `0x301 00` |

The front indicator relays share one non-blocking 500 ms blink phase, matching
v5 rear's 500 ms ON / 500 ms OFF timing. Entering the two-sided state sends the
atomic hazard frame, so the rear never sees a transient one-side-only transition.

## CAN protocol

500 kbit/s, standard 11-bit data frames, one Boolean byte:

| CAN ID | Direction | Byte 0 | Meaning |
|---|---|---|---|
| `0x300` | Front → bus | `0x01` / `0x00` | Left indicator on / off |
| `0x301` | Front → bus | `0x01` / `0x00` | Right indicator on / off |
| `0x303` | Front → bus | `0x01` / `0x00` | Brake on / off |
| `0x304` | Front → bus | `0x01` / `0x00` | Both indicators on / off atomically |
| `0x305` | Bus → front | `0x01` / `0x00` | Dashboard relay on / off |

At boot, the dashboard and both front indicators default OFF. Once CAN starts,
v7 sends `0x304 00` to clear stale rear indicator state and publishes the brake
switch's actual boot state. The dashboard stays at its last valid `0x305` state;
there is no timeout because the EFI message cadence has not yet been specified.

## Provisional ESP32-S3 pin map

The CAN pins reuse the ESP32-S3 mapping already documented in this repository's
v3 front hardware. The new control and output pins are provisional until the
exact ESP32-S3 board and finished harness are confirmed.

| Purpose | GPIO |
|---|---:|
| MCP2515 SCK | 6 |
| MCP2515 MOSI / SI | 7 |
| MCP2515 MISO / SO | 14 |
| MCP2515 CS | 16 |
| High-beam input | 47 |
| Low-beam input | 48 |
| Left input | 17 |
| Right input | 18 |
| Brake input | 21 |
| 4-board IN1 / high | 8 |
| 4-board IN2 / low | 9 |
| 4-board IN3 / left | 10 |
| 4-board IN4 / right | 11 |
| Single relay / dashboard | 12 |

The sketch uses the Adafruit MCP2515 software-SPI constructor so the S3 board
package cannot silently substitute default SPI pins. The oscillator setting is
currently 8 MHz to match the verified v5 rear MCP2515. Read the metal oscillator
marking on the new front module before flashing: use `8000000UL` for `8.000`,
`12000000UL` for `12.000`, or `16000000UL` for `16.000`. The upstream Adafruit
library may require an added timing entry for 12 MHz at 500 kbit/s, as documented
under `../v3_relay6/`.

## Relay polarity and input style

The checked-in defaults match v5:

```cpp
constexpr bool RELAYS_ACTIVE_LOW = false;
constexpr bool HEADLIGHT_INPUTS_ARE_MAINTAINED = true;
```

Before connecting loads, test the relay inputs with a current-limited bench
supply. Many four-relay modules are active LOW; if this one is, change
`RELAYS_ACTIVE_LOW` to `true`. If the four-board and single relay have different
polarities, split the setting into two constants before use.

With maintained headlight inputs, closing the switch turns the corresponding
relay on and opening it turns the relay off. If high/low are momentary buttons,
set `HEADLIGHT_INPUTS_ARE_MAINTAINED` to `false`; each press then toggles its
relay.

## Low-voltage wiring and CAN safety

- All button/switch commons go to ESP32 GND.
- Relay `VCC`/`JD-VCC` and input-voltage requirements must be verified for the
  exact modules. Do not assume a nominal "5 V relay board" accepts 3.3 V logic.
- The ESP32, relay-control side, and CAN transceiver need a valid common ground
  reference.
- Connect `CAN_H` to `CAN_H` and `CAN_L` to `CAN_L`.
- The complete physical bus must have exactly two 120-ohm terminators, one at
  each end. With all power removed, the bus should measure about 60 ohms between
  CAN_H and CAN_L.
- Do not connect motorcycle 12 V or lamp current to an ESP32 pin. Route fused
  12 V loads only through correctly rated relay `COM`/`NO` contacts.
- Validate the front module's CAN-transceiver supply voltage; earlier project
  hardware included both 3.3 V SN65HVD230 and 5 V TJA1050 transceivers.

## Build

Install the CAN library once:

```bash
arduino-cli lib install "Adafruit MCP2515"
```

Compile for a generic ESP32-S3 development module:

```bash
cd v7_front_can
arduino-cli compile \
  --fqbn esp32:esp32:esp32s3 \
  v7_front_can.ino
```

For a wired upload, substitute the port reported by `arduino-cli board list`:

```bash
arduino-cli compile --upload \
  --fqbn esp32:esp32:esp32s3 \
  -p /dev/cu.usbmodem-YOUR_PORT \
  v7_front_can.ino
```

Use a 115200-baud serial monitor for input edges, CAN TX/RX, state changes, and
the two-second health heartbeat.
