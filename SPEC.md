# Ducati Relay — Project Spec

## Overview

This project adds a custom CAN bus control layer to a friend's **Ducati Scrambler 800** running a
[rusEFI](https://rusefi.com) aftermarket ECU (**microRusEFI v6**). Two ESP32 microcontrollers (with MCP2515 SPI CAN
controller modules) sit on the same CAN bus as the ECU, allowing button-driven relay control
of bike functions. ESP-A (front) aggregates data from all nodes and drives a front dashboard
display.

---

## Why We're Building This

The friend's Ducati has a rusEFI ECU that exposes a CAN bus. The goal is to extend that bus
with custom hardware to:

1. **Control** bike functions (ignition, starter, lights, turn signals, stereo, brake light)
   via physical buttons wired to the ESPs.
2. **Display** real-time telemetry on a front dashboard, driven by ESP-A which listens to
   all nodes on the bus (ECU telemetry + ESP-B status broadcasts).

This gives a clean, modular wiring architecture that doesn't hack the stock harness directly —
all switching happens through relays that the ESPs drive over CAN.

---

## Phase 1: Practice Rig (COMPLETE)

Before touching the bike, we built and validated a full CAN-bus + relay circuit on the bench.

### Hardware

| Component | Count | Notes |
|-----------|-------|-------|
| ESP32 DevKit v1 | 2 | "ESP-A" (sender) and "ESP-B" (receiver) |
| MCP2515 SPI CAN controller module | 2 | One per ESP; default 16MHz crystal |
| 5-pin relay modules | 3 | On ESP-B; active-HIGH (`RELAY_ACTIVE_LOW false`) |
| Tactile pushbuttons | 3 | On ESP-A; internal pull-up, connect pin → GND when pressed |
| 120Ω resistors | 2 | CAN termination, one at each end of the bus |

### Wiring

**MCP2515 → ESP32 (both boards, same VSPI pins):**

| MCP2515 pin | ESP32 GPIO |
|-------------|-----------|
| SCK | 18 |
| MISO (SO) | 19 |
| MOSI (SI) | 23 |
| CS | 5 |
| VCC | 3.3V |
| GND | GND |

**ESP-A buttons (INPUT_PULLUP; press connects pin to GND):**

| Button | GPIO |
|--------|------|
| Button 1 | 27 |
| Button 2 | 25 |
| Button 3 | 26 |

**ESP-B relay outputs:**

| Relay | GPIO | CAN ID |
|-------|------|--------|
| Relay 1 | 4 | 0x100 |
| Relay 2 | 32 | 0x101 |
| Relay 3 | 33 | 0x102 |

> **Note:** GPIO 2 on ESP-B is reserved for the onboard status LED (see below). Relay 1 is on
> GPIO 4, not GPIO 2. If the signal wire for Relay 1 is ever moved back to GPIO 2 by mistake,
> the relay won't activate even though the software state will appear correct.

**CAN bus:**

```
[ESP-A MCP2515] ─── CAN_H / CAN_L ─── [ESP-B MCP2515]
    [120Ω]                                  [120Ω]
```

Common GND must be shared between ESP-A GND and ESP-B GND (run a wire between them).
Load-side ground (battery negative) is isolated on the relay's switched side.

### CAN Protocol (practice rig)

- **Baud rate:** 500 kbps
- **Frame type:** standard (11-bit ID)
- **Payload:** 1 byte — `0x01` = pressed/ON, `0x00` = released/OFF

| CAN ID | Meaning |
|--------|---------|
| 0x100 | Button 1 / Relay 1 |
| 0x101 | Button 2 / Relay 2 |
| 0x102 | Button 3 / Relay 3 |

### ESP-B Status LED

GPIO 2 (onboard LED) reflects the active relay state since it's a single-color LED:

| State | Pattern |
|-------|---------|
| No relay active | Off |
| Relay 1 active | Solid on |
| Relay 2 active | Slow flash (500ms half-period) |
| Relay 3 active | Rapid flash (100ms half-period) |

