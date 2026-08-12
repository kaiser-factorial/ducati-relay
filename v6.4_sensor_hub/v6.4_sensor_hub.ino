/*
  v6.4_sensor_hub

  Capability-driven BLE + outbound Wi-Fi sensor hub for the ESP32-S3.

  Connections:
    TK12 NTC thermistor Signal -> GPIO 6
    TK04 push button Signal    -> GPIO 37

  Measurement triggers:
    - physical TinkerBlock button
    - WearabLLM Sensor tab over BLE
    - authenticated Sphere action over outbound HTTPS

  Copy wifi_config.example.h to wifi_config.h to enable Wi-Fi. The local config
  is ignored by Git. BLE remains available when Wi-Fi is not configured.
*/

#include <Arduino.h>
#include <ArduinoJson.h>
#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <math.h>

#if __has_include("wifi_config.h")
#include "wifi_config.h"
#else
#define TEMP_SENSOR_WIFI_SSID ""
#define TEMP_SENSOR_WIFI_PASSWORD ""
#define TEMP_SENSOR_BRIDGE_BASE_URL ""
#define TEMP_SENSOR_BRIDGE_TOKEN ""
#define TEMP_SENSOR_ROOT_CA ""
#endif

namespace {

constexpr uint8_t THERMISTOR_PIN = 6;
constexpr uint8_t BUTTON_PIN = 37;

constexpr char DEVICE_NAME[] = "Ducati Temperature Sensor";
constexpr char DEVICE_ID[] = "ducati-temp-sensor";
constexpr char SERVICE_UUID[] = "7b8f2b10-3a42-4d4e-9fd4-8b5b86d8a101";
constexpr char READING_UUID[] = "7b8f2b11-3a42-4d4e-9fd4-8b5b86d8a101";
constexpr char COMMAND_UUID[] = "7b8f2b12-3a42-4d4e-9fd4-8b5b86d8a101";
constexpr uint8_t COMMAND_TAKE_READING = 0x01;

constexpr uint16_t ADC_MAX = 4095;
constexpr uint8_t SAMPLE_COUNT = 24;
constexpr uint16_t SAMPLE_INTERVAL_MS = 4;
constexpr uint16_t DEBOUNCE_MS = 45;
constexpr uint32_t ACTION_POLL_MS = 3000;
constexpr uint32_t WIFI_RETRY_MS = 10000;
constexpr uint32_t MANIFEST_REFRESH_MS = 60000;
constexpr char FIRMWARE_VERSION[] = "6.4";
constexpr char TEMPERATURE_SENSOR_ID[] = "ambient_temperature";

constexpr float SERIES_RESISTOR_OHMS = 10000.0F;
constexpr float NOMINAL_RESISTANCE_OHMS = 10000.0F;
constexpr float NOMINAL_TEMPERATURE_K = 298.15F;
constexpr float BETA_COEFFICIENT = 3950.0F;
constexpr float CALIBRATION_OFFSET_C = 0.0F;

struct __attribute__((packed)) SensorPacket {
  uint8_t version;
  uint8_t flags;
  uint16_t sequence;
  int16_t temperatureCentiC;
  uint16_t rawAdc;
  uint32_t uptimeMs;
};

struct Measurement {
  bool valid;
  uint16_t sequence;
  float temperatureC;
  uint16_t rawAdc;
  uint32_t uptimeMs;
};

static_assert(sizeof(SensorPacket) == 12, "SensorPacket must stay 12 bytes");

BLECharacteristic* readingCharacteristic = nullptr;
bool buttonStableState = LOW;
bool buttonLastSample = LOW;
uint32_t buttonChangedAtMs = 0;
uint16_t readingSequence = 0;
volatile bool measurementRequested = false;
uint32_t lastActionPollMs = 0;
uint32_t lastWifiAttemptMs = 0;
uint32_t lastManifestPublishMs = 0;

class SensorServerCallbacks : public BLEServerCallbacks {
 public:
  void onConnect(BLEServer*) override {
    Serial.println("Browser connected over BLE.");
  }

