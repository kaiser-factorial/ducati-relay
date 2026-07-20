# Ducati rear-light CAN controller — v5

This is the consolidated guide for the working v5 bench setup. It covers the rear relay controller, the USB CAN tester, serial commands, wiring, diagnostics, and the changes needed when the tester is replaced by the motorcycle/rusEFI CAN bus.

The two firmware projects remain in their own directories:

- [Rear relay receiver](../v5_rear_can/v5_rear_can.ino)
- [USB CAN sender](../v5_usb_can_sender/v5_usb_can_sender.ino)
- [Receiver-specific notes](../v5_rear_can/README.md)
- [Sender-specific notes](../v5_usb_can_sender/README.md)

## Current working configuration

The bench-tested network has two nodes:

```text
Computer
   |
   | USB serial, 115200 baud
   v
USB ESP32 DevKit -- TWAI --> Waveshare/SN65HVD230 CAN transceiver
                                      |
                                CAN_H / CAN_L
                                      |
ESP32_Relay X4 <-- SPI -- MCP2515/TJA1050 module (8 MHz crystal)
   |
   +-- Relay 1: left indicator
   +-- Relay 2: right indicator
   +-- Relay 3: brake light
```

Both CAN nodes run at **500 kbit/s** and currently exchange standard 11-bit CAN frames. The sender has been tested from both its GPIO18 switch and its USB serial console. Left, right, brake, and the atomic two-indicator hazard command work on the receiver.

The present CAN IDs are a private bench protocol. They are not confirmed Ducati or rusEFI message assignments.

## Rear light and relay wiring

The light assembly has these five wires:

| Light wire | Function | Connection |
|---|---|---|
| Blue | Left indicator | Relay 1 `NO` |
| Orange | Right indicator | Relay 2 `NO` |
| Red | Brake light | Relay 3 `NO` |
| Yellow | Always-on/running lights | Fused, switched +12 V; not relay-controlled in v5 |
| Black | Ground | Battery/chassis negative |

For positive-side relay switching:

1. Feed fused, switched +12 V to `COM_1`, `COM_2`, and `COM_3`.
2. Connect `NO_1` to blue, `NO_2` to orange, and `NO_3` to red.
3. Leave the three `NC` terminals unused.
4. Connect the light's black wire directly to battery/chassis negative.

With this arrangement each light function is off when its relay is idle and receives +12 V when the relay closes. The relay contacts switch the lamp power; the ESP32 board only energizes the relay coils.

Use an appropriately sized fuse and wire for the lamp current. Never connect motorcycle 12 V, a light wire, or a relay `COM`/`NO` terminal to an ESP32 GPIO. Bench-test first with a current-limited supply.

### Relay-board GPIO map

| Function | Relay channel | ESP32 GPIO |
|---|---:|---:|
| Left indicator | 1 | 32 |
| Right indicator | 2 | 33 |
| Brake light | 3 | 25 |
| Unused | 4 | 26 |
| D14 CAN status LED | — | 23 |

The relay inputs on this board are active HIGH. All four relays are forced off at startup.

## CAN module pinouts

### Rear ESP32_Relay X4 to MCP2515/TJA1050

The tested MCP2515 module has an **8.000 MHz crystal** and uses 5 V for its TJA1050 transceiver board.

| MCP2515 module | ESP32_Relay X4 |
|---|---|
| `VCC` | 5 V |
| `GND` | GND |
| `SCK` | GPIO18 |
| `SO` / `MISO` | GPIO19 |
| `SI` / `MOSI` | GPIO13 |
| `CS` | GPIO5 |
| `INT` | Not connected in this firmware |
| `CANH` | CAN_H bus wire |
| `CANL` | CAN_L bus wire |

GPIO13 is intentionally used for MOSI. GPIO23 is not available for MOSI because it drives the relay board's D14 status LED.

This 5 V statement applies to the tested MCP2515/TJA1050 board, not to every CAN module. Do not put a 5 V logic signal directly into an ESP32 GPIO.

### USB ESP32 DevKit to Waveshare/SN65HVD230

