// ESP32 "B" v1.5 — drives FOUR relays from CAN commands and confirms back to ESP-A.
//
// Ch1 (0x100): one-way — fires relay 1 + beeps buzzer, no response sent to ESP-A.
// Ch2–4 (0x101–0x103): request/response — fires relay, then sends confirmation
//   back to ESP-A (0x111–0x113) so ESP-A can fire its own relay.
//
// Push button (GPIO 26): sends CAN 0x110 to ESP-A on press/release.
//   ESP-A uses this to drive its Ch1 relay. No relay fires on ESP-B for this.
//
// Onboard LED (GPIO 2) blinks briefly on every incoming CAN message from ESP-A.
//
// Library: install "Adafruit MCP2515" via Arduino Library Manager
// (arduino-cli lib install "Adafruit MCP2515")
//
// Relay wiring (signal → relay IN; relay VCC/GND from 3.3V/GND):
//   Relay 1: GPIO 4   (CAN 0x100, one-way — no response)
//   Relay 2: GPIO 32  (CAN 0x101, responds with CAN 0x111)
//   Relay 3: GPIO 33  (CAN 0x102, responds with CAN 0x112)
//   Relay 4: GPIO 25  (CAN 0x103, responds with CAN 0x113)
//
// Push button wiring (one leg → GPIO 26, other leg → GND; INPUT_PULLUP):
//   Sends CAN 0x110: 0x01 = pressed, 0x00 = released.
//
// Passive piezo buzzer:
//   Signal → GPIO 13, + → 3.3V, − → GND
//   Beeps on Relay 1 ON events only.
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

#define CS_PIN               5
#define CAN_BAUDRATE         500000
#define RELAY_ACTIVE_LOW     false
#define HEARTBEAT_PERIOD_MS  2000

#define LED_PIN              2
#define LED_BLINK_MS         200

#define BUZZER_PIN           13
#define BUZZER_FREQ_HZ       1000
#define BUZZER_DURATION_MS   3000

#define BTN_PIN              26
#define CAN_ID_BTN           0x110   // B's button broadcasts here → ESP-A Ch1 relay

// #define MCP2515_CRYSTAL_8MHZ

Adafruit_MCP2515 mcp(CS_PIN);

struct RelayChannel {
  const char *name;
  uint8_t     pin;
  uint32_t    rxId;   // incoming CAN ID that activates this relay (A → B)
  uint32_t    txId;   // response CAN ID sent back to ESP-A (0 = one-way, no response)
  bool        isOn;
};

RelayChannel channels[] = {
  { "Relay 1", 4,  0x100, 0x000, false },
  { "Relay 2", 32, 0x101, 0x111, false },
  { "Relay 3", 33, 0x102, 0x112, false },
  { "Relay 4", 25, 0x103, 0x113, false },
};
const int NUM_CHANNELS = sizeof(channels) / sizeof(channels[0]);

unsigned long lastHeartbeat  = 0;
unsigned long packetsSeen    = 0;
unsigned long packetsMatched = 0;
unsigned long ledOnUntil     = 0;
bool          btnLastState   = HIGH;
bool          btnOn          = false;

void setRelay(RelayChannel &ch, bool on) {
  ch.isOn = on;
  bool level = RELAY_ACTIVE_LOW ? !on : on;
  digitalWrite(ch.pin, level ? HIGH : LOW);
  Serial.printf("[%8lu ms]     >>> %s -> %s (GPIO%d)\n",
                millis(), ch.name, on ? "ON " : "OFF", ch.pin);
  if (on && &ch == &channels[0]) {
    tone(BUZZER_PIN, BUZZER_FREQ_HZ, BUZZER_DURATION_MS);
  }
}

void blinkLed() {
  ledOnUntil = millis() + LED_BLINK_MS;
}

void sendCan(uint32_t id, uint8_t payload) {
  mcp.beginPacket(id);
  mcp.write(payload);
  int ok = mcp.endPacket();
  Serial.printf("[%8lu ms]     <<< CAN 0x%03lX data=0x%02X %s\n",
                millis(), (unsigned long)id, payload, ok ? "OK" : "FAILED");
}

