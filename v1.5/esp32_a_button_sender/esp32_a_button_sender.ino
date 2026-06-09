// ESP32 "A" v1.5 — reads FOUR pushbuttons, drives a local relay per button,
// and broadcasts each button's state over CAN via an MCP2515 SPI CAN controller
// module (Adafruit MCP2515 library). Also listens for CAN 0x110, the
// brake-state broadcast from ESP-B, and logs whether the brake is applied.
//
// Library: install "Adafruit MCP2515" via Arduino Library Manager
// (arduino-cli lib install "Adafruit MCP2515")
//
// Button wiring (one leg → GPIO, other leg → GND; internal pull-up, no external
// resistor needed):
//   Button 1 → GPIO 32  (sends CAN ID 0x100)
//   Button 2 → GPIO 33  (sends CAN ID 0x101)
//   Button 3 → GPIO 25  (sends CAN ID 0x102)
//   Button 4 → GPIO 26  (sends CAN ID 0x103)
//
// Relay wiring (signal pin → relay module IN; relay module VCC/GND from 3.3V/GND):
//   Relay 1 → GPIO 17  (follows Button 1)
//   Relay 2 → GPIO 16  (follows Button 2)
//   Relay 3 → GPIO 14  (follows Button 3)
//   Relay 4 → GPIO 27  (follows Button 4)
//
// MCP2515 wiring (same VSPI pins as v1):
//   VCC → 3.3V, GND → GND
//   SCK → GPIO 18, MISO → GPIO 19, MOSI → GPIO 23, CS → GPIO 5
//
// NOTE: many cheap relay modules are "active LOW" (energize when signal is LOW).
// If relays turn on when buttons are released instead of pressed, flip
// RELAY_ACTIVE_LOW to true.
//
// IMPORTANT: library assumes a 16 MHz crystal on the MCP2515 by default.
// If your module's oscillator can is marked 8 MHz, uncomment MCP2515_CRYSTAL_8MHZ.

#include <Adafruit_MCP2515.h>

#define CS_PIN              5
#define CAN_BAUDRATE        500000
#define RELAY_ACTIVE_LOW    false
#define HEARTBEAT_PERIOD_MS 2000

#define CAN_ID_BRAKE        0x110   // ESP-B broadcasts brake state here

// #define MCP2515_CRYSTAL_8MHZ

Adafruit_MCP2515 mcp(CS_PIN);

struct Channel {
  const char *name;
  uint8_t     btnPin;
  uint8_t     relayPin;
  uint32_t    canId;
  bool        lastBtnState;
  bool        relayOn;
};

Channel channels[] = {
  { "Ch1", 32, 17, 0x100, HIGH, false },
  { "Ch2", 33, 16, 0x101, HIGH, false },
  { "Ch3", 25, 14, 0x102, HIGH, false },
  { "Ch4", 26, 27, 0x103, HIGH, false },
};
const int NUM_CHANNELS = sizeof(channels) / sizeof(channels[0]);

unsigned long lastHeartbeat = 0;
unsigned long sendCount     = 0;
unsigned long sendFailCount = 0;
bool          brakeOn       = false;

void setRelay(Channel &ch, bool on) {
  ch.relayOn = on;
  bool level = RELAY_ACTIVE_LOW ? !on : on;
  digitalWrite(ch.relayPin, level ? HIGH : LOW);
}

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("==========================================");
  Serial.println("ESP32 A v1.5 — CAN button sender booting");
  Serial.println("==========================================");
  Serial.printf("CS_PIN=%d  CAN_BAUDRATE=%ld  RELAY_ACTIVE_LOW=%s\n",
                CS_PIN, (long)CAN_BAUDRATE, RELAY_ACTIVE_LOW ? "true" : "false");

  for (int i = 0; i < NUM_CHANNELS; i++) {
    pinMode(channels[i].btnPin, INPUT_PULLUP);
    pinMode(channels[i].relayPin, OUTPUT);
    setRelay(channels[i], false);
    Serial.printf("  %s: btn=GPIO%d  relay=GPIO%d  canId=0x%lX  btn=%s\n",
                  channels[i].name, channels[i].btnPin, channels[i].relayPin,
                  (unsigned long)channels[i].canId,
                  digitalRead(channels[i].btnPin) == LOW ? "LOW (pressed)" : "HIGH (released)");
  }