| Waveshare CAN module | USB ESP32 DevKit |
|---|---|
| `3.3V` | 3.3 V |
| `GND` | GND |
| `CAN TX` | GPIO21, ESP32 TWAI TX |
| `CAN RX` | GPIO22, ESP32 TWAI RX |
| `CANH` | CAN_H bus wire |
| `CANL` | CAN_L bus wire |

The small Waveshare/SN65HVD230-type module is a transceiver for the ESP32's built-in TWAI controller; it is not an SPI MCP2515 controller.

### CAN bus wiring and termination

- Connect CAN_H to CAN_H and CAN_L to CAN_L.
- Connect the grounds of both ESP/CAN assemblies. A common reference is required even when the boards use separate power supplies.
- Use one 120-ohm termination resistor at each physical end of the bus—exactly two total.
- With all power removed, a correctly terminated two-node bus should measure approximately 60 ohms between CAN_H and CAN_L.
- Keep the bench wires short and preferably use a twisted pair for CAN_H/CAN_L.

The MCP board's J1 jumper enables its termination. Check whether the Waveshare module has fixed or selectable termination before adding another resistor.

## Current CAN protocol

All current messages are standard 11-bit frames at 500 kbit/s with DLC 1. Byte 0 is an explicit requested state, not a toggle instruction: `0x01` means on and `0x00` means off.

| CAN ID | Byte 0 | Receiver action |
|---|---|---|
| `0x300` | `0x01` / `0x00` | Enable / disable left blinking |
| `0x301` | `0x01` / `0x00` | Enable / disable right blinking |
| `0x303` | `0x01` / `0x00` | Brake light on / off |
| `0x304` | `0x01` / `0x00` | Enable / disable both indicators atomically |

Left and right retain independent enabled states. Each enabled indicator blinks with a non-blocking 500 ms on, 500 ms off cycle. The brake output is steady. No `delay()` is used for blinking, so CAN reception, Wi-Fi, BLE provisioning, and OTA servicing continue while the indicators operate.

The separate `0x304` hazard frame is important: it changes both states during one received frame, preventing a transient or state-tracking mismatch between two separate commands.

## Controlling the bench sender over USB serial

Connect the USB ESP32 sender to the computer. Find its port:

```bash
arduino-cli board list
```

Then open the monitor, replacing the example port if needed:

```bash
arduino-cli monitor \
  -p /dev/cu.usbserial-0001 \
  -c baudrate=115200
```

Type a command and press Return:

| Command | Result |
|---|---|
| `l` | Toggle the sender's left state and send explicit left on/off |
| `r` | Toggle the sender's right state and send explicit right on/off |
| `h` | Turn both indicators on if either is off; otherwise turn both off |
| `bon` | Send brake on |
| `boff` | Send brake off |
| `status` | Print tracked light state and TWAI error counters |
| `help` or `?` | Print the command list |

The command is case-insensitive because the firmware converts it to lowercase.

A normal successful transmission produces output similar to:

```text
[TX QUEUED] ID=0x300 DLC=1 data[0]=0x01; awaiting CAN ACK...
[CAN ACK] Frame transmitted and acknowledged by another CAN node.
```

`[TX QUEUED]` only means the ESP32 accepted the message into its transmit queue. `[CAN ACK]` confirms that another active CAN node acknowledged it on the physical bus. A frame can be electrically acknowledged even if its ID is ignored by the receiver application.

The sender uses single-shot frames during diagnostics, so a disconnected or incorrect bus produces one clear error rather than retrying indefinitely. It also prints a heartbeat and controller/error counters every two seconds.

Close the serial monitor before uploading new firmware to the USB sender; otherwise the upload port may be busy.

### Sender's physical test switch

Connect a switch between sender GPIO18 and GND. The internal pull-up is enabled, so no external resistor or positive-voltage connection is required. Each open-to-closed transition toggles the left state and sends its explicit new state. Debouncing is non-blocking.

The sender tracks state locally, while the receiver controls the actual relay state. If one board resets independently during a bench session, their displayed states can differ. Reset both together or send explicit known states before relying on the sender's next toggle.

## D14 receiver diagnostics

| D14 behavior | Meaning |
|---|---|
| Solid on | MCP2515 initialization failed |
| Off | MCP2515 initialized; waiting for traffic |
| Brief 100 ms flash | A CAN frame was received |

D14 flashes for any valid received CAN packet, even one whose ID the lighting code does not use. Its pulse is timer-based and does not block the main loop.

