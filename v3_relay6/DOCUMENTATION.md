# Ducati CAN Relay System (v3)

## Overview
This project involves building a custom CAN bus relay system for a Ducati motorcycle. The system uses two ESP32 boards communicating over a CAN bus:
* **`esp_relay` (Transmitter)**: An ESP32-S3 with a Waveshare 6-channel relay CAN Hat. This board reads physical button presses and broadcasts them.
* **`esp_b` (Receiver)**: An ESP32 paired with a NiRen MCP2515 breakout board. This board listens for CAN messages and will eventually trigger physical relays (currently mapped to blink the onboard blue LED for testing).

## Hardware Specifications

### Transmitter (`esp_relay`)
* **Microcontroller**: ESP32-S3
* **CAN Controller**: MCP2515
* **CAN Transceiver**: SN65HVD230 (3.3V Logic)
* **Crystal Oscillator**: 12MHz (`EAS12.000`)
* **SPI Pins**: `CS=16`, `MOSI=7`, `MISO=14`, `SCK=6`

### Receiver (`esp_b`)
* **Microcontroller**: ESP32
* **CAN Controller**: MCP2515
* **CAN Transceiver**: TJA1050 (Requires 5V Power)
* **Crystal Oscillator**: 8MHz
* **SPI Pins**: `CS=5`, `MOSI=23`, `MISO=19`, `SCK=18`

---

## The Debugging Journey & Solutions

Getting these two boards to communicate flawlessly required solving a cascading series of hardware and software issues:

### 1. The 12MHz vs 8MHz Clock Mismatch
* **The Problem**: The initial code assumed both boards were using 8MHz crystals. Because the `esp_relay` Hat actually had a 12MHz crystal, requesting a 500kbps baud rate resulted in it physically transmitting at 750kbps. This meant the two boards were speaking at entirely different speeds and dropping all packets.
* **The Fix**: Updated the Arduino sketches to explicitly pass the exact physical crystal frequencies to the library (`mcp.setClockFrequency(12e6)` and `8e6`).

### 2. The Adafruit MCP2515 Library 12MHz Bug
* **The Problem**: Once `12e6` was passed to the `Adafruit_MCP2515` library, it threw a `Failed to initialize` error. We discovered the core library only contains bit-timing math for 8MHz and 16MHz crystals; it literally did not know how to configure a 12MHz crystal to 500kbps.
* **The Fix**: Spliced custom mathematical bit-timing payload registers directly into the core `Adafruit_MCP2515.cpp` library file on the host machine:
  `{(long)12E6, (long)500E3, {0x00, 0xA0, 0x04}}`

### 3. The Ghost Library Cache
* **The Problem**: After patching the library, the compiler silently ignored the changes. 
* **The Fix**: Ran a deep filesystem search (`find`) and discovered two identical installations of the Adafruit library (`~/Documents/Arduino` and `~/Arduino`). The compiler was using the hidden one. Patched the correct library and ran a `--clean` compile to force the cache to absorb the new 12MHz math.

### 4. Hardware SPI Pin Conflicts on ESP32-S3
* **The Problem**: The Adafruit library's internal SPI initialization was forcefully resetting the ESP32-S3's hardware SPI pins back to factory defaults during boot, completely ignoring our custom `7, 14, 6, 16` pin mapping for the CAN Hat. This left the board dead in the water.
* **The Fix**: Swapped the MCP2515 initialization in `esp_relay_can.ino` to use **Software SPI** by passing all four pins directly to the constructor (`Adafruit_MCP2515 mcp(SPI_CS, SPI_MOSI, SPI_MISO, SPI_SCK);`). This forces bit-banging and guarantees the ESP32 strictly respects the physical wiring without hardware overrides.

### 5. The Physical CAN Layer (Transceiver Voltage & Wiring)
* **The Problem**: The NiRen breakout board on `esp_b` uses a `TJA1050` transceiver, which requires a strict 5V power supply. When powered by 3.3V, the MCP2515 initializes perfectly via SPI, but the transceiver is physically asleep and deaf to the CAN network.
* **The Fix**: Ensured the NiRen `VCC` pin was connected to `5V` on `esp_b`, verified the CAN-H and CAN-L wires were correctly oriented, and ensured both boards shared a common Ground (`GND`).

---

## Current State
Both boards are completely stable. The `esp_relay` successfully initializes its MCP2515 at 12MHz / 500kbps, and `esp_b` successfully initializes at 8MHz / 500kbps. 

When a physical button is pressed on `esp_relay`:
1. It registers the press and prints `EVENT: Button PUSHED IN`.
2. It broadcasts the CAN packet and immediately receives an ACK from `esp_b`, printing `STATUS -> SUCCESS`.
3. `esp_b` successfully parses the packet and toggles the onboard blue LED to confirm receipt.

The communication bridge is now 100% reliable and the system is fully prepped for the next phase: mapping the Ducati's physical relays.