int findChannel(uint32_t id) {
  for (int i = 0; i < NUM_CHANNELS; i++) {
    if (channels[i].rxId == id) return i;
  }
  return -1;
}

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("============================================");
  Serial.println("ESP32 B v1.5 — CAN relay receiver booting");
  Serial.println("============================================");
  Serial.printf("CS_PIN=%d  CAN_BAUDRATE=%ld  RELAY_ACTIVE_LOW=%s\n",
                CS_PIN, (long)CAN_BAUDRATE, RELAY_ACTIVE_LOW ? "true" : "false");

  for (int i = 0; i < NUM_CHANNELS; i++) {
    pinMode(channels[i].pin, OUTPUT);
    setRelay(channels[i], false);
    if (channels[i].txId) {
      Serial.printf("  %s: GPIO%d  rx=0x%03lX  tx=0x%03lX (request/response)\n",
                    channels[i].name, channels[i].pin,
                    (unsigned long)channels[i].rxId, (unsigned long)channels[i].txId);
    } else {
      Serial.printf("  %s: GPIO%d  rx=0x%03lX  tx=(none, one-way)\n",
                    channels[i].name, channels[i].pin,
                    (unsigned long)channels[i].rxId);
    }
  }

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  Serial.printf("  Onboard LED: GPIO%d (blinks on incoming CAN from ESP-A)\n", LED_PIN);

  pinMode(BUZZER_PIN, OUTPUT);
  Serial.printf("  Buzzer: GPIO%d  %d Hz  %d ms (Relay 1 ON only)\n",
                BUZZER_PIN, BUZZER_FREQ_HZ, BUZZER_DURATION_MS);

  pinMode(BTN_PIN, INPUT_PULLUP);
  btnLastState = digitalRead(BTN_PIN);
  Serial.printf("  Push button: GPIO%d  broadcasts CAN 0x%03X to ESP-A\n",
                BTN_PIN, CAN_ID_BTN);

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
  Serial.println("Ready. Waiting for CAN from ESP-A + local button input.");
  Serial.println();
}

void loop() {
  // --- Non-blocking LED ---
  digitalWrite(LED_PIN, millis() < ledOnUntil ? HIGH : LOW);

  // --- Poll push button ---
  bool cur = digitalRead(BTN_PIN);
  if (cur != btnLastState) {
    delay(30);
    cur = digitalRead(BTN_PIN);
    if (cur != btnLastState) {
      btnLastState = cur;
      btnOn = (cur == LOW);
      Serial.printf("[%8lu ms] BTN: %s -> CAN 0x%03X data=0x%02X\n",
                    millis(), btnOn ? "PRESSED " : "RELEASED", CAN_ID_BTN, btnOn ? 0x01 : 0x00);
      sendCan(CAN_ID_BTN, btnOn ? 0x01 : 0x00);
    }
  }

  // --- Receive CAN from ESP-A ---
  int packetSize = mcp.parsePacket();
  if (packetSize > 0 && !mcp.packetRtr()) {
    packetsSeen++;
    long id = mcp.packetId();
    uint8_t firstByte = 0;
    Serial.printf("[%8lu ms] RX #%lu: id=0x%03lX data=[", millis(), packetsSeen, id);
    for (int i = 0; i < packetSize; i++) {
      int b = mcp.read();
      if (i == 0) firstByte = (uint8_t)b;
      Serial.printf("%s0x%02X", i > 0 ? " " : "", b);
    }
    Serial.print("] + LED blink");

    int chIdx = findChannel((uint32_t)id);
    if (chIdx >= 0) {
      packetsMatched++;
      Serial.printf(" -> %s\n", channels[chIdx].name);
      bool on = (firstByte == 0x01);
      setRelay(channels[chIdx], on);
      blinkLed();
      if (channels[chIdx].txId) {
        sendCan(channels[chIdx].txId, firstByte);
      }
    } else {
      Serial.printf(" (ignored — id 0x%03lX not mapped)\n", id);
    }
  }

  // --- Heartbeat ---
  if (millis() - lastHeartbeat >= HEARTBEAT_PERIOD_MS) {
    lastHeartbeat = millis();
    Serial.printf("[%8lu ms] heartbeat — pkts seen: %lu | matched: %lu | btn: %s |",
                  millis(), packetsSeen, packetsMatched, btnOn ? "PRESSED" : "released");
    for (int i = 0; i < NUM_CHANNELS; i++) {
      Serial.printf(" %s=%s", channels[i].name, channels[i].isOn ? "ON " : "OFF");
    }
    Serial.println();
  }
}
