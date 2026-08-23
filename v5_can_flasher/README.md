# rusEFI nano CAN flasher — Route 1

This package turns the existing Ducati v5 USB ESP32 and SN65HVD230 transceiver into a Linux
SocketCAN adapter, flashes a rusEFI nano through its OpenBLT CAN bootloader, and automatically
captures the successful request/response exchange for the future native-macOS flasher.

The flasher does **not** repair or overwrite the OpenBLT bootloader itself. It works only if the
nano still powers up and its CAN-enabled OpenBLT bootloader is intact. A damaged bootloader
requires SWD recovery.

## Package contents

```text
v5_can_flasher/
├── firmware/v5_can_flasher/v5_can_flasher.ino  ESP32 SLCAN firmware
├── scripts/install_bridge_macos.sh              one-command ESP32 install from macOS
├── scripts/setup_linux.sh                       installs/builds Linux dependencies
├── scripts/start_slcan.sh                       creates the slcan0 interface
├── scripts/stop_slcan.sh                        safely removes slcan0
├── scripts/flash_and_capture.sh                 flashes and captures in one command
├── scripts/summarize_capture.py                 verifies bidirectional capture
├── tests/test_summarize_capture.py              host-side capture tests
└── captures/                                    generated, git-ignored flash evidence
```

## What the package automates

`flash_and_capture.sh` performs the entire sensitive portion of the operation:

1. Validates that the firmware is a Motorola S-record file.
2. Starts the ESP32 at 500 kbit/s as Linux interface `slcan0`.
3. Captures exact OpenBLT CAN IDs `0x667` and `0x7E1` with absolute timestamps.
4. Runs BootCommander using XCP over classic CAN.
5. Saves the firmware SHA-256, host metadata, BootCommander output, raw CAN exchange, and a
   request/response summary.
6. Stops the CAN interface even if flashing fails or the script is interrupted.

An updater exit code of zero **and** at least one frame in each direction are both required before
the wrapper reports success.

## Hardware

Use the same USB sender hardware documented in `../v5_usb_can_sender/README.md`:

| SN65HVD230 | Classic ESP32 DevKit |
|---|---|
| `3.3V` | `3.3V` |
| `GND` | `GND` |
| `CAN TX` | GPIO21 |
| `CAN RX` | GPIO22 |
| `CANH` | nano CAN high, connector pin 18D |
| `CANL` | nano CAN low, connector pin 17D |

Also connect ESP32/transceiver ground to the nano power ground. The current official nano pinout
identifies 16D and 22D as power ground and 6D as 12 V battery power. Confirm those labels against
the exact harness/revision before applying power.

Power the nano separately from a fused, current-limited 12–14 V bench supply. **Never connect
12 V to the ESP32 or SN65HVD230.** USB powers the ESP32 side; the bench supply powers the ECU.

With all power removed, measure between CAN-H and CAN-L:

- About 60 ohms: correct—two 120-ohm terminators are present.
- About 120 ohms: one terminator is missing.
- About 40 ohms: three terminators are present; remove one.
- Near 0 ohms: likely short; do not power anything.

For the first update, use a short two-node bench bus. Do not flash on the motorcycle with coils,
injectors, pumps, relays, or other CAN nodes active.

## Part 1: load the bridge firmware from macOS

Do this before handing the ESP32 USB device to the Linux VM.

```bash
cd /Users/corinakaiser/Projects/ducati_relay/v5_can_flasher
arduino-cli board list
./scripts/install_bridge_macos.sh /dev/cu.usbserial-YOUR_PORT
```

Replace the port with the result from `arduino-cli board list`. The firmware uses USB serial at
921600 baud and intentionally prints no banners or diagnostics: silence is correct until an SLCAN
client connects.

Do not open Arduino Serial Monitor afterward. Only one program may own the serial port.

## Part 2: create the Linux VM on an Apple-silicon Mac

UTM is the recommended free route because its QEMU backend supports USB device sharing.

