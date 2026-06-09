// ESP32 "A" v1.5 — reads FOUR pushbuttons and coordinates with ESP-B via CAN.
//
// Button presses are sent over CAN to ESP-B. Local relays fire ONLY after
// ESP-B sends back a confirmation — CAN is the single source of truth for
// relay state (mirrors having an ECU in the middle on the actual bike).
//
// Ch1 (GPIO 32) → CAN 0x100 → ESP-B fires relay 1 + buzzer (one-way, no response).
//   Ch1 local relay is driven by ESP-B's push button via CAN 0x110.
//
// Ch2–4 (GPIO 33/25/26) → CAN 0x101–0x103 → ESP-B confirms via CAN 0x111–0x113
//   → local relay fires on ESP-A only after confirmation arrives.
//
// Onboard LED (GPIO 2) blinks briefly on every incoming CAN message from ESP-B.
//
// Library: install "Adafruit MCP2515" via Arduino Library Manager
// (arduino-cli lib install "Adafruit MCP2515")
//
// Button wiring (one leg → GPIO, other leg → GND; INPUT_PULLUP):
//   Button 1 → GPIO 32  (sends CAN 0x100)
//   Button 2 → GPIO 33  (sends CAN 0x101)
//   Button 3 → GPIO 25  (sends CAN 0x102)
//   Button 4 → GPIO 26  (sends CAN 0x103)
//
// Relay wiring (signal → relay IN; relay VCC/GND from 3.3V/GND):
//   Relay 1 → GPIO 17  (fires when ESP-B's push button is pressed, via CAN 0x110)
//   Relay 2 → GPIO 16  (fires when ESP-B confirms via CAN 0x111)
//   Relay 3 → GPIO 14  (fires when ESP-B confirms via CAN 0x112)
//   Relay 4 → GPIO 27  (fires when ESP-B confirms via CAN 0x113)
//
// MCP2515 wiring (VSPI):
//   VCC → 3.3V, GND → GND
//   SCK → GPIO 18, MISO → GPIO 19, MOSI → GPIO 23, CS → GPIO 5
//
// NOTE: many cheap relay modules are "active LOW". If relays turn on when
// they should be off, flip RELAY_ACTIVE_LOW to true.
//
// IMPORTANT: library assumes a 16 MHz crystal on the MCP2515 by default.
// If your module's oscillator can is marked 8 MHz, uncomment MCP2515_CRYSTAL_8MHZ.

#include <Adafruit_MCP2515.h>

#define CS_PIN              5
#define CAN_BAUDRATE        500000
#define RELAY_ACTIVE_LOW    false
#define HEARTBEAT_PERIOD_MS 2000

#define LED_PIN             2
#define LED_BLINK_MS        200

// #define MCP2515_CRYSTAL_8MHZ

Adafruit_MCP2515 mcp(CS_PIN);

struct Channel {
  const char *name;
  uint8_t     btnPin;
  uint8_t     relayPin;
  uint32_t    txId;          // CAN ID sent when button changes (A → B)
  uint32_t    rxId;          // CAN ID that fires this relay (B → A)
  bool        lastBtnState;
  bool        relayOn;
};

//                  name   btn  relay  txId   rxId   btnState  relay
Channel channels[] = {
  { "Ch1", 32, 17, 0x100, 0x110, HIGH, false },
  { "Ch2", 33, 16, 0x101, 0x111, HIGH, false },
  { "Ch3", 25, 14, 0x102, 0x112, HIGH, false },
  { "Ch4", 26, 27, 0x103, 0x113, HIGH, false },
};
const int NUM_CHANNELS = sizeof(channels) / sizeof(channels[0]);

unsigned long lastHeartbeat = 0;
unsigned long sendCount     = 0;
unsigned long sendFailCount = 0;
unsigned long ledOnUntil    = 0;

void setRelay(Channel &ch, bool on) {
  ch.relayOn = on;
  bool level = RELAY_ACTIVE_LOW ? !on : on;
  digitalWrite(ch.relayPin, level ? HIGH : LOW);
}