If D14 stays solid, check MCP power, shared ground, CS and SPI pins, and the configured 8 MHz crystal frequency. If D14 flashes but no relay changes, check the CAN ID, standard-versus-extended format, DLC, and payload decoding.

## Building and flashing

Install the MCP2515 library once:

```bash
arduino-cli lib install "Adafruit MCP2515"
```

### Rear receiver

Compile using the OTA-compatible `min_spiffs` partition:

```bash
cd /Users/corinakaiser/Projects/ducati_relay/v5_rear_can
arduino-cli compile \
  --fqbn esp32:esp32:esp32:PartitionScheme=min_spiffs \
  v5_rear_can.ino
```

The rear board retains the v4 BLE Wi-Fi provisioning and ArduinoOTA support. Once it is on the network, upload by hostname:

```bash
arduino-cli upload \
  -p ducati-rear.local \
  --fqbn esp32:esp32:esp32:PartitionScheme=min_spiffs \
  --upload-field password= \
  v5_rear_can.ino
```

If mDNS does not resolve, use its current IP address in place of `ducati-rear.local`. A first wired upload requires an appropriate USB-to-serial/programming connection for this board.

### USB sender

```bash
cd /Users/corinakaiser/Projects/ducati_relay/v5_usb_can_sender
arduino-cli compile --upload \
  --fqbn esp32:esp32:esp32 \
  -p /dev/cu.usbserial-0001 \
  v5_usb_can_sender.ino
```

Replace the port with the result from `arduino-cli board list`.

## Relay-only test mode

Before adding CAN hardware, the receiver can directly test its relays and lamp wiring. In [v5_rear_can.ino](../v5_rear_can/v5_rear_can.ino), set:

```cpp
constexpr bool TEST_MODE = true;
```

Then connect temporary switches to ground:

| Test switch | Connection | Result while held |
|---|---|---|
| Left | GPIO18 to GND | Left blinks |
| Right | GPIO19 to GND | Right blinks |
| Brake | GPIO13 to GND | Brake stays on |

The internal pull-ups are enabled. Never connect these test inputs to 5 V or 12 V.

GPIO18, GPIO19, and GPIO13 become the MCP2515 SPI pins in CAN mode. **Remove all three temporary switches**, set `TEST_MODE = false`, and reflash before connecting the MCP2515. CAN is deliberately disabled while test mode is true.

## Changing CAN messages for rusEFI

There are two distinct migration cases.

### Case 1: keep a custom front controller

If another ESP32 continues to read the motorcycle buttons and sends lighting commands, the existing private protocol can remain. Choose unused IDs, then change the same four constants near the top of **both** sketches:

```cpp
constexpr uint32_t CAN_LEFT_ID   = 0x300;
constexpr uint32_t CAN_RIGHT_ID  = 0x301;
constexpr uint32_t CAN_BRAKE_ID  = 0x303;
constexpr uint32_t CAN_HAZARD_ID = 0x304;
```

When using the tester, sender and receiver must agree. Changing only one end will still produce CAN ACKs, but the receiver will ignore the unfamiliar IDs.

### Case 2: decode an existing rusEFI/Ducati frame

If rusEFI already broadcasts the relevant state, the USB sender is removed from the final system. The receiver must decode the real frame rather than merely substituting a guessed ID.

For each desired signal, determine:

1. The CAN bitrate used on that bus.
2. Whether the frame uses an 11-bit standard or 29-bit extended ID.
3. The frame ID and expected DLC.
4. Which byte and bit contain the state.
5. Whether active means bit set or bit clear.
6. Whether the value is a bit, integer, counter, or scaled multi-byte field.
7. The normal message rate and a safe stale-message timeout.

Capture traffic while changing only one input—for example, operate the brake lever repeatedly—and compare frames to identify the field. Do not assume that a frame seen during braking is a brake command; engine speed, counters, checksums, and other periodic data may change at the same time.

Once identified, edit `handleCanFrame()` in the receiver. A single-bit brake field might look like this:

