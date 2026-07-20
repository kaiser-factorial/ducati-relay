# Ducati Flasher — native macOS Route 2

This is the dependency-free Swift foundation for updating a rusEFI nano through the v5 ESP32
SLCAN bridge directly from macOS. It uses the same ESP32 firmware as Route 1; switching between
Linux BootCommander and this tool does not require reflashing the bridge.

## Current safety status

The native transport and preparation layers are implemented, but **live programming is
deliberately locked** until a successful Route 1 exchange is captured and the native XCP sequence
is validated against it. The `flash` command always refuses to erase or program the ECU in this
placeholder phase.

Working now:

- Native Apple-silicon/Intel macOS command-line build via Swift Package Manager.
- ESP32 USB serial discovery and SLCAN bridge diagnostics.
- SLCAN classic CAN encoding/decoding, including 11-bit and 29-bit frames.
- Direct filtered capture of OpenBLT IDs `0x667` and `0x7E1` on macOS.
- Motorola S-record structure, count, checksum, address, and termination validation.
- Route 1 `candump -L` exchange import into a versioned JSON profile.
- Safe dry runs that never open the serial port or transmit CAN traffic.
- A hard live-flash gate that cannot be enabled by editing the profile alone.

Still required after the capture:

- Implement the OpenBLT XCP connect/program/erase/write/reset state machine.
- Compare its generated frames and timing with the successful Route 1 exchange.
- Add interrupted-transfer and power-loss recovery tests.
- Perform a controlled hardware replay before unlocking any live programming path.

## Build

Apple's command-line developer tools provide Swift:

```bash
xcode-select --install
```

Build and package both native executables:

```bash
cd /Users/corinakaiser/Projects/ducati_relay/v5_macos_updater
./scripts/build_release.sh
```

The binaries appear under `dist/`:

```text
dist/ducati-flasher
dist/ducati-flasher-selftest
```

Run the self-test before use:

```bash
./dist/ducati-flasher-selftest
```

## Commands available now

List likely ESP32 ports:

```bash
./dist/ducati-flasher ports
```

Check the Mac without hardware:

```bash
./dist/ducati-flasher doctor
```

After loading the Route 1 SLCAN firmware onto the ESP32, query the bridge and open CAN at
500 kbit/s:

```bash
./dist/ducati-flasher doctor --port /dev/cu.usbserial-YOUR_PORT
```

Validate the nano firmware without touching hardware:

```bash
./dist/ducati-flasher validate /path/to/rusefi_update.srec
```

Run the current placeholder dry-run:

```bash
./dist/ducati-flasher dry-run \
  --firmware /path/to/rusefi_update.srec \
  --profile profiles/placeholder.json
```

This validates the firmware and profile while explicitly reporting that the exchange is still a
placeholder.

## Import the Route 1 exchange

After `v5_can_flasher/scripts/flash_and_capture.sh` succeeds, copy its complete timestamped capture
directory back to the Mac. Import the raw exchange:

```bash
./dist/ducati-flasher import-capture \
  ../v5_can_flasher/captures/TIMESTAMP/openblt_exchange.log \
  --output profiles/nano-route1.generated.json
```

The importer requires traffic in both directions, records counts/timing/first frames, and produces
a profile with:

```json
"liveFlashingEnabled" : false,
"validationState" : "captured-unvalidated"
```

Those values are intentional. A captured exchange is evidence for implementation; it is not by
itself permission to flash.

Use the generated profile in a dry run:

```bash
./dist/ducati-flasher dry-run \
  --firmware /path/to/rusefi_update.srec \
  --profile profiles/nano-route1.generated.json
```

## Optional direct capture on macOS

The native tool can also listen to the SLCAN bridge while another CAN node performs an update:

```bash
mkdir -p captures
./dist/ducati-flasher capture \
  --port /dev/cu.usbserial-YOUR_PORT \
  --output captures/native-observation.log \
  --seconds 120
```

Only `0x667` and `0x7E1` are saved. Let the specified duration finish so the capture is written;
interrupting the process early does not currently guarantee a saved partial file.

## Live flash placeholder

The command shape is reserved now so scripts and documentation will not need to change later:

```bash
./dist/ducati-flasher flash \
  --port /dev/cu.usbserial-YOUR_PORT \
  --firmware /path/to/rusefi_update.srec \
  --profile profiles/nano-route1.generated.json
```

At this stage it exits with `LIVE FLASH REFUSED` before opening the serial port or sending CAN.
Do not weaken that gate manually. It will be replaced only after the native XCP engine and replay
tests exist.

## Relationship to Route 1

```text
Route 1 now
  Linux BootCommander → slcan0 → ESP32 SLCAN → CAN → nano

Route 2 foundation
  macOS ducati-flasher → ESP32 SLCAN → CAN → nano
```

Route 1 remains the authoritative recovery/update mechanism until Route 2 completes its protocol
and hardware verification gates. See [the Route 1 guide](../v5_can_flasher/README.md) for wiring,
termination, power, firmware selection, and recovery precautions.