Lowest-numbered active relay wins if multiple are on. Pattern is updated non-blockingly in
`loop()` using `millis()`-based timing — no `delay()` used.

### Firmware

- **Library:** Adafruit MCP2515 (`arduino-cli lib install "Adafruit MCP2515"`)
- **Board target:** `esp32:esp32:esp32`
- **Serial baud:** 115200
- Both sketches include heavy debug logging: boot banner, register dump, per-event logs,
  2-second heartbeat with running packet counts.

**Sketch locations:**

```
ducati_relay/v1/
├── esp32_a_button_sender/
│   └── esp32_a_button_sender.ino
└── esp32_b_relay_receiver/
    └── esp32_b_relay_receiver.ino
```

### Key Lessons Learned

- **Termination resistors are mandatory.** Without 120Ω at both ends, CAN_H/CAN_L will read
  ~24kΩ (open) instead of ~60Ω and no packets will be received. Add one resistor bridging
  CAN_H and CAN_L at each transceiver module.
- **Common ground between nodes is required.** CAN is differential but the transceivers need
  a shared voltage reference.
- **MCP2515 crystal frequency matters.** The Adafruit library defaults to 16MHz. If the
  oscillator on your module is marked 8MHz, call `mcp.setClockFrequency(8e6)` before
  `mcp.begin()` (toggle via `#define MCP2515_CRYSTAL_8MHZ` at the top of each sketch).
- **GPIO 2 is the onboard LED on most ESP32 DevKit boards.** Driving it as a relay output
  produces unexpected LED behavior. Use GPIO 4 (or any other free GPIO) for relay signals.
- **Relay coil inrush can brownout the ESP32** if the board isn't externally powered. Make sure
  the power supply (battery or bench supply) is on before testing relay activation.

---

## Phase 1.5: Extended Practice Rig

Builds on v1. The goal of this phase is to make cross-board CAN communication visible and
tangible — modelling how an ECU would sit as the single source of truth in the middle.

Key design choices:

- **CAN is the single source of truth.** ESP-A relays do not fire on button press. They only
  fire after ESP-B sends back a confirmation over CAN.
- **Ch1 (GPIO 32) is one-way.** ESP-A sends 0x100 → ESP-B fires relay 1 + buzzer. No response.
  ESP-A's Ch1 relay is instead driven by ESP-B's push button (0x110) — each board controls
  something on the other board.
- **Ch2–4 are request/response.** ESP-A sends a request → ESP-B fires its relay and sends a
  confirmation back → ESP-A fires its local relay only on receipt of that confirmation.
- **Onboard LED (GPIO 2) on each board** blinks briefly on every incoming CAN message, making
  cross-board traffic visible at a glance.
- **Passive piezo buzzer on ESP-B** (GPIO 13) beeps when relay 1 fires.
- **SVG wiring diagrams** for both boards are included at the repo root:
  `relay_wiring_front.svg` (ESP-A) and `relay_wiring_rear.svg` (ESP-B).

### CAN Protocol (v1.5)

- **Baud rate:** 500 kbps
- **Frame type:** standard (11-bit ID)
- **Payload:** 1 byte — `0x01` = pressed/ON, `0x00` = released/OFF

| CAN ID | Direction | Meaning |
|--------|-----------|---------|
| 0x100 | ESP-A → ESP-B | Ch1 button event (one-way; triggers relay 1 + buzzer on ESP-B) |
| 0x101 | ESP-A → ESP-B | Ch2 button request |
| 0x102 | ESP-A → ESP-B | Ch3 button request |
| 0x103 | ESP-A → ESP-B | Ch4 button request |
| 0x110 | ESP-B → ESP-A | ESP-B push button event (triggers Ch1 relay on ESP-A) |
| 0x111 | ESP-B → ESP-A | Ch2 confirmation (mirrors request payload) |
| 0x112 | ESP-B → ESP-A | Ch3 confirmation |
| 0x113 | ESP-B → ESP-A | Ch4 confirmation |