```cpp
constexpr uint32_t CAN_BRAKE_ID = 0x456; // example only
constexpr uint8_t BRAKE_BYTE = 2;
constexpr uint8_t BRAKE_MASK = 0x04;

// Inside the CAN_BRAKE_ID case, after verifying the received length:
if (length > BRAKE_BYTE) {
  brakeEnabled = (data[BRAKE_BYTE] & BRAKE_MASK) != 0;
  stateChanged = true;
}
```

If the signal is active-low, invert the expression. If it spans multiple bytes, confirm the byte order before combining them. Preserve bounds checks so a short frame cannot read past the received payload.

The current receiver rejects extended and remote frames here:

```cpp
if (extended || remote || packetSize < 1 || length < 1) {
  return;
}
```

That is correct for the bench protocol. It must be intentionally revised if the verified rusEFI signal uses a 29-bit extended frame.

Before selecting custom IDs on a rusEFI network, inspect the actual configuration and CAN documentation. Common rusEFI traffic may occupy IDs around `0x100`, `0x102`, `0x190`, `0x200`–`0x20B`, `0x667`, and `0x7E1`; these examples are a warning to verify, not a complete reserved-ID list. Avoid collisions with every ECU, dashboard, wideband, bootloader, and aftermarket node on the motorcycle.

Turn-button states may not be native rusEFI signals at all. If a front ESP32 reads the handlebar switches, retaining dedicated custom left/right/hazard messages is often simpler, while only the brake state is decoded from an ECU frame.

### Changing the bus speed

On the rear receiver, change:

```cpp
constexpr uint32_t CAN_BITRATE = 500000;
```

Keep this separate setting correct for the MCP module's oscillator:

```cpp
constexpr uint32_t MCP2515_CLOCK_HZ = 8000000UL;
```

The oscillator frequency is not the CAN bitrate. Selecting the wrong crystal value causes incorrect CAN timing even if `CAN_BITRATE` appears correct.

On the USB TWAI sender, replace:

```cpp
const twai_timing_config_t timingConfig = TWAI_TIMING_CONFIG_500KBITS();
```

with the matching ESP-IDF TWAI timing preset or a validated custom timing configuration. Every active node on a CAN bus must use the same nominal bitrate.

## Troubleshooting checklist

| Symptom | Check |
|---|---|
| Receiver D14 stays solid | MCP 5 V/GND, SPI pin order, CS GPIO5, 8 MHz clock setting |
| Sender reports `TX QUEUED` but no `CAN ACK` | Other node powered, shared ground, matching 500 kbit/s, CAN_H/CAN_L, termination |
| Sender reaches bus-off | Fix physical bus, then power-cycle the sender |
| D14 flashes but lights do nothing | `TEST_MODE` must be false; verify ID, DLC, byte value, and relay/load wiring |
| CAN ACK appears but command is ignored | ACK is electrical only; confirm sender and receiver protocol constants match |
| Resistance is about 120 ohms | Only one terminator is present |
| Resistance is about 40 ohms | Three terminators are present |
| Resistance is about 60 ohms | Expected for two 120-ohm terminators in parallel |
| Serial upload cannot open port | Close `arduino-cli monitor` or any other serial application |
| Hazard affects only one output | Use the atomic `0x304` hazard frame and the current receiver firmware |
| Sender's next toggle seems backwards | Its locally tracked state was desynchronized; reset both nodes or send explicit states |

Resistance checks must be made with the entire bus powered off. A 60-ohm measurement verifies termination, not bitrate, polarity, signal integrity, module power, or application-level decoding.

## Before installing on the motorcycle

- Confirm actual rear-light voltage, current, and wire functions.
- Fuse the lamp supply and secure/insulate every connection.
- Confirm the motorcycle CAN bitrate and physical-layer compatibility.
- Verify chosen custom IDs do not collide with existing traffic.
- Add an appropriate stale-data/fail-safe policy for any continuously broadcast ECU signal.
- Confirm all relays default off through startup, Wi-Fi provisioning, OTA, and CAN faults.
- Test left, right, hazards, brake, and simultaneous brake-plus-indicator operation on the bench.
- Keep the USB tester disconnected from the final bike unless it is deliberately configured as a third CAN node with correct termination.

The remaining vehicle-specific work is to capture and validate the actual rusEFI/Ducati frames. Until that is done, `0x300`, `0x301`, `0x303`, and `0x304` should be treated only as the proven bench-test protocol.
