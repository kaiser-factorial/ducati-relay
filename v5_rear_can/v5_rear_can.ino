/*
 * Ducati rear lighting controller - v5
 *
 * Board: ESP32_Relay X4 (classic ESP32-WROOM-32)
 * CAN:   MCP2515, 500 kbit/s, standard 11-bit frames
 *
 * Protocol:
 *   0x300 data[0] = 0x01/0x00  Set left indicator ON/OFF
 *   0x301 data[0] = 0x01/0x00  Set right indicator ON/OFF
 *   0x303 data[0] = 0x01  Brake ON; 0x00 Brake OFF
 *   0x304 data[0] = 0x01/0x00  Set both indicators ON/OFF (hazards)
 *
 * Left and right are independent, so both can blink together.
 */

#include <Adafruit_MCP2515.h>
#include <ArduinoOTA.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiProv.h>

// Set true for relay/load bench testing without an MCP2515 connected.
// TEST MODE WIRING (each switch connects its GPIO to GND when pressed/closed):
//   Left switch  -> GPIO18
//   Right switch -> GPIO19
//   Brake switch -> GPIO13
// These are normally the CAN SPI pins, so REMOVE the test switches before setting this
// back to false and connecting the MCP2515.
constexpr bool TEST_MODE = false;

// ESP32_Relay X4 integrated relay inputs (active HIGH).
constexpr uint8_t RELAY_LEFT_PIN  = 32; // physical relay 1 / IN1
constexpr uint8_t RELAY_RIGHT_PIN = 33; // physical relay 2 / IN2
constexpr uint8_t RELAY_BRAKE_PIN = 25; // physical relay 3 / IN3
constexpr uint8_t RELAY_UNUSED_PIN = 26;
constexpr uint8_t CAN_STATUS_LED_PIN = 23; // onboard D14 status LED, active HIGH

// MCP2515 on the ESP32's standard VSPI pins.
constexpr uint8_t CAN_SCK_PIN  = 18;
constexpr uint8_t CAN_MISO_PIN = 19;
// GPIO23 drives the board's D14 status LED, so use free GPIO13 for MOSI instead.
constexpr uint8_t CAN_MOSI_PIN = 13;
constexpr uint8_t CAN_CS_PIN   = 5;

// In test mode, the future CAN pins become active-LOW switch inputs.
constexpr uint8_t TEST_LEFT_SWITCH_PIN  = CAN_SCK_PIN;  // GPIO18
constexpr uint8_t TEST_RIGHT_SWITCH_PIN = CAN_MISO_PIN; // GPIO19
constexpr uint8_t TEST_BRAKE_SWITCH_PIN = CAN_MOSI_PIN; // GPIO13

constexpr uint32_t CAN_LEFT_ID  = 0x300;
constexpr uint32_t CAN_RIGHT_ID = 0x301;
constexpr uint32_t CAN_BRAKE_ID = 0x303;
constexpr uint32_t CAN_HAZARD_ID = 0x304;

constexpr uint32_t CAN_BITRATE = 500000;
// Verified from the metal crystal marking on this MCP2515 module: "8.000".
constexpr uint32_t MCP2515_CLOCK_HZ = 8000000UL;

constexpr uint32_t BLINK_HALF_PERIOD_MS = 500; // 500 ms ON + 500 ms OFF = 60 flashes/min
constexpr uint32_t CAN_RX_LED_PULSE_MS = 100;
constexpr char OTA_HOSTNAME[] = "ducati-rear";

Adafruit_MCP2515 mcp(CAN_CS_PIN);

bool leftEnabled = false;
bool rightEnabled = false;
bool brakeEnabled = false;
bool blinkPhaseOn = false;
bool canReady = false;
bool otaInitialized = false;
uint32_t lastBlinkMs = 0;
uint32_t lastHeartbeatMs = 0;
uint32_t receivedFrames = 0;
uint32_t canRxLedStartedMs = 0;
bool canRxLedPulseActive = false;

void allRelaysOff() {
  digitalWrite(RELAY_LEFT_PIN, LOW);
  digitalWrite(RELAY_RIGHT_PIN, LOW);
  digitalWrite(RELAY_BRAKE_PIN, LOW);
  digitalWrite(RELAY_UNUSED_PIN, LOW);
}

