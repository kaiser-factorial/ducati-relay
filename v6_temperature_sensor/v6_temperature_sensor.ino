/*
  v6_temperature_sensor

  Lonely Binary ESP32-S3 with:
    TK12 NTC thermistor Signal -> GPIO 6
    TK04 push button Signal    -> GPIO 37

  Press the button once to take one temperature reading. Results are printed
  to Serial Monitor at 115200 baud.
*/

#include <Arduino.h>
#include <math.h>

namespace {

constexpr uint8_t THERMISTOR_PIN = 6;
constexpr uint8_t BUTTON_PIN = 37;

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

bool buttonStableState = LOW;
bool buttonLastSample = LOW;
uint32_t buttonChangedAtMs = 0;
uint32_t readingNumber = 0;

uint16_t readAveragedAdc() {
  uint32_t total = 0;
  for (uint8_t i = 0; i < SAMPLE_COUNT; ++i) {
    total += analogRead(THERMISTOR_PIN);
    delay(SAMPLE_INTERVAL_MS);
  }
  return static_cast<uint16_t>((total + (SAMPLE_COUNT / 2)) / SAMPLE_COUNT);
}

bool calculateTemperatureC(uint16_t rawAdc, float& temperatureC) {
  // A value near either ADC rail usually means an unplugged, shorted, or
  // incorrectly connected thermistor.
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

void takeAndPrintReading() {
  const uint16_t rawAdc = readAveragedAdc();
  float temperatureC = 0.0F;
  const bool valid = calculateTemperatureC(rawAdc, temperatureC);
  ++readingNumber;

  Serial.println();
  Serial.print("Reading #");
  Serial.println(readingNumber);
  Serial.print("Raw ADC: ");
  Serial.println(rawAdc);

  if (!valid) {
    Serial.println("Temperature: ERROR");
    Serial.println("Check that the thermistor Signal is connected to GPIO 6.");
    return;
  }

  const float temperatureF = temperatureC * 9.0F / 5.0F + 32.0F;
  Serial.print("Temperature: ");
  Serial.print(temperatureC, 2);
  Serial.print(" C / ");
  Serial.print(temperatureF, 2);
  Serial.println(" F");
}

}  // namespace

void setup() {
  Serial.begin(115200);
  pinMode(THERMISTOR_PIN, INPUT);
  pinMode(BUTTON_PIN, INPUT);  // TK04 has a pull-down; pressed is HIGH.
  analogReadResolution(12);
  analogSetPinAttenuation(THERMISTOR_PIN, ADC_11db);

  delay(300);
  Serial.println("Ducati temperature sensor ready.");
  Serial.println("Press the TinkerBlock button to take a reading.");
}

void loop() {
  const bool sample = digitalRead(BUTTON_PIN) == HIGH;
  const uint32_t now = millis();

  if (sample != buttonLastSample) {
    buttonLastSample = sample;
    buttonChangedAtMs = now;
  }

  if (sample != buttonStableState && now - buttonChangedAtMs >= DEBOUNCE_MS) {
    buttonStableState = sample;
    if (buttonStableState == HIGH) {
      takeAndPrintReading();
    }
  }

  delay(2);
}
