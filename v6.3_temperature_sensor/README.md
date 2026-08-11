# v6.3 hybrid temperature sensor

v6.3 keeps the v6.2 Bluetooth service and adds outbound authenticated Wi-Fi
polling for Sphere tools. It never opens an inbound port on the ESP32 or home
network.

## Hardware

- Lonely Binary ESP32-S3
- TK12 thermistor Signal → GPIO 6
- TK04 button Signal → GPIO 37

Measurements can be triggered by the physical button, the WearabLLM Sensor
tab over BLE, or a private Sphere action over HTTPS. Every path uses the same
24-sample average and thermistor conversion.

## Local configuration

Install the one additional Arduino dependency:

```bash
arduino-cli lib install ArduinoJson
```

To reuse an existing local WearabLLM firmware configuration without printing
or committing its secrets:

```bash
python3 v6.3_temperature_sensor/configure_from_wearabllm.py \
  /Users/corinakaiser/Projects/wearabLLM/WearabLLM/v3_WAVESHARE/firmware/sdkconfig
```

This creates `wifi_config.h` with mode `0600`. The file is ignored by Git and
contains the Wi-Fi credentials, private bridge URL/token, and pinned HTTPS
trust anchor. HTTPS fails closed if the trust anchor is absent.

## Compile and flash

The combined BLE, Wi-Fi, TLS, and JSON build needs the 3 MB application
partition. This ESP32-S3 is flashed over USB, so v6.3 deliberately uses the
Huge APP partition without OTA updates:

```bash
arduino-cli compile --upload \
  -p /dev/cu.usbmodem112301 \
  --fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc,PartitionScheme=huge_app \
  v6.3_temperature_sensor
```

Serial diagnostics remain available at 115200 baud:

```bash
arduino-cli monitor -p /dev/cu.usbmodem112301 -c baudrate=115200
```

## Sphere contract

The board identifies as `ducati-temp-sensor`, polls the private bridge every
three seconds, and accepts only `temperature_measurement` actions. A successful
terminal acknowledgement contains the real sequence number, Celsius value,
raw ADC value, and uptime. Failed or expired actions never become reported
temperatures.