void updateRelayOutputs() {
  digitalWrite(RELAY_LEFT_PIN, leftEnabled && blinkPhaseOn ? HIGH : LOW);
  digitalWrite(RELAY_RIGHT_PIN, rightEnabled && blinkPhaseOn ? HIGH : LOW);
  digitalWrite(RELAY_BRAKE_PIN, brakeEnabled ? HIGH : LOW);
}

void pulseCanStatusLed() {
  if (!canReady) {
    return;
  }
  canRxLedStartedMs = millis();
  canRxLedPulseActive = true;
  digitalWrite(CAN_STATUS_LED_PIN, HIGH);
}

void updateCanStatusLed(uint32_t now) {
  // A solid D14 means the MCP2515 did not initialize.
  if (!canReady) {
    digitalWrite(CAN_STATUS_LED_PIN, HIGH);
    return;
  }

  if (canRxLedPulseActive && now - canRxLedStartedMs >= CAN_RX_LED_PULSE_MS) {
    canRxLedPulseActive = false;
    digitalWrite(CAN_STATUS_LED_PIN, LOW);
  }
}

void printState() {
  Serial.printf("[STATE] left=%s right=%s brake=%s\n",
                leftEnabled ? "ON" : "OFF",
                rightEnabled ? "ON" : "OFF",
                brakeEnabled ? "ON" : "OFF");
}

void handleCanFrame(int packetSize) {
  const uint32_t id = mcp.packetId();
  const bool extended = mcp.packetExtended();
  const bool remote = mcp.packetRtr();

  uint8_t data[8] = {0};
  uint8_t length = 0;
  while (mcp.available() && length < sizeof(data)) {
    data[length++] = static_cast<uint8_t>(mcp.read());
  }

  receivedFrames++;
  pulseCanStatusLed();

  // Only accept ordinary 11-bit data frames with at least one byte.
  if (extended || remote || packetSize < 1 || length < 1) {
    return;
  }

  bool stateChanged = false;
  switch (id) {
    case CAN_LEFT_ID:
      if (data[0] == 0x00 || data[0] == 0x01) {
        leftEnabled = data[0] == 0x01;
        stateChanged = true;
      }
      break;

    case CAN_RIGHT_ID:
      if (data[0] == 0x00 || data[0] == 0x01) {
        rightEnabled = data[0] == 0x01;
        stateChanged = true;
      }
      break;

    case CAN_BRAKE_ID:
      if (data[0] == 0x00 || data[0] == 0x01) {
        brakeEnabled = data[0] == 0x01;
        stateChanged = true;
      }
      break;

    case CAN_HAZARD_ID:
      if (data[0] == 0x00 || data[0] == 0x01) {
        leftEnabled = data[0] == 0x01;
        rightEnabled = data[0] == 0x01;
        stateChanged = true;
      }
      break;

    default:
      break;
  }

  if (stateChanged) {
    // Start a newly enabled indicator with an immediate visible ON phase.
    if ((id == CAN_LEFT_ID && leftEnabled) || (id == CAN_RIGHT_ID && rightEnabled)) {
      blinkPhaseOn = true;
      lastBlinkMs = millis();
    }
    updateRelayOutputs();
    Serial.printf("[CAN] ID=0x%03lX data[0]=0x%02X\n",
                  static_cast<unsigned long>(id), data[0]);
    printState();
  }
}

void setupCan() {
  SPI.begin(CAN_SCK_PIN, CAN_MISO_PIN, CAN_MOSI_PIN, CAN_CS_PIN);
  mcp.setClockFrequency(MCP2515_CLOCK_HZ);

  if (!mcp.begin(CAN_BITRATE)) {
    Serial.println("[CAN] ERROR: MCP2515 initialization failed; relays remain OFF.");
    return;
  }

  canReady = true;
  digitalWrite(CAN_STATUS_LED_PIN, LOW);
  Serial.printf("[CAN] Ready: 500 kbit/s, MCP2515 clock %lu Hz\n",
                static_cast<unsigned long>(MCP2515_CLOCK_HZ));
}