  void onDisconnect(BLEServer*) override {
    Serial.println("Browser disconnected; advertising again.");
    BLEDevice::startAdvertising();
  }
};

class SensorCommandCallbacks : public BLECharacteristicCallbacks {
 public:
  void onWrite(BLECharacteristic* characteristic) override {
    const String command = characteristic->getValue();
    if (command.length() > 0 &&
        static_cast<uint8_t>(command[0]) == COMMAND_TAKE_READING) {
      measurementRequested = true;
    }
  }
};

bool wifiConfigured() {
  return TEMP_SENSOR_WIFI_SSID[0] != '\0' &&
      TEMP_SENSOR_BRIDGE_BASE_URL[0] != '\0' &&
      TEMP_SENSOR_BRIDGE_TOKEN[0] != '\0';
}

String bridgeUrl(const String& path) {
  String base = TEMP_SENSOR_BRIDGE_BASE_URL;
  while (base.endsWith("/")) base.remove(base.length() - 1);
  return base + path;
}

void addBridgeHeaders(HTTPClient& http) {
  http.addHeader("X-WearabLLM-Device-Token", TEMP_SENSOR_BRIDGE_TOKEN);
  http.addHeader("X-WearabLLM-Device-Id", DEVICE_ID);
}

bool beginBridgeRequest(
    HTTPClient& http,
    WiFiClientSecure& secureClient,
    const String& url) {
  if (!url.startsWith("https://")) {
    Serial.println("Bridge request refused: HTTPS is required.");
    return false;
  }
  if (TEMP_SENSOR_ROOT_CA[0] == '\0' || strstr(TEMP_SENSOR_ROOT_CA, "PASTE_ROOT_CA_HERE")) {
    Serial.println("HTTPS disabled: configure TEMP_SENSOR_ROOT_CA.");
    return false;
  }
  secureClient.setCACert(TEMP_SENSOR_ROOT_CA);
  return http.begin(secureClient, url);
}

uint16_t readAveragedAdc() {
  uint32_t total = 0;
  for (uint8_t i = 0; i < SAMPLE_COUNT; ++i) {
    total += analogRead(THERMISTOR_PIN);
    delay(SAMPLE_INTERVAL_MS);
  }
  return static_cast<uint16_t>((total + (SAMPLE_COUNT / 2)) / SAMPLE_COUNT);
}

bool calculateTemperatureC(uint16_t rawAdc, float& temperatureC) {
  if (rawAdc <= 5 || rawAdc >= ADC_MAX - 5) return false;
  const float resistance = SERIES_RESISTOR_OHMS *
      (static_cast<float>(ADC_MAX) / static_cast<float>(rawAdc) - 1.0F);
  if (!isfinite(resistance) || resistance <= 0.0F) return false;
  const float inverseKelvin =
      (logf(resistance / NOMINAL_RESISTANCE_OHMS) / BETA_COEFFICIENT) +
      (1.0F / NOMINAL_TEMPERATURE_K);
  temperatureC = (1.0F / inverseKelvin) - 273.15F + CALIBRATION_OFFSET_C;
  return isfinite(temperatureC) && temperatureC >= -40.0F && temperatureC <= 125.0F;
}

Measurement takeAndPublishReading() {
  Measurement measurement{};
  measurement.rawAdc = readAveragedAdc();
  measurement.uptimeMs = millis();
  measurement.sequence = ++readingSequence;
  if (measurement.sequence == 0) measurement.sequence = ++readingSequence;
  measurement.valid = calculateTemperatureC(measurement.rawAdc, measurement.temperatureC);

  SensorPacket packet{};
  packet.version = 1;
  packet.flags = measurement.valid ? 0x01 : 0x00;
  packet.sequence = measurement.sequence;
  packet.temperatureCentiC = measurement.valid
      ? static_cast<int16_t>(lroundf(measurement.temperatureC * 100.0F))
      : 0;
  packet.rawAdc = measurement.rawAdc;
  packet.uptimeMs = measurement.uptimeMs;
  readingCharacteristic->setValue(reinterpret_cast<uint8_t*>(&packet), sizeof(packet));
  readingCharacteristic->notify();

  Serial.println();
  Serial.printf("Reading #%u | Raw ADC: %u", measurement.sequence, measurement.rawAdc);
  if (!measurement.valid) {
    Serial.println(" | Temperature: ERROR");
  } else {
    const float fahrenheit = measurement.temperatureC * 9.0F / 5.0F + 32.0F;
    Serial.printf(" | Temperature: %.2f C / %.2f F\n", measurement.temperatureC, fahrenheit);
  }
  return measurement;
}

bool acknowledgeAction(
    const String& actionId,
    const char* status,
    const char* error,
    const Measurement* measurement) {
  WiFiClientSecure secureClient;
  HTTPClient http;
  const String url = bridgeUrl(
      "/v1/devices/" + String(DEVICE_ID) + "/actions/" + actionId + "/ack");
  if (!beginBridgeRequest(http, secureClient, url)) return false;
  addBridgeHeaders(http);
  http.addHeader("Content-Type", "application/json");

  JsonDocument document;
  document["status"] = status;
  if (error && error[0]) document["error"] = error;
  if (measurement && measurement->valid) {
    JsonObject result = document["result"].to<JsonObject>();
    result["version"] = 1;
    result["sequence"] = measurement->sequence;
    result["uptime_ms"] = measurement->uptimeMs;
    JsonArray readings = result["readings"].to<JsonArray>();
    JsonObject reading = readings.add<JsonObject>();
    reading["sensor_id"] = TEMPERATURE_SENSOR_ID;
    reading["value"] = roundf(measurement->temperatureC * 100.0f) / 100.0f;
    reading["unit"] = "Cel";
  }
  String body;
  serializeJson(document, body);
  const int statusCode = http.POST(body);
  http.end();
  return statusCode >= 200 && statusCode < 300;
}

bool publishSensorManifest() {
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClientSecure secureClient;
  HTTPClient http;
  const String url = bridgeUrl(
      "/v1/devices/" + String(DEVICE_ID) + "/sensor-manifest");
  if (!beginBridgeRequest(http, secureClient, url)) return false;
  addBridgeHeaders(http);
  http.addHeader("Content-Type", "application/json");
  JsonDocument document;
  document["version"] = 1;
  document["firmware_version"] = FIRMWARE_VERSION;
  JsonArray sensors = document["sensors"].to<JsonArray>();
  JsonObject sensor = sensors.add<JsonObject>();
  sensor["id"] = TEMPERATURE_SENSOR_ID;
  sensor["quantity"] = "temperature";
  sensor["label"] = "Ambient temperature";
  sensor["unit"] = "Cel";
  String body;
  serializeJson(document, body);
  const int statusCode = http.POST(body);
  http.end();
  return statusCode >= 200 && statusCode < 300;
}

void pollSphereAction() {
  if (WiFi.status() != WL_CONNECTED) return;
  WiFiClientSecure secureClient;
  HTTPClient http;
  const String url = bridgeUrl("/v1/devices/" + String(DEVICE_ID) + "/actions");
  if (!beginBridgeRequest(http, secureClient, url)) return;
  addBridgeHeaders(http);
  const int statusCode = http.GET();
  const String body = statusCode >= 200 && statusCode < 300 ? http.getString() : "";
  http.end();
  if (statusCode < 200 || statusCode >= 300 || body.isEmpty()) {
    Serial.printf("Sphere poll failed: HTTP %d\n", statusCode);
    return;
  }

  JsonDocument document;
  if (deserializeJson(document, body)) {
    Serial.println("Sphere poll returned invalid JSON.");
    return;
  }
  JsonVariant action = document["action"];
  if (action.isNull()) return;
  const String actionId = action["id"] | "";
  const String actionType = action["action_type"] | "";
  if (actionId.isEmpty()) return;
  if (actionType != "sensor_read") {
    acknowledgeAction(actionId, "failed", "Unsupported action type", nullptr);
    return;
  }
  JsonArray requested = action["payload"]["sensor_ids"].as<JsonArray>();
  if (requested.size() != 1 || String(requested[0] | "") != TEMPERATURE_SENSOR_ID) {
    acknowledgeAction(actionId, "failed", "Requested sensor is unavailable", nullptr);
    return;
  }

  const Measurement measurement = takeAndPublishReading();
  if (!measurement.valid) {
    acknowledgeAction(actionId, "failed", "Thermistor reading was outside the valid range", nullptr);
    return;
  }
  if (!acknowledgeAction(actionId, "completed", "", &measurement)) {
    Serial.println("Temperature acknowledgement failed; the leased action will retry.");
  }
}

void maintainWifi(uint32_t now) {
  if (!wifiConfigured()) return;
  if (WiFi.status() == WL_CONNECTED) return;
  if (lastWifiAttemptMs && now - lastWifiAttemptMs < WIFI_RETRY_MS) return;
  lastWifiAttemptMs = now;
  Serial.println("Connecting to configured Wi-Fi…");
  WiFi.disconnect();
  WiFi.begin(TEMP_SENSOR_WIFI_SSID, TEMP_SENSOR_WIFI_PASSWORD);
}

void configureBluetooth() {
  BLEDevice::init(DEVICE_NAME);
  BLEServer* server = BLEDevice::createServer();
  server->setCallbacks(new SensorServerCallbacks());
  BLEService* service = server->createService(SERVICE_UUID);
  readingCharacteristic = service->createCharacteristic(
      READING_UUID,
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  readingCharacteristic->addDescriptor(new BLE2902());
  BLECharacteristic* commandCharacteristic = service->createCharacteristic(
      COMMAND_UUID, BLECharacteristic::PROPERTY_WRITE);
  commandCharacteristic->setCallbacks(new SensorCommandCallbacks());
  const SensorPacket waitingPacket{1, 0, 0, 0, 0, 0};
  readingCharacteristic->setValue(
      reinterpret_cast<const uint8_t*>(&waitingPacket), sizeof(waitingPacket));
  service->start();
  BLEAdvertising* advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->setScanResponse(true);
  advertising->start();
}

}  // namespace

void setup() {
  Serial.begin(115200);
  pinMode(THERMISTOR_PIN, INPUT);
  pinMode(BUTTON_PIN, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(THERMISTOR_PIN, ADC_11db);
  configureBluetooth();
  if (wifiConfigured()) {
    WiFi.mode(WIFI_STA);
    maintainWifi(millis());
  }
  delay(300);
  Serial.println("Ducati sensor hub v6.4 ready.");
  Serial.println(wifiConfigured()
      ? "BLE enabled; authenticated Sphere Wi-Fi polling enabled."
      : "BLE enabled; Wi-Fi disabled until wifi_config.h is configured.");
}

void loop() {
  const uint32_t now = millis();
  maintainWifi(now);

  if (measurementRequested) {
    measurementRequested = false;
    takeAndPublishReading();
  }

  if (wifiConfigured() && WiFi.status() == WL_CONNECTED &&
      (!lastManifestPublishMs || now - lastManifestPublishMs >= MANIFEST_REFRESH_MS)) {
    if (publishSensorManifest()) lastManifestPublishMs = now;
  }

  if (wifiConfigured() && WiFi.status() == WL_CONNECTED &&
      now - lastActionPollMs >= ACTION_POLL_MS) {
    lastActionPollMs = now;
    pollSphereAction();
  }

  const bool sample = digitalRead(BUTTON_PIN) == HIGH;
  if (sample != buttonLastSample) {
    buttonLastSample = sample;
    buttonChangedAtMs = now;
  }
  if (sample != buttonStableState && now - buttonChangedAtMs >= DEBOUNCE_MS) {
    buttonStableState = sample;
    if (buttonStableState == HIGH) takeAndPublishReading();
  }
  delay(2);
}
