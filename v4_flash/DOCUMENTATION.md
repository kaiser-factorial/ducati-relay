# Ducati Rear Module — v4 Bootstrap (BLE Provisioning + OTA)

## Overview

`v4_flash/rear_bootstrap` is a **bootstrap firmware** for the rear module of the Ducati relay
system. Its only job is to get the board onto WiFi without hardcoding credentials, and to make
all future firmware updates wireless:

1. **BLE provisioning** — on first boot the board advertises over Bluetooth as
   `Ducati_Rear_Module`. You hand it your WiFi SSID + password from a phone app (Espressif's
   "ESP BLE Provisioning"). Credentials are stored in the ESP32's NVS flash, so it remembers
   them across reboots and power cycles.
2. **OTA (Over-The-Air) updates** — once on WiFi, the board runs an ArduinoOTA server and is
   reachable as `ducati-rear.local`. From then on you flash new firmware over WiFi — **no USB
   cable, no serial adapter**.

This is the foundation the real rear-module firmware (4 relays driven over CAN) will build on.

---

## Hardware

| Item | Detail |
|------|--------|
| Board | **ESP32_Relay X4 (AC/DC powered)** — integrated 4-channel relay board |
| MCU | ESP32-WROOM-32 (ESP32-D0WD-V3, classic dual-core, **not** an S3) |
| Relays | 4× Songle SRD-05VDC (active-HIGH) |
| Onboard power | AC-DC supply (mains) **plus** a DC input terminal: `7-30V / GND / 5V` |
| Programming | No onboard USB. Flashed via an external **SH-U09C5 USB-to-TTL adapter** |
| Serial port (Mac) | `/dev/cu.usbserial-BG03U2R7` (yours; re-check with `arduino-cli board list`) |
| Board MAC | `00:70:07:eb:30:04` |

### GPIO map (ESP32_Relay X4)

| Function | GPIO | Notes |
|----------|------|-------|
| Relay 1 (IN1) | 32 | active-HIGH; **not driven by the bootstrap**, defined for reference |
| Relay 2 (IN2) | 33 | active-HIGH |
| Relay 3 (IN3) | 25 | active-HIGH |
| Relay 4 (IN4) | 26 | active-HIGH |
| Onboard status LED | 23 | documented LED pin; bootstrap blinks it as "best guess" |
| External status LED | 13 | for a hand-wired LED: `GPIO13 → resistor → LED(anode) → LED(cathode) → GND` |
| Programming header | TX / RX / GND / GPIO0 / 5V | connect to the SH-U09C5 |

> ⚠️ **There is NO LED on GPIO2.** The old DevKit sketches (v1/v1.5/v2) used `LED_PIN 2` for
> the onboard LED, but on this integrated relay board GPIO2 is bare. The v4 sketch blinks
> GPIO23 (estimated onboard) **and** GPIO13 (external) instead.

### ⚠️ Pin conflict to resolve before adding CAN

`GPIO23` is used here as the status LED, but elsewhere in this project (see `SPEC.md` and
`v3_relay6`) **GPIO23 is the MCP2515 CAN MOSI line**. The relay pins `32/33/25/26` also overlap
the relay/button assignments in the v1.5/v2 firmware. If this board will also carry an MCP2515
for CAN, the LED and SPI pins collide — remap the status LED (or drop it) before wiring CAN.

---

## Wiring for flashing & power

### SH-U09C5 USB-TTL adapter → board (for flashing / serial)

| Adapter | Board header |
|---------|--------------|
| TX | RX |
| RX | TX |
| GND | GND |
| 5V | *(see power note below)* |

### Powering the board

The ESP32 radio (BLE/WiFi) draws current spikes that the adapter's weak 5V pin **cannot**
supply — this causes a brownout reset loop (see Troubleshooting). **Power the board from its own
input terminal instead:**