void blinkLed() {
  ledOnUntil = millis() + LED_BLINK_MS;
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
    Serial.printf("  %s: btn=GPIO%d  relay=GPIO%d  tx=0x%03lX  rx=0x%03lX\n",
                  channels[i].name, channels[i].btnPin, channels[i].relayPin,
                  (unsigned long)channels[i].txId, (unsigned long)channels[i].rxId);
  }

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  Serial.printf("  Onboard LED: GPIO%d (blinks on incoming CAN from ESP-B)\n", LED_PIN);

#ifdef MCP2515_CRYSTAL_8MHZ
  Serial.println("Setting MCP2515 clock to 8 MHz");
  mcp.setClockFrequency(8e6);
#endif

  Serial.println("Calling mcp.begin() ...");
  if (!mcp.begin(CAN_BAUDRATE)) {
    Serial.println("!!! mcp.begin() FAILED — check wiring / MCP2515_CRYSTAL_8MHZ.");
    while (1) delay(10);
  }
  Serial.println("mcp.begin() succeeded.");
  mcp.dumpRegisters(Serial);
  Serial.println("Ready. Buttons send CAN to ESP-B; relays fire only on CAN response.");
  Serial.println();
}

void onButtonChange(Channel &ch, bool pressed) {
  uint8_t payload = pressed ? 0x01 : 0x00;
  Serial.printf("[%8lu ms] >>> %s: %s -> CAN 0x%03lX data=0x%02X (awaiting response)\n",
                millis(), ch.name, pressed ? "PRESSED " : "RELEASED",
                (unsigned long)ch.txId, payload);

  mcp.beginPacket(ch.txId);
  mcp.write(payload);
  int ok = mcp.endPacket();

  sendCount++;
  if (!ok) {
    sendFailCount++;
    Serial.printf("[%8lu ms]     send #%lu FAILED (total failures: %lu)\n",
                  millis(), sendCount, sendFailCount);
  } else {
    Serial.printf("[%8lu ms]     send #%lu OK\n", millis(), sendCount);
  }
}

void loop() {
  // --- Non-blocking LED ---
  digitalWrite(LED_PIN, millis() < ledOnUntil ? HIGH : LOW);

  // --- Poll buttons ---
  for (int i = 0; i < NUM_CHANNELS; i++) {
    Channel &ch = channels[i];
    bool cur = digitalRead(ch.btnPin);
    if (cur != ch.lastBtnState) {
      delay(30);
      cur = digitalRead(ch.btnPin);
      if (cur != ch.lastBtnState) {
        ch.lastBtnState = cur;
        onButtonChange(ch, cur == LOW);
      }
    }
  }

  // --- Receive CAN from ESP-B (responses + B's button event) ---
  int packetSize = mcp.parsePacket();
  if (packetSize > 0 && !mcp.packetRtr()) {
    long id = mcp.packetId();
    uint8_t firstByte = 0;
    for (int i = 0; i < packetSize; i++) {
      int b = mcp.read();
      if (i == 0) firstByte = (uint8_t)b;
    }

    bool matched = false;
    for (int i = 0; i < NUM_CHANNELS; i++) {
      if ((uint32_t)id == channels[i].rxId) {
        bool on = (firstByte == 0x01);
        Serial.printf("[%8lu ms] <<< CAN 0x%03lX -> %s relay %s + LED blink\n",
                      millis(), id, channels[i].name, on ? "ON " : "OFF");
        setRelay(channels[i], on);
        blinkLed();
        matched = true;
        break;
      }
    }
    if (!matched) {
      Serial.printf("[%8lu ms] RX id=0x%03lX (ignored)\n", millis(), id);
    }
  }

  // --- Heartbeat ---
  if (millis() - lastHeartbeat >= HEARTBEAT_PERIOD_MS) {
    lastHeartbeat = millis();
    Serial.printf("[%8lu ms] heartbeat — sends ok: %lu | failed: %lu |",
                  millis(), sendCount - sendFailCount, sendFailCount);
    for (int i = 0; i < NUM_CHANNELS; i++) {
      Serial.printf(" %s relay=%s", channels[i].name, channels[i].relayOn ? "ON " : "OFF");
    }
    Serial.println();
  }
}
