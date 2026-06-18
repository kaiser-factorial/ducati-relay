/*
 * Ducati Rear Module - Bootstrap v1.0
 * Purpose: BLE Provisioning & OTA Readiness
 * LED Status: Blinking = Waiting for Provisioning, OFF = Connected
 */

#include <Adafruit_MCP2515.h>
#include <ArduinoOTA.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiProv.h>

#define LED_PIN 2

// Timer for non-blocking blink
unsigned long previousMillis = 0;
const long interval = 750; // 500ms blink rate
int ledState = LOW;
bool otaInitialized = false;

void setup() {
  pinMode(LED_PIN, OUTPUT);

  // Use the explicit enum types that the compiler recognizes
  WiFiProv.beginProvision(
      (prov_scheme_t)WIFI_PROV_SCHEME_BLE,      // Cast to the scheme enum
      (scheme_handler_t)WIFI_PROV_HANDLER_NONE, // Cast to the handler enum
      (network_prov_security_t)
          WIFI_PROV_SECURITY_1, // Cast to the security enum
      "12345678", "Ducati_Rear_Module");
}

void loop() {
  // If not connected, keep the LED blinking to indicate "Ready to Provision"
  if (WiFi.status() != WL_CONNECTED) {
    unsigned long currentMillis = millis();
    if (currentMillis - previousMillis >= interval) {
      previousMillis = currentMillis;
      ledState = (ledState == LOW) ? HIGH : LOW;
      digitalWrite(LED_PIN, ledState);
    }
  } else {
    // Once connected, turn LED off
    digitalWrite(LED_PIN, LOW);

    // Initialize OTA only once after WiFi connects
    if (!otaInitialized) {
      ArduinoOTA.begin();
      otaInitialized = true;
    }

    // OTA Handler
    ArduinoOTA.handle();
  }
}