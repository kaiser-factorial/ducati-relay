/*
  v6.2_temperature_sensor

  Bluetooth version of the Lonely Binary ESP32-S3 temperature sensor.

  Correct TinkerBlock connections:
    TK12 NTC thermistor Signal -> GPIO 6
    TK04 push button Signal    -> GPIO 37

  Press the physical button once to take one averaged reading. The board sends
  that reading to the WearabLLM Sensor tab over Bluetooth Low Energy (BLE).
  Serial output at 115200 baud is retained as an optional troubleshooting aid.
*/

#include <Arduino.h>
#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <math.h>

namespace {

constexpr uint8_t THERMISTOR_PIN = 6;
constexpr uint8_t BUTTON_PIN = 37;

constexpr char DEVICE_NAME[] = "Ducati Temperature Sensor";
constexpr char SERVICE_UUID[] = "7b8f2b10-3a42-4d4e-9fd4-8b5b86d8a101";
constexpr char READING_UUID[] = "7b8f2b11-3a42-4d4e-9fd4-8b5b86d8a101";
constexpr char COMMAND_UUID[] = "7b8f2b12-3a42-4d4e-9fd4-8b5b86d8a101";
constexpr uint8_t COMMAND_TAKE_READING = 0x01;

constexpr uint16_t ADC_MAX = 4095;
constexpr uint8_t SAMPLE_COUNT = 24;
constexpr uint16_t SAMPLE_INTERVAL_MS = 4;
constexpr uint16_t DEBOUNCE_MS = 45;

// TK12 thermistor circuit values documented by Lonely Binary.
constexpr float SERIES_RESISTOR_OHMS = 10000.0F;
constexpr float NOMINAL_RESISTANCE_OHMS = 10000.0F;
constexpr float NOMINAL_TEMPERATURE_K = 298.15F;  // 25 C
constexpr float BETA_COEFFICIENT = 3950.0F;
constexpr float CALIBRATION_OFFSET_C = 0.0F;

// Versioned 12-byte packet, little-endian on ESP32-S3.
// flags bit 0: temperature reading is valid.
struct __attribute__((packed)) SensorPacket {
  uint8_t version;
  uint8_t flags;
  uint16_t sequence;
  int16_t temperatureCentiC;
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

class SensorServerCallbacks : public BLEServerCallbacks {
 public:
  void onConnect(BLEServer* server) override {
    Serial.println("Browser connected over BLE.");
  }

  void onDisconnect(BLEServer* server) override {
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

uint16_t readAveragedAdc() {
  uint32_t total = 0;
  for (uint8_t i = 0; i < SAMPLE_COUNT; ++i) {
    total += analogRead(THERMISTOR_PIN);
    delay(SAMPLE_INTERVAL_MS);
  }
  return static_cast<uint16_t>((total + (SAMPLE_COUNT / 2)) / SAMPLE_COUNT);
}

bool calculateTemperatureC(uint16_t rawAdc, float& temperatureC) {
  if (rawAdc <= 5 || rawAdc >= ADC_MAX - 5) {
    return false;
  }

  // TK12 circuit: NTC to VCC, 10k fixed resistor to GND, Signal at junction.
  const float resistance = SERIES_RESISTOR_OHMS *
      (static_cast<float>(ADC_MAX) / static_cast<float>(rawAdc) - 1.0F);
  if (!isfinite(resistance) || resistance <= 0.0F) {
    return false;
  }

  const float inverseKelvin =
      (logf(resistance / NOMINAL_RESISTANCE_OHMS) / BETA_COEFFICIENT) +
      (1.0F / NOMINAL_TEMPERATURE_K);
  temperatureC = (1.0F / inverseKelvin) - 273.15F + CALIBRATION_OFFSET_C;
  return isfinite(temperatureC) && temperatureC >= -40.0F && temperatureC <= 125.0F;
}

void publishReading() {
  const uint16_t rawAdc = readAveragedAdc();
  float temperatureC = 0.0F;
  const bool valid = calculateTemperatureC(rawAdc, temperatureC);

  SensorPacket packet{};
  packet.version = 1;
  packet.flags = valid ? 0x01 : 0x00;
  packet.sequence = ++readingSequence;
  packet.temperatureCentiC = valid
      ? static_cast<int16_t>(lroundf(temperatureC * 100.0F))
      : 0;
  packet.rawAdc = rawAdc;
  packet.uptimeMs = millis();

  readingCharacteristic->setValue(
      reinterpret_cast<uint8_t*>(&packet), sizeof(packet));
  readingCharacteristic->notify();

  Serial.println();
  Serial.print("Reading #");
  Serial.print(packet.sequence);
  Serial.print(" | Raw ADC: ");
  Serial.print(rawAdc);
  if (!valid) {
    Serial.println(" | Temperature: ERROR");
    return;
  }
  const float temperatureF = temperatureC * 9.0F / 5.0F + 32.0F;
  Serial.print(" | Temperature: ");
  Serial.print(temperatureC, 2);
  Serial.print(" C / ");
  Serial.print(temperatureF, 2);
  Serial.println(" F");
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
  pinMode(BUTTON_PIN, INPUT);  // TK04 includes a pull-down; pressed is HIGH.
  analogReadResolution(12);
  analogSetPinAttenuation(THERMISTOR_PIN, ADC_11db);
  configureBluetooth();

  delay(300);
  Serial.println("Ducati temperature sensor v6.2 ready.");
  Serial.println("Connect in WearabLLM, then press either Take reading or the physical button.");
}

void loop() {
  const bool sample = digitalRead(BUTTON_PIN) == HIGH;
  const uint32_t now = millis();

  if (measurementRequested) {
    measurementRequested = false;
    publishReading();
  }

  if (sample != buttonLastSample) {
    buttonLastSample = sample;
    buttonChangedAtMs = now;
  }

  if (sample != buttonStableState && now - buttonChangedAtMs >= DEBOUNCE_MS) {
    buttonStableState = sample;
    if (buttonStableState == HIGH) {
      publishReading();
    }
  }

  delay(2);
}