### Board Pin Mapping

Pin order matches the physical ESP32 DevKit v1 header, top → bottom on each side.
*Italics* on an unused pin indicate a hardware constraint worth knowing.

**Left header:**

| # | Pin | ESP-A v1.5 | ESP-B v1.5 |
|---|-----|------------|------------|
| 1 | 3V3 | MCP2515 VCC | MCP2515 VCC |
| 2 | EN | — | — |
| 3 | GPIO 36 (SVP) | — *input only* | — *input only* |
| 4 | GPIO 39 (SVN) | — *input only* | — *input only* |
| 5 | GPIO 34 | — *input only, no internal pull-up* | — *input only, no internal pull-up* |
| 6 | GPIO 35 | — *input only, no internal pull-up* | — *input only, no internal pull-up* |
| 7 | GPIO 32 | Button 1 | Relay 2 (CAN 0x101) |
| 8 | GPIO 33 | Button 2 | Relay 3 (CAN 0x102) |
| 9 | GPIO 25 | Button 3 | Relay 4 (CAN 0x103) |
| 10 | GPIO 26 | Button 4 | push button (sends CAN 0x110) |
| 11 | GPIO 27 | Relay 4 | — |
| 12 | GPIO 14 | Relay 3 | — |
| 13 | GPIO 12 | — *strapping — HIGH at boot = 1.8V flash voltage* | — *strapping — HIGH at boot = 1.8V flash voltage* |
| 14 | GND | common GND | common GND |
| 15 | GPIO 13 | — | buzzer (piezo signal) |
| 16 | GPIO 9 (SD2) | — *internal flash — do not use* | — *internal flash — do not use* |
| 17 | GPIO 10 (SD3) | — *internal flash — do not use* | — *internal flash — do not use* |
| 18 | GPIO 11 (CMD) | — *internal flash — do not use* | — *internal flash — do not use* |
| 19 | 5V | — | — |

**Right header:**

| # | Pin | ESP-A v1.5 | ESP-B v1.5 |
|---|-----|------------|------------|
| 1 | GND | common GND | common GND |
| 2 | GPIO 23 | MCP2515 MOSI | MCP2515 MOSI |
| 3 | GPIO 22 | — | — |
| 4 | GPIO 1 (TX) | — *UART0 TX; used by Serial* | — *UART0 TX; used by Serial* |
| 5 | GPIO 3 (RX) | — *UART0 RX; used by Serial* | — *UART0 RX; used by Serial* |
| 6 | GPIO 21 | — | — |
| 7 | GND | — | — |
| 8 | GPIO 19 | MCP2515 MISO | MCP2515 MISO |
| 9 | GPIO 18 | MCP2515 SCK | MCP2515 SCK |
| 10 | GPIO 5 | MCP2515 CS | MCP2515 CS |
| 11 | GPIO 17 | Relay 1 | — |
| 12 | GPIO 16 | Relay 2 | — |
| 13 | GPIO 4 | — | Relay 1 (CAN 0x100) |
| 14 | GPIO 0 | — *strapping — LOW at boot = download mode* | — *strapping — LOW at boot = download mode* |
| 15 | GPIO 2 | CAN receive LED *strapping pin + onboard LED* | CAN receive LED *strapping pin + onboard LED* |
| 16 | GPIO 15 | — *strapping — LOW at boot silences UART log* | — *strapping — LOW at boot silences UART log* |
| 17 | GPIO 8 (SD1) | — *internal flash — do not use* | — *internal flash — do not use* |
| 18 | GPIO 7 (SD0) | — *internal flash — do not use* | — *internal flash — do not use* |
| 19 | GPIO 6 (CLK) | — *internal flash — do not use* | — *internal flash — do not use* |

---

