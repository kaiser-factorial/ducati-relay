/*
  v6_temperature_sensor

  Lonely Binary ESP32-S3 with:
    TK12 NTC thermistor Signal -> GPIO 6
    TK04 push button Signal    -> GPIO 37

  Press the button or send "r" / "read" over Serial to take one temperature
  reading. Results are printed to Serial Monitor at 115200 baud.
*/

#include <Arduino.h>
#include <ctype.h>
#include <math.h>
#include <string.h>

namespace {

constexpr uint8_t THERMISTOR_PIN = 6;
constexpr uint8_t BUTTON_PIN = 37;

constexpr uint16_t ADC_MAX = 4095;
constexpr uint8_t SAMPLE_COUNT = 24;
constexpr uint16_t SAMPLE_INTERVAL_MS = 4;
constexpr uint16_t DEBOUNCE_MS = 45;
constexpr uint16_t SERIAL_COMMAND_IDLE_MS = 120;
constexpr size_t SERIAL_COMMAND_CAPACITY = 16;

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
char serialCommand[SERIAL_COMMAND_CAPACITY]{};
size_t serialCommandLength = 0;
uint32_t serialLastByteAtMs = 0;

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

void runSerialCommand() {
  if (serialCommandLength == 0) return;
  serialCommand[serialCommandLength] = '\0';

  if (strcmp(serialCommand, "r") == 0 || strcmp(serialCommand, "read") == 0) {
    takeAndPrintReading();
  } else if (strcmp(serialCommand, "h") == 0 || strcmp(serialCommand, "help") == 0) {
    Serial.println("Commands: r or read = take a temperature reading");
  } else {
    Serial.print("Unknown command: ");
    Serial.println(serialCommand);
    Serial.println("Send r to take a reading, or h for help.");
  }

  serialCommandLength = 0;
}

void handleSerialInput(uint32_t now) {
  while (Serial.available() > 0) {
    const char incoming = static_cast<char>(Serial.read());
    serialLastByteAtMs = now;

    if (incoming == '\r' || incoming == '\n') {
      runSerialCommand();
      continue;
    }
    if (isspace(static_cast<unsigned char>(incoming))) continue;

    if (serialCommandLength < SERIAL_COMMAND_CAPACITY - 1) {
      serialCommand[serialCommandLength++] =
          static_cast<char>(tolower(static_cast<unsigned char>(incoming)));
    } else {
      serialCommandLength = 0;
      Serial.println("Serial command too long. Send r to take a reading.");
    }
  }

  // Also supports Serial Monitor with "No line ending" selected.
  if (serialCommandLength > 0 && now - serialLastByteAtMs >= SERIAL_COMMAND_IDLE_MS) {
    runSerialCommand();
  }
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
  Serial.println("Press the TinkerBlock button or send r to take a reading.");
}

void loop() {
  const bool sample = digitalRead(BUTTON_PIN) == HIGH;
  const uint32_t now = millis();

  handleSerialInput(now);

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