- **9V into the `7-30V` / `GND` terminal** ← this is what we used, works great.
- Or a solid **5V into the `5V` / `GND` terminal** (phone charger ≥1A / bench PSU).
- (Skip the AC mains input for bench work — that's the high-voltage side.)

When externally powered, connect the adapter for **TX / RX / GND only — do NOT also feed the
adapter's 5V** into the board (two supplies fighting).

### ⚠️ GPIO0 (IO0) must be floating to RUN

`GPIO0` is the boot-mode strapping pin:
- **IO0 LOW (grounded) at boot → download/flash mode** — the sketch never runs (looks like a
  dead board: no blink, silent serial).
- **IO0 HIGH/floating at boot → runs your firmware.**

Only ground IO0 momentarily *if* a USB flash fails to auto-start. For normal running, **leave
IO0 disconnected.** `arduino-cli upload` drives the reset/boot sequence over DTR/RTS, so you
usually don't need to touch IO0 at all.

---

## Build settings (important)

These three settings are all required — getting any one wrong is a separate failure:

| Setting | Value | Why |
|---------|-------|-----|
| FQBN | `esp32:esp32:esp32` | It's a **classic ESP32**, not `esp32s3`. Wrong target = bad binary. |
| Partition scheme | `PartitionScheme=min_spiffs` | The provisioning+OTA app is ~1.78 MB and overflows the default partition (~1.2 MB/app slot). `min_spiffs` gives 1.9 MB/slot and keeps OTA's dual-slot layout. |
| ESP32 core | 3.x (built on 3.3.10) | Core 3.x renamed provisioning constants `WIFI_PROV_*` → `NETWORK_PROV_*`. |

Full FQBN string used everywhere below:
```
esp32:esp32:esp32:PartitionScheme=min_spiffs
```

---

## First-time flash (USB / serial)

The very first flash must be over the wire, because the board has no firmware yet.

```bash
cd v4_flash/rear_bootstrap

# Compile + upload over the SH-U09C5 (adjust the port to your machine)
arduino-cli compile --upload \
  --fqbn esp32:esp32:esp32:PartitionScheme=min_spiffs \
  -p /dev/cu.usbserial-BG03U2R7 \
  rear_bootstrap.ino
```

Find your port with `arduino-cli board list` (run before/after plugging in the adapter; it's
the `/dev/cu.usbserial-*` one). If upload won't start, briefly ground IO0 during the
"Connecting…" phase, then release.

---

## Provisioning over Bluetooth (one time)

After the first flash, the board blinks (= "ready to provision"). On your phone:

1. Install **ESP BLE Provisioning** (Espressif) — iOS or Android.
2. Enable **Bluetooth** and (Android) **Location** for the app, or it won't scan BLE.
3. Tap **Provision New Device → I don't have a QR code**.
4. Select **`Ducati_Rear_Module`** from the list.
5. Proof-of-possession (PoP): **`12345678`**
6. Pick your **2.4 GHz** WiFi network (ESP32 does **not** do 5 GHz) and enter the password.

**Success:** the board's LEDs **go solid off** (= connected), and it disappears from the app
(the BLE radio is freed after provisioning — this is normal, not a failure). Credentials are
saved; it auto-reconnects on every future boot.

---

## Future updates over WiFi (OTA) — the normal workflow

Once provisioned, you never need the USB adapter again. The board advertises as
`ducati-rear.local`.

```bash
cd v4_flash/rear_bootstrap

# Compile
arduino-cli compile \
  --fqbn esp32:esp32:esp32:PartitionScheme=min_spiffs \
  rear_bootstrap.ino

# Upload over WiFi
arduino-cli upload \
  -p ducati-rear.local \
  --fqbn esp32:esp32:esp32:PartitionScheme=min_spiffs \
  --upload-field password= \
  rear_bootstrap.ino
```

**Two non-obvious bits:**
- `--upload-field password=` (empty value) is **required**. arduino-cli prompts for an OTA
  password even though none is set; without this flag a scripted upload fails with
  *"user input not supported in non interactive mode."*
- `-p ducati-rear.local` resolves via mDNS to whatever IP the board currently has. If `.local`
  doesn't resolve on your network, fall back to the raw IP — find it with
  `arduino-cli board list` (look for the `network` port) and use `-p <ip>` instead.

> 🔒 **Rule for OTA:** every firmware you push over OTA **must also** keep `ArduinoOTA.begin()`
> in it, or you lose wireless access and have to go back to the USB adapter to recover.

---

## How the firmware behaves (LED + serial cheat-sheet)

| Board state | LEDs (GPIO23 + GPIO13) | Serial (115200 baud) |
|-------------|------------------------|----------------------|
| Boot | **5 quick flashes** (self-test) | `=== Ducati Rear Module Bootstrap v1.0 ===` |
| Waiting to provision | **fast blink (250 ms)** | `[PROV] BLE provisioning started`, `[HB] ... (not connected)` |
| Connected to WiFi | **solid off** | `[WIFI] Connected. IP: ...`, `[OTA] ... ducati-rear.local`, `[HB] ... (CONNECTED)` |

The boot self-test is **5 flashes** on purpose: after an OTA update reboots the board, seeing
5 flashes is your visual proof the new firmware actually ran.

Watch the serial log live:
```bash
arduino-cli monitor -p /dev/cu.usbserial-BG03U2R7 -c baudrate=115200
```

---

## Troubleshooting — issues hit during bring-up (and fixes)

| Symptom | Cause | Fix |
|---------|-------|-----|
| Compile error: `'WIFI_PROV_SCHEME_BLE' was not declared` | ESP32 core 3.x renamed the provisioning enums | Use `NETWORK_PROV_*` names (`NETWORK_PROV_SCHEME_BLE`, `NETWORK_PROV_SCHEME_HANDLER_FREE_BTDM`, `NETWORK_PROV_SECURITY_1`) |
| `text section exceeds available space` | Default partition too small for the app | Add `:PartitionScheme=min_spiffs` to the FQBN |
| Flashes/uploads but never runs; LED never blinks; serial silent | **IO0 jumpered to GND** → board stuck in download mode | Disconnect IO0 from GND; keep GND. Tap EN/reset |
| Status LED never blinks even though it's running | **No LED on GPIO2** (old DevKit pin) | Blink GPIO23 (onboard) and/or wire an external LED to GPIO13 |
| "TX/RX go mad", reset loop, serial shows `E BOD: Brownout detector was triggered` | Radio current spike browns out the weak adapter 5V | Power from the board's `7-30V` (9V) or `5V` terminal; don't rely on adapter 5V |
| Binary builds but board misbehaves / won't flash right | Wrong board target (`esp32s3`) | Use `esp32:esp32:esp32` — it's a classic ESP32 |
| OTA upload: `user input not supported in non interactive mode` | arduino-cli prompts for OTA password | Add `--upload-field password=` |
| `ducati-rear.local` won't resolve | Router/mDNS quirk | Use the raw IP from `arduino-cli board list` instead |

---

## Re-provisioning (changing WiFi networks)

The board remembers its WiFi credentials in NVS. To point it at a different network you must
clear the stored provisioning so it advertises over BLE again. Options:
- Reflash a sketch that calls `WiFiProv.beginProvision(..., reset_provisioned = true)`, or
- Erase NVS / full flash erase over USB (`esptool erase_flash`), then re-flash the bootstrap.

(The current bootstrap does **not** auto-reset provisioning, so it won't re-enter BLE mode on
its own once it has valid credentials.)

---

## Next phase: wiring the CAN bus (MCP2515)

The end goal is this rear module on a shared CAN bus with a front ESP32 (buttons + dashboard)
and the **rusEFI** ECU on a **Ducati Scrambler 800**. Each ESP32 node gets an **MCP2515** SPI
CAN controller + a CAN transceiver. The v1–v3 builds already paid for the painful lessons
below — read these *before* you solder, they will save you hours. Full pin tables, the rusEFI
CAN ID map, and telemetry decode live in the repo-root `SPEC.md`; the v3 bring-up war stories
are in `v3_relay6/DOCUMENTATION.md`.

### Topology
```
[Front ESP32 + display] ── CAN_H/CAN_L ── [rusEFI ECU] ── CAN_H/CAN_L ── [THIS rear module]
        [120Ω]                                                              [120Ω]
                         one shared bus, 500 kbps, common ground
```

### ⚠️ The GPIO23 conflict — decide this first
The project-standard MCP2515 wiring (used by every other node, see `SPEC.md`) is:

| MCP2515 | ESP32 GPIO |
|---------|-----------|
| SCK | 18 |
| MISO (SO) | 19 |
| MOSI (SI) | **23** |
| CS | 5 |
| INT | a free GPIO (e.g. 27 or 22 — verify it's broken out on the header) |
| VCC | 3.3V (controller) — **transceiver may need 5V, see below** |
| GND | GND |

But on this relay board **GPIO23 is the status LED.** You can't have both. Recommended
resolution: **adopt 18/19/23/5 for the MCP2515** (so firmware is reusable across nodes) and
**drop the GPIO23 status LED** — keep only the external LED on **GPIO13**. The 4 relays are
hardwired by the PCB to **GPIO 32/33/25/26**, so those are fixed and can't be moved.

### Hard-won CAN lessons (from v1–v3)

1. **Crystal frequency must match the physical module.** `Adafruit_MCP2515` defaults to 16 MHz.
   Cheap modules are often **8 MHz**. Call `mcp.setClockFrequency(8e6)` (or `16e6`) to match the
   crystal *before* `mcp.begin(500000)`, or the real bitrate will be wrong and every packet
   drops. **The library only ships bit-timing tables for 8 and 16 MHz** — a **12 MHz** module at
   500 kbps will throw `Failed to initialize` and needs a custom timing triplet spliced into
   `Adafruit_MCP2515.cpp` (v3 used `{(long)12E6,(long)500E3,{0x00,0xA0,0x04}}`). **Avoid 12 MHz
   modules** unless you enjoy patching libraries.

2. **Transceiver voltage.** `SN65HVD230` = 3.3 V logic (fine off the ESP32). `TJA1050` (common
   on NiRen breakouts) **requires 5 V VCC** — at 3.3 V the SPI side initializes fine but the
   transceiver is physically deaf to the bus. Match the transceiver's VCC to its spec.

3. **Termination: 120 Ω at both physical ends.** Without it CAN_H/CAN_L read ~24 kΩ (open) and
   nothing is received; with it you should measure ~60 Ω across the bus. One 120 Ω resistor
   across CAN_H–CAN_L at each *end* node only. If the rusEFI board has a switchable on-board
   120 Ω, enable it on the ECU and put the second resistor at the far ESP32.

4. **Common ground across all three nodes.** CAN is differential but every transceiver needs a
   shared voltage reference — run GND between the ESP32s and to the ECU ground.

5. **Baud rate = 500 kbps**, matching rusEFI. Confirm the ECU's actual setting before relying
   on it.

6. **Ghost library cache (host-side).** If you ever patch the Adafruit library and changes seem
   ignored, you probably have two installs (`~/Arduino` and `~/Documents/Arduino`). Patch the
   one the compiler actually uses and do a `--clean` build.

### rusEFI CAN IDs — do NOT collide
Reserved by rusEFI (avoid): **0x100, 0x102** (TunerStudio-over-CAN), **0x190** (wideband O2),
**0x200–0x20B** (verbose telemetry, base address configurable), **0x667 / 0x7E1** (OpenBLT
bootloader). Put your own command/status IDs at **0x300+**. `SPEC.md` has the full custom
allocation plus the BASE0–BASE4 telemetry decode (RPM, TPS, coolant, battery V).

### Keep OTA alive alongside CAN
Don't let a CAN failure brick your wireless recovery path:
- Make CAN receive **non-blocking** and keep `ArduinoOTA.handle()` running every loop.
- If `mcp.begin()` fails, **log it and still start OTA** — don't `while(1){}` spin. That way you
  can always push a fix over WiFi instead of dragging the SH-U09C5 back out.
- (FYI, ESP32-S3 only: the Adafruit lib forced HW-SPI pins back to default on the S3; v3 fixed
  it with the software-SPI 4-arg constructor. On this classic WROOM-32 with standard VSPI pins
  you shouldn't hit it — but if SPI reads dead, try the software-SPI constructor.)

---

## Status & next steps

**Working end-to-end (2026-06-17):** first USB flash → BLE provisioning → connected to WiFi →
OTA update confirmed (`100% Done`, no USB). Board reachable as `ducati-rear.local`.

**Not done yet / TODO:**
- [ ] **Add an OTA password** + ArduinoOTA progress/error callbacks (harden before it lives on
      a bike, so a failed update logs *why* instead of silently bricking).
- [ ] **Resolve the GPIO23 LED vs CAN-MOSI conflict** before wiring an MCP2515 to this board.
- [ ] **Drive the actual relays** (GPIO 32/33/25/26) — currently only `#define`d for reference.
- [ ] Confirm whether the onboard LED really is on GPIO23 (note which LED lit during the
      5-flash self-test: GPIO23, GPIO13, or both).