#ifdef MCP2515_CRYSTAL_8MHZ
  Serial.println("Setting MCP2515 clock frequency to 8 MHz");
  mcp.setClockFrequency(8e6);
#else
  Serial.println("Using default MCP2515 clock frequency assumption (16 MHz)");
#endif

  Serial.println("Calling mcp.begin() ...");
  if (!mcp.begin(CAN_BAUDRATE)) {
    Serial.println("!!! mcp.begin() FAILED — MCP2515 not found or not responding.");
    Serial.println("!!! Check: CS/SCK/MISO/MOSI wiring, module power, MCP2515_CRYSTAL_8MHZ.");
    while (1) delay(10);
  }
  Serial.println("mcp.begin() succeeded.");

  Serial.println("--- MCP2515 register dump ---");
  mcp.dumpRegisters(Serial);
  Serial.println("-----------------------------");

  Serial.println("Ready. Buttons drive local relays + send CAN; watching brake state from ESP-B.");
  Serial.println();
}

void onButtonChange(Channel &ch, bool pressed) {
  setRelay(ch, pressed);

  uint8_t payload = pressed ? 0x01 : 0x00;
  Serial.printf("[%8lu ms] >>> %s: %s -> relay %s, CAN 0x%lX data=0x%02X\n",
                millis(), ch.name, pressed ? "PRESSED " : "RELEASED",
                pressed ? "ON " : "OFF", (unsigned long)ch.canId, payload);

  mcp.beginPacket(ch.canId);
  mcp.write(payload);
  int ok = mcp.endPacket();

  sendCount++;
  if (ok) {
    Serial.printf("[%8lu ms]     send #%lu: OK\n", millis(), sendCount);
  } else {
    sendFailCount++;
    Serial.printf("[%8lu ms]     send #%lu: FAILED — total failures: %lu\n",
                  millis(), sendCount, sendFailCount);
  }
}

void loop() {
  // --- Poll buttons ---
  for (int i = 0; i < NUM_CHANNELS; i++) {
    Channel &ch = channels[i];
    bool cur = digitalRead(ch.btnPin);
    if (cur != ch.lastBtnState) {
      delay(30); // debounce
      cur = digitalRead(ch.btnPin);
      if (cur != ch.lastBtnState) {
        ch.lastBtnState = cur;
        onButtonChange(ch, cur == LOW);
      }
    }
  }

  // --- Receive CAN frames (pot selection from ESP-B) ---
  int packetSize = mcp.parsePacket();
  if (packetSize > 0) {
    long id = mcp.packetId();
    uint8_t firstByte = 0;
    for (int i = 0; i < packetSize; i++) {
      int b = mcp.read();
      if (i == 0) firstByte = (uint8_t)b;
    }

    if ((uint32_t)id == CAN_ID_BRAKE && !mcp.packetRtr()) {
      bool applied = (firstByte == 0x01);
      if (applied != brakeOn) {
        brakeOn = applied;
        Serial.printf("[%8lu ms] BRAKE from ESP-B: %s\n",
                      millis(), applied ? "APPLIED" : "released");
      }
    } else {
      Serial.printf("[%8lu ms] RX id=0x%lX (ignored)\n", millis(), id);
    }
  }

  // --- Heartbeat ---
  if (millis() - lastHeartbeat >= HEARTBEAT_PERIOD_MS) {
    lastHeartbeat = millis();
    Serial.printf("[%8lu ms] heartbeat — sends ok: %lu | failed: %lu | brake: %s |",
                  millis(), sendCount - sendFailCount, sendFailCount,
                  brakeOn ? "APPLIED" : "released");
    for (int i = 0; i < NUM_CHANNELS; i++) {
      Serial.printf(" %s btn=%s relay=%s",
                    channels[i].name,
                    digitalRead(channels[i].btnPin) == LOW ? "LOW " : "HIGH",
                    channels[i].relayOn ? "ON " : "OFF");
    }
    Serial.println();
  }
}