## Phase 2: Bike Firmware (IN PROGRESS)

### Topology

```
[Display]
    |
[ESP-A] ────────── [rusEFI ECU] ────────── [ESP-B]
 (front)        single shared CAN bus        (rear)
 [120Ω]                                     [120Ω]
```

All three nodes share **one CAN bus** (CAN_H / CAN_L, 120Ω termination at each physical end).
ESP-A is at the front of the bike and drives the dashboard display directly (wired to ESP-A,
not a separate CAN node). ESP-A listens to all traffic on the bus — ECU telemetry and ESP-B
status — and pushes relevant data to the display.

### Node Roles

| Node | Physical location | Role |
|------|------------------|------|
| ESP-A | Front of bike / handlebars | Reads rider input buttons; sends CAN commands; drives front relays; listens to all CAN traffic and drives front dashboard display |
| microRusEFI v6 ECU | Engine bay | Engine management; broadcasts telemetry (RPM, TPS, coolant temp, etc.); receives brake sensor input and relays it over CAN |
| ESP-B | Rear of bike | Receives CAN commands from ESP-A; drives rear relays; broadcasts rear status |

### Implemented Functions

#### ESP-A buttons and outputs

ESP-A reads 7 buttons. Four control local front relays directly; three send CAN commands
to ESP-B. Pressing the power button while outputs are active cancels all relays and CAN
commands (ignition-off cascade).

| Button | GPIO | Behavior | Controls |
|--------|------|----------|----------|
| Power | 27 | Toggle | Relay A1 (ignition). Turning OFF cascades: cancels all other relays and CAN commands. |
| Starter | 25 | Momentary (hold to crank) | Relay A2 (starter). Blocked if ignition relay is OFF. |
| Headlight low beam | 26 | Toggle | Relay A3 |
| Headlight high beam | 14 | Toggle | Relay A4 |
| Left turn signal | 13 | Toggle | CAN 0x300 → ESP-B. Cancels right turn if active. |
| Right turn signal | 17 | Toggle | CAN 0x301 → ESP-B. Cancels left turn if active. |
| Stereo | 22 | Toggle | CAN 0x302 → ESP-B |

**ESP-A front relay GPIO assignments:**

| Relay | GPIO | Switches |
|-------|------|----------|
| A1 | 4 | Ignition / bike power rail |
| A2 | 32 | Starter motor circuit |
| A3 | 33 | Headlight low beam |
| A4 | 16 | Headlight high beam |

> GPIO confirmation with friend still required before flashing to the actual bike.

#### ESP-B relay outputs

ESP-B receives CAN commands from ESP-A and drives 4 rear relays. Turn signals flash at
~60 BPM (500 ms half-period) in firmware. Brake light relay is wired up but disabled in
firmware until the ECU broadcast CAN ID is confirmed.

| Relay | GPIO | CAN ID | Switches | Flash |
|-------|------|--------|----------|-------|
| B1 | 4 | 0x300 | Left turn signal | Yes — 500ms half-period |
| B2 | 32 | 0x301 | Right turn signal | Yes — 500ms half-period |
| B3 | 33 | TBD (ECU) | Brake light | No |
| B4 | 25 | 0x302 | Stereo / accessory power | No |

> GPIO confirmation with friend still required before flashing to the actual bike.

#### Dashboard data (ESP-A as aggregator)

ESP-A listens to all CAN traffic and pushes data to the front dashboard display (display is
wired directly to ESP-A, not a separate CAN node). Data sources ESP-A will consume:

- rusEFI: RPM, throttle position, coolant temperature, battery voltage, gear position
- ESP-B: active relay states, any rear sensor data it broadcasts
- ESP-A itself: active button states, ignition state

### CAN ID Plan

The practice rig uses 0x100–0x102, but those IDs conflict with rusEFI — see below.

**Confirmed rusEFI native IDs (do not use):**