void setupTestMode() {
  pinMode(TEST_LEFT_SWITCH_PIN, INPUT_PULLUP);
  pinMode(TEST_RIGHT_SWITCH_PIN, INPUT_PULLUP);
  pinMode(TEST_BRAKE_SWITCH_PIN, INPUT_PULLUP);
  Serial.println("[TEST] TEST MODE ACTIVE - MCP2515/CAN disabled.");
  Serial.println("[TEST] GPIO18=left, GPIO19=right, GPIO13=brake; switch each pin to GND.");
}

void updateTestMode() {
  const bool newLeft = digitalRead(TEST_LEFT_SWITCH_PIN) == LOW;
  const bool newRight = digitalRead(TEST_RIGHT_SWITCH_PIN) == LOW;
  const bool newBrake = digitalRead(TEST_BRAKE_SWITCH_PIN) == LOW;

  if (newLeft == leftEnabled && newRight == rightEnabled && newBrake == brakeEnabled) {
    return;
  }

  const bool indicatorJustEnabled =
      (newLeft && !leftEnabled) || (newRight && !rightEnabled);

  leftEnabled = newLeft;
  rightEnabled = newRight;
  brakeEnabled = newBrake;

  // Start an activated indicator with an immediate visible ON phase. The loop's millis()
  // timer handles all subsequent blinking without blocking CAN, Wi-Fi, or OTA work.
  if (indicatorJustEnabled) {
    blinkPhaseOn = true;
    lastBlinkMs = millis();
  }

  updateRelayOutputs();
  printState();
}

void provisioningEvent(arduino_event_t *event) {
  switch (event->event_id) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.print("[WIFI] Connected: ");
      Serial.println(WiFi.localIP());
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.println("[WIFI] Disconnected (CAN lighting continues locally).");
      break;
    default:
      break;
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== Ducati Rear Lighting Controller v5 ===");

  pinMode(RELAY_LEFT_PIN, OUTPUT);
  pinMode(RELAY_RIGHT_PIN, OUTPUT);
  pinMode(RELAY_BRAKE_PIN, OUTPUT);
  pinMode(RELAY_UNUSED_PIN, OUTPUT);
  pinMode(CAN_STATUS_LED_PIN, OUTPUT);
  allRelaysOff(); // fail-safe boot state
  digitalWrite(CAN_STATUS_LED_PIN, HIGH); // stays solid if CAN initialization fails

  if (TEST_MODE) {
    setupTestMode();
  } else {
    setupCan();
  }

  // Retain v4 provisioning and OTA so future updates can stay wireless.
  WiFi.onEvent(provisioningEvent);
  WiFiProv.beginProvision(
      NETWORK_PROV_SCHEME_BLE,
      NETWORK_PROV_SCHEME_HANDLER_FREE_BTDM,
      NETWORK_PROV_SECURITY_1,
      "12345678", "Ducati_Rear_Module");
}

void loop() {
  if (TEST_MODE) {
    updateTestMode();
  } else if (canReady) {
    const int packetSize = mcp.parsePacket();
    if (packetSize > 0) {
      handleCanFrame(packetSize);
    }
  }

  const uint32_t now = millis();
  updateCanStatusLed(now);
  if (now - lastBlinkMs >= BLINK_HALF_PERIOD_MS) {
    lastBlinkMs = now;
    blinkPhaseOn = !blinkPhaseOn;
    updateRelayOutputs();
  }

  if (WiFi.status() == WL_CONNECTED) {
    if (!otaInitialized) {
      ArduinoOTA.setHostname(OTA_HOSTNAME);
      ArduinoOTA.begin();
      otaInitialized = true;
      Serial.println("[OTA] Ready at ducati-rear.local");
    }
    ArduinoOTA.handle();
  }

  if (now - lastHeartbeatMs >= 2000) {
    lastHeartbeatMs = now;
    Serial.printf("[HB] mode=%s CAN=%s frames=%lu WiFi=%s\n",
                  TEST_MODE ? "TEST" : "CAN",
                  TEST_MODE ? "disabled" : (canReady ? "ready" : "FAILED"),
                  static_cast<unsigned long>(receivedFrames),
                  WiFi.status() == WL_CONNECTED ? "connected" : "offline");
  }
}
