# v6.4 capability-driven sensor hub

v6.4 keeps the v6.2 Bluetooth temperature service and adds a generic,
capability-driven sensor contract over outbound authenticated Wi-Fi. It never
opens an inbound port on the ESP32 or home network.

## Hardware

- Lonely Binary ESP32-S3
- TK12 thermistor Signal → GPIO 6
- TK04 button Signal → GPIO 37

At boot and once per minute, the board publishes a structured manifest with
sensor IDs, quantities, labels, and units. The initial manifest contains
`ambient_temperature`; humidity and light can later be added as sibling
capabilities without changing Sphere's tool names.

## Local configuration

Install the one additional Arduino dependency:

```bash
arduino-cli lib install ArduinoJson
```

To reuse an existing local WearabLLM firmware configuration without printing
or committing its secrets:

```bash
python3 v6.4_sensor_hub/configure_from_wearabllm.py \
  /Users/corinakaiser/Projects/wearabLLM/WearabLLM/v3_WAVESHARE/firmware/sdkconfig
```

This creates `wifi_config.h` with mode `0600`. The file is ignored by Git and
contains the Wi-Fi credentials, private bridge URL/token, and pinned HTTPS
trust anchor. HTTPS fails closed if the trust anchor is absent.

## Compile and flash

The combined BLE, Wi-Fi, TLS, and JSON build needs the 3 MB application
partition. This ESP32-S3 is flashed over USB, so v6.4 deliberately uses the
Huge APP partition without OTA updates:

```bash
arduino-cli compile --upload \
  -p /dev/cu.usbmodem112301 \
  --fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc,PartitionScheme=huge_app \
  v6.4_sensor_hub
```

Serial diagnostics remain available at 115200 baud:

```bash
arduino-cli monitor -p /dev/cu.usbmodem112301 -c baudrate=115200
```

## Sphere contract

The board retains device ID `ducati-temp-sensor` for compatibility, publishes
its sensor manifest, polls every three seconds, and accepts only `sensor_read`
actions naming capabilities it actually registered. A successful terminal
acknowledgement contains an array of real sensor readings plus sequence and
uptime metadata. Failed or expired actions never become reported measurements.