| ID(s) | Owner | Purpose |
|-------|-------|---------|
| 0x100, 0x102 | rusEFI | TunerStudio-over-CAN (active during tuning sessions) |
| 0x190 | rusEFI | Wideband O2 (WBO) sensor communication |
| 0x200–0x20B+ | rusEFI | Verbose telemetry broadcast (see section below) |
| 0x667 | rusEFI | OpenBLT bootloader TX |
| 0x7E1 | rusEFI | OpenBLT bootloader RX |

**Custom ID allocation:**

| ID | Owner | Purpose |
|----|-------|---------|
| 0x130 | ESP-A | Relay status broadcast (1-byte bitmask, bits 0–3 = A1–A4) |
| 0x160 | ESP-B | Relay logical status broadcast (1-byte bitmask, bits 0–3 = B1–B4) |
| 0x300 | ESP-A | Left turn signal command → ESP-B (0x01=ON, 0x00=OFF) |
| 0x301 | ESP-A | Right turn signal command → ESP-B (0x01=ON, 0x00=OFF) |
| 0x302 | ESP-A | Stereo command → ESP-B (0x01=ON, 0x00=OFF) |

### rusEFI Verbose CAN Broadcast

rusEFI broadcasts a rich telemetry stream when "CAN broadcast" is enabled in TunerStudio.
The base address defaults to **0x200** and is configurable via `verboseCanBaseAddress` —
verify the friend's ECU setting before relying on these IDs.

All multi-byte values are **little-endian**. Payloads are 8 bytes each.

| CAN ID | Name | Signal | Bytes | Decode |
|--------|------|--------|-------|--------|
| 0x200 | BASE0 | Gear | 5 | `buf[5]` → gear number |
| | | Status flags | 0–4 | rev limit, fuel pump, CEL, fans, etc. |
| 0x201 | BASE1 | **RPM** | 0–1 | `(buf[1]<<8)\|buf[0]` → RPM |
| | | Vehicle speed | 6 | kph |
| 0x202 | BASE2 | **TPS1** | 2–3 | `((buf[3]<<8)\|buf[2]) × 0.01` → % |
| 0x203 | BASE3 | **Coolant temp** | 2 | `buf[2] − 40` → °C |
| | | Intake temp | 3 | `buf[3] − 40` → °C |
| 0x204 | BASE4 | **Battery voltage** | 6–7 | `((buf[7]<<8)\|buf[6]) × 0.001` → V |

