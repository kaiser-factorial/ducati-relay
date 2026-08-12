#pragma once

// Copy this file to wifi_config.h and replace every placeholder. wifi_config.h
// is ignored by Git so network credentials and the private bridge token stay
// local. For HTTPS, paste the PEM root CA that validates the bridge hostname;
// v6.3 deliberately refuses insecure TLS.

#define TEMP_SENSOR_WIFI_SSID "YOUR_WIFI_NAME"
#define TEMP_SENSOR_WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
#define TEMP_SENSOR_BRIDGE_BASE_URL "https://YOUR_PRIVATE_BRIDGE.example"
#define TEMP_SENSOR_BRIDGE_TOKEN "YOUR_WEARABLLM_DEVICE_TOKEN"
#define TEMP_SENSOR_ROOT_CA R"PEM(
-----BEGIN CERTIFICATE-----
PASTE_ROOT_CA_HERE
-----END CERTIFICATE-----
)PEM"
