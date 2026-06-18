/*
 * Ducati Rear Module - Bootstrap v1.0
 * Purpose: BLE Provisioning & OTA Readiness
 * Board: ESP32_Relay X4 (AC/DC powered) - ESP32-WROOM-32, 4x Songle relays
 * LED Status: Blinking = Waiting for Provisioning, OFF = Connected
 * OTA: reachable wirelessly as "ducati-rear.local"
 * Serial: 115200 baud for debug
 */

#include <Adafruit_MCP2515.h>
#include <ArduinoOTA.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiProv.h>

// --- Status LEDs ---
// This relay board has NO LED on GPIO2 (that was the old DevKit pin). GPIO23 is the
// documented onboard status LED for the ESP32_Relay X4; we drive it as a best guess
// (harmless if nothing is wired there). LED_EXT is your external LED + resistor.
#define LED_ONBOARD 23 // estimated onboard status LED (ESP32_Relay X4)
#define LED_EXT 13     // external LED: anode -> GPIO13 -> resistor -> LED -> GND

// --- Relay outputs (documented ESP32_Relay X4 mapping; verify before driving loads) ---
// Defined for reference / future use; not driven in this bootstrap sketch.
#define RELAY_1 32
#define RELAY_2 33
#define RELAY_3 25
#define RELAY_4 26

// OTA hostname -> board is reachable as "ducati-rear.local" regardless of its DHCP IP
#define OTA_HOSTNAME "ducati-rear"

// Timer for non-blocking blink
unsigned long previousMillis = 0;
const long interval = 250; // fast blink so it's obvious
int ledState = LOW;
bool otaInitialized = false;

// Heartbeat so we can confirm loop() is alive over serial
unsigned long lastHeartbeat = 0;

// Drive both status LEDs together
void setLeds(int state) {
  digitalWrite(LED_ONBOARD, state);
  digitalWrite(LED_EXT, state);
}

// Print every WiFi/provisioning event so we can see exactly what the chip is doing
void SysProvEvent(arduino_event_t *sys_event) {
  switch (sys_event->event_id) {
    case ARDUINO_EVENT_PROV_START:
      Serial.println("[PROV] BLE provisioning started.");
      Serial.println("[PROV] Open ESP BLE Provisioning app -> 'Ducati_Rear_Module', PoP '12345678'");
      break;
    case ARDUINO_EVENT_PROV_CRED_RECV:
      Serial.print("[PROV] Received WiFi credentials. SSID: ");
      Serial.println((const char *)sys_event->event_info.prov_cred_recv.ssid);
      break;
    case ARDUINO_EVENT_PROV_CRED_FAIL:
      Serial.println("[PROV] Credential check FAILED (wrong WiFi password?).");
      break;
    case ARDUINO_EVENT_PROV_CRED_SUCCESS:
      Serial.println("[PROV] Credentials applied successfully.");
      break;
    case ARDUINO_EVENT_PROV_END:
      Serial.println("[PROV] Provisioning finished.");
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.print("[WIFI] Connected. IP: ");
      Serial.println(WiFi.localIP());
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.println("[WIFI] Disconnected.");
      break;
    default:
      break;
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000); // give the USB serial monitor time to attach
  Serial.println();
  Serial.println("=== Ducati Rear Module Bootstrap v1.0 ===");
  Serial.println("[BOOT] setup() running.");

  pinMode(LED_ONBOARD, OUTPUT);
  pinMode(LED_EXT, OUTPUT);

  // LED self-test: 5 quick flashes. Distinct count doubles as OTA proof -- after a
  // wireless update reboots the board, seeing 5 flashes confirms the new firmware ran.
  Serial.println("[BOOT] LED self-test (5 flashes on GPIO23 + GPIO13)...");
  for (int i = 0; i < 5; i++) {
    setLeds(HIGH);
    delay(150);
    setLeds(LOW);
    delay(150);
  }

  WiFi.onEvent(SysProvEvent);

  // ESP32 core 3.x renamed these constants from WIFI_PROV_* to NETWORK_PROV_*
  Serial.println("[BOOT] Starting BLE provisioning...");
  WiFiProv.beginProvision(
      NETWORK_PROV_SCHEME_BLE,               // BLE provisioning scheme
      NETWORK_PROV_SCHEME_HANDLER_FREE_BTDM, // free BT/BLE memory after provisioning
      NETWORK_PROV_SECURITY_1,               // secured session with proof-of-possession
      "12345678", "Ducati_Rear_Module");
  Serial.println("[BOOT] beginProvision() returned. Entering loop().");
}

void loop() {
  // Heartbeat every 2s: proves the loop is running and shows current WiFi state
  if (millis() - lastHeartbeat >= 2000) {
    lastHeartbeat = millis();
    Serial.print("[HB] WiFi.status()=");
    Serial.print(WiFi.status());
    Serial.println(WiFi.status() == WL_CONNECTED ? " (CONNECTED)" : " (not connected - LEDs should be blinking)");
  }

  // If not connected, keep the LEDs blinking to indicate "Ready to Provision"
  if (WiFi.status() != WL_CONNECTED) {
    unsigned long currentMillis = millis();
    if (currentMillis - previousMillis >= interval) {
      previousMillis = currentMillis;
      ledState = (ledState == LOW) ? HIGH : LOW;
      setLeds(ledState);
    }
  } else {
    // Once connected, turn LEDs off
    setLeds(LOW);

    // Initialize OTA only once after WiFi connects
    if (!otaInitialized) {
      ArduinoOTA.setHostname(OTA_HOSTNAME); // advertise as ducati-rear.local
      ArduinoOTA.begin();
      otaInitialized = true;
      Serial.print("[OTA] ArduinoOTA started. Reachable as ");
      Serial.print(OTA_HOSTNAME);
      Serial.println(".local");
    }

    // OTA Handler
    ArduinoOTA.handle();
  }
}