BASE5–BASE11 carry EGT, knock, lambda, cam timing, and other channels not needed
for the dashboard. The full signal definitions are in
[`rusEFI_CAN_verbose.dbc`](https://github.com/rusefi/rusefi/blob/master/firmware/controllers/can/rusEFI_CAN_verbose.dbc)
in the rusEFI repo.

> **Brake light via ECU:** The brake lever sensor signal reaches the ECU but its CAN
> broadcast ID is not in BASE0–BASE4. It likely requires either a custom Lua script on the
> ECU or locating the signal in BASE5+. This is still TBD — see Open Questions.

---

## Open Questions / TBD

### Must confirm before writing bike firmware

- [ ] **CAN baud rate**: rusEFI defaults to 500 kbps (matches our practice rig) but confirm
      this matches the actual ECU configuration before flashing anything.
- [x] **rusEFI CAN ID map**: confirmed — telemetry on 0x200–0x204 (verbose CAN, base
      configurable), TunerStudio over CAN on 0x100/0x102, WBO on 0x190. Custom IDs moved
      to 0x300–0x302 to avoid all conflicts. Verify `verboseCanBaseAddress` in the friend's
      TunerStudio config is the default 0x200.
- [ ] **Starter interlock logic**: does the ECU or ESP-A need to verify a safety condition
      (e.g. neutral gear signal, clutch lever) before sending the start command? Current
      firmware only checks ignition ON. Cranking without a proper interlock is a safety hazard.
- [ ] **Brake light CAN ID**: the brake lever sensor signal is received by the ECU but its
      CAN broadcast ID is not in the standard BASE0–BASE4 messages. Determine whether rusEFI
      broadcasts it in BASE5+ or whether a Lua script on the ECU is needed. Until resolved,
      the B3-Brake relay on ESP-B is disabled in firmware.

### Still to confirm with friend

- [ ] **Exact relay assignments**: which physical circuits on the Ducati does each relay
      switch, and what are the safe switching voltages/currents?
- [x] **Turn signal flash pattern**: implemented in software — ESP-B flashes at 500ms
      half-period (~60 BPM) while logical state is ON. No hardware RC timer needed.
- [ ] **Dashboard display type**: what hardware is the front display? (e.g. SPI/I2C OLED or
      TFT, dedicated CAN gauge module, etc.) Blocks display rendering code in ESP-A.
- [ ] **rusEFI termination resistor**: does the ECU board have a built-in 120Ω termination
      that can be enabled? If yes, only one external resistor is needed (at the far physical
      end of the cable run, which will be one of the ESPs).
- [ ] **Physical routing / cable run order**: which node is at each end of the harness?
      (Determines which two nodes get the 120Ω termination resistors.)

---

## Build & Flash (arduino-cli)

### First-time setup

```bash
# Install ESP32 platform
arduino-cli core update-index
arduino-cli core install esp32:esp32

# Install CAN library
arduino-cli lib install "Adafruit MCP2515"
```

### Find the board's port

Plug in the ESP32, then:

```bash
arduino-cli board list
```

Look for `/dev/cu.usbserial-XXXX` or `/dev/cu.SLAB_USBtoUART` on Mac. Run before and after
plugging in if unsure which port is the board.

### Compile

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 v1.5/esp32_a_button_sender
arduino-cli compile --fqbn esp32:esp32:esp32 v1.5/esp32_b_relay_receiver
```

### Upload

```bash
arduino-cli upload -p /dev/cu.usbserial-XXXX --fqbn esp32:esp32:esp32 v1.5/esp32_a_button_sender
```

### Monitor serial output

```bash
arduino-cli monitor -p /dev/cu.usbserial-XXXX --config baudrate=115200
```

`Ctrl+C` to exit. Both sketches boot at 115200 baud and print a banner, register dump,
per-event logs, and a 2-second heartbeat.

### Compile + upload in one step

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 v1.5/esp32_a_button_sender && \
arduino-cli upload -p /dev/cu.usbserial-XXXX --fqbn esp32:esp32:esp32 v1.5/esp32_a_button_sender
```

> **Note:** The FQBN `esp32:esp32:esp32` targets the standard 30-pin ESP32 DevKit v1. If
> `board list` shows a different variant name, adjust accordingly.

---

## Repository Layout

```
ducati_relay/
├── SPEC.md
├── v1/
│   ├── esp32_a_button_sender/
│   │   └── esp32_a_button_sender.ino    ← practice rig: 3-button CAN sender
│   └── esp32_b_relay_receiver/
│       └── esp32_b_relay_receiver.ino  ← practice rig: 3-relay CAN receiver + status LED
├── v1.5/
│   ├── esp32_a_button_sender/
│   │   └── esp32_a_button_sender.ino   ← 4 buttons; relays fire on CAN response from ESP-B
│   └── esp32_b_relay_receiver/
│       └── esp32_b_relay_receiver.ino  ← 4 relays + push button + piezo buzzer; request/response CAN
├── relay_wiring_front.svg              ← ESP-A relay load wiring diagram
├── relay_wiring_rear.svg               ← ESP-B relay load wiring diagram
└── v2/
    ├── esp32_a_bike/
    │   └── esp32_a_bike.ino            ← bike: 7 buttons, 4 front relays, ECU telemetry rx
    └── esp32_b_bike/
        └── esp32_b_bike.ino            ← bike: 4 rear relays, turn-signal flash, status tx
```