1. Install [UTM](https://mac.getutm.app/).
2. Download the current **Ubuntu 24.04 LTS ARM64 server ISO** from the
   [official Ubuntu ARM download](https://ubuntu.com/download/server/arm).
3. In UTM choose **Create a New Virtual Machine → Virtualize → Linux**.
4. Select the ARM64 Ubuntu ISO. Allocate roughly 4 GB RAM, 2–4 CPU cores, and at least 20 GB disk.
5. Complete the Ubuntu installation and remove/eject the installer ISO when asked to reboot.
6. Shut down the VM. In its UTM configuration, use the QEMU backend and enable **USB sharing**.
7. Start Ubuntu, connect the ESP32, press UTM's USB toolbar button, and attach the ESP32/USB-serial
   device to the VM. If macOS still owns it, close Arduino tools and detach/re-attach it in UTM.
8. Copy or share this repository into Ubuntu. A simple option is a temporary ZIP or `git clone`;
   avoid running the flasher directly from an unreliable network-mounted folder.

Inside Ubuntu, verify that the ESP32 appeared:

```bash
ls -l /dev/ttyUSB* /dev/ttyACM*
```

Most classic ESP32 boards appear as `/dev/ttyUSB0`; some appear as `/dev/ttyACM0`.

UTM notes that macOS cannot provide every USB device with a true hardware reset. If the serial
device does not appear, detach it using UTM's USB menu, unplug/replug it, and attach it again. A
native Linux laptop or Raspberry Pi is an equally valid alternative and removes the VM USB layer.

## Part 3: install Linux dependencies

From the package directory inside Ubuntu:

```bash
cd /path/to/ducati_relay/v5_can_flasher
chmod +x scripts/*.sh scripts/*.py
./scripts/setup_linux.sh
sudo usermod -aG dialout "$USER"
```

Log out of Ubuntu and back in after adding the `dialout` group. The setup script installs
`can-utils`, build tools, and Python, then checks out the pinned OpenBLT host release and builds
LibOpenBLT plus BootCommander under `v5_can_flasher/tools/`.

## Part 4: obtain the correct nano firmware

Download the **nano-specific** rusEFI release from the
[official nano page](https://wiki.rusefi.com/nano/). Extract the bundle and locate its
`rusefi_update.srec` file.

Do not substitute a `.bin`, `.hex`, or firmware for another rusEFI board. The wrapper accepts only
Motorola S-record extensions, but it cannot prove that an S-record belongs to your exact nano
hardware. Preserve your current tune/configuration separately if it is accessible through another
connection; a firmware update and calibration backup are different operations.

## Part 5: preflight the bus

Before connecting the nano, the ESP32 firmware and Linux SLCAN path can be checked:

```bash
./scripts/start_slcan.sh /dev/ttyUSB0
ip -details link show slcan0
./scripts/stop_slcan.sh
```

Then wire the powered-off two-node bus, verify resistance, attach the USB bridge to Ubuntu, and
apply the nano's current-limited 12 V power. Do not proceed if the supply immediately current-limits,
anything heats unexpectedly, or CAN-H/CAN-L wiring is uncertain.

## Part 6: flash and capture

Run one command inside Ubuntu:

```bash
./scripts/flash_and_capture.sh \
  /dev/ttyUSB0 \
  /path/to/nano-bundle/rusefi_update.srec
```

During the update:

- Do not remove nano power.
- Do not unplug USB or CAN wiring.
- Do not suspend the Mac or VM.
- Do not start another serial monitor or CAN program.

If the nano application starts too quickly for the bootloader connection, leave the command
ready, switch the nano power off, start the command, and then immediately restore nano power.
OpenBLT listens for an XCP `CONNECT` during its configured boot window.

On success, the script prints the capture directory. Example:

```text
captures/20260720T221500Z/
├── bootcommander.log
├── metadata.txt
├── openblt_exchange.log
└── summary.txt
```

The raw exchange is intentionally in `candump -L` format so it remains machine-readable and can
be replayed or analyzed later. Copy the complete timestamped directory back to the Mac. Do not
publish it until it has been reviewed; while firmware-update traffic normally contains firmware
blocks rather than tune values, treat captured ECU traffic as private by default.

## Reading the result

A successful wrapper run ends with:

```text
SUCCESS: BootCommander completed and a bidirectional OpenBLT exchange was captured.
```

`summary.txt` must show `status: CAPTURED`, with nonzero counts for both IDs. `bootcommander.log`
must independently show completion. CAN ACK alone does not prove that the nano accepted or wrote
the firmware.

If flashing fails, the wrapper still preserves all logs:

| Symptom | Likely cause |
|---|---|
| No `0x667` requests | BootCommander/SocketCAN setup failed |
| Requests but no `0x7E1` responses | Nano unpowered, wrong bitrate/IDs, boot window missed, or bootloader unavailable |
| CAN errors/bus-off | H/L reversal, termination, missing common ground, or transceiver power |
| Responses begin then stop | Power/USB interruption, serial overrun, or ECU reset |
| BootCommander rejects file | Wrong format, corrupt S-record, or incompatible image |

After a failed flash, keep the nano powered off until the logs are reviewed. If OpenBLT itself is
still intact, another CAN attempt may recover the application. If it no longer responds, stop and
use SWD recovery rather than repeatedly power-cycling unknown firmware.

## Manual diagnostics

To inspect the bootloader IDs without flashing:

```bash
./scripts/start_slcan.sh /dev/ttyUSB0
candump -L 'slcan0,667:7FF,7E1:7FF'
```

Press `Ctrl-C`, then always stop the adapter:

```bash
./scripts/stop_slcan.sh
```

Do not use `cansend` to guess OpenBLT commands during a real update. BootCommander owns protocol
sequencing and timeouts.

## Protocol and source references

- [rusEFI firmware update via CAN](https://wiki.rusefi.com/Firmware-update-via-CAN/)
- [OpenBLT BootCommander manual](https://www.feaser.com/openblt/doku.php?id=manual:bootcommander)
- [OpenBLT CAN update guide](https://www.feaser.com/openblt/doku.php?id=manual:can_demo)
- [Linux can-utils](https://github.com/linux-can/can-utils)
- [UTM USB sharing](https://docs.getutm.app/guest-support/sharing/usb/)
- [Official rusEFI nano pinout](https://rusefi.com/docs/pinouts/nano/)

## Known verification boundary

The ESP32 firmware compiles locally, the host scripts/capture parser have automated checks, and
`setup_linux.sh` has been run successfully in a clean ARM64 Ubuntu 24.04 environment. That test
built pinned OpenBLT from source and launched the resulting ARM64 BootCommander executable.

A complete update still cannot be certified without the physical ESP32/transceiver, nano, bench
supply, USB pass-through, and a real SocketCAN bus. The first hardware run should therefore be
treated as a controlled bench validation, and its generated capture is the evidence needed for
Route 2.
