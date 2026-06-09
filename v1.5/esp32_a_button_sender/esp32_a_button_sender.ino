// ESP32 "A" v1.5 — reads FOUR pushbuttons and broadcasts each one's state over
// CAN via an MCP2515 SPI CAN controller module (Adafruit MCP2515 library).
// Also listens for CAN 0x110, the potentiometer-selection broadcast from ESP-B,
// and logs which relay ESP-B is currently pointing at with its pot.
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
// MCP2515 wiring (same VSPI pins as v1):
//   VCC → 3.3V, GND → GND
//   SCK → GPIO 18, MISO → GPIO 19, MOSI → GPIO 23, CS → GPIO 5
//
// IMPORTANT: library assumes a 16 MHz crystal on the MCP2515 by default.
// If your module's oscillator can is marked 8 MHz, uncomment MCP2515_CRYSTAL_8MHZ.

#include <Adafruit_MCP2515.h>

#define CS_PIN              5
#define CAN_BAUDRATE        500000
#define HEARTBEAT_PERIOD_MS 2000

#define CAN_ID_POT_SELECT   0x110   // ESP-B broadcasts its pot-selected relay here

// #define MCP2515_CRYSTAL_8MHZ

Adafruit_MCP2515 mcp(CS_PIN);

struct ButtonChannel {
  const char *name;
  uint8_t     pin;
  uint32_t    canId;
  bool        lastState;
};

ButtonChannel channels[] = {
  { "Button 1", 32, 0x100, HIGH },
  { "Button 2", 33, 0x101, HIGH },
  { "Button 3", 25, 0x102, HIGH },
  { "Button 4", 26, 0x103, HIGH },
};
const int NUM_CHANNELS = sizeof(channels) / sizeof(channels[0]);

unsigned long lastHeartbeat   = 0;
unsigned long sendCount       = 0;
unsigned long sendFailCount   = 0;
int8_t        lastPotRelay    = -1;  // last pot-selected relay index received from ESP-B (-1=none)

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("==========================================");
  Serial.println("ESP32 A v1.5 — CAN button sender booting");
  Serial.println("==========================================");
  Serial.printf("CS_PIN=%d  CAN_BAUDRATE=%ld\n", CS_PIN, (long)CAN_BAUDRATE);

  for (int i = 0; i < NUM_CHANNELS; i++) {
    pinMode(channels[i].pin, INPUT_PULLUP);
    Serial.printf("  %s: pin=%d  canId=0x%lX  initial=%s\n",
                  channels[i].name, channels[i].pin, (unsigned long)channels[i].canId,
                  digitalRead(channels[i].pin) == LOW ? "LOW (pressed)" : "HIGH (released)");
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

  Serial.println("Ready. Press/release buttons to send CAN; watching for 0x110 from ESP-B.");
  Serial.println();
}

void sendButtonState(ButtonChannel &ch, bool pressed) {
  uint8_t payload = pressed ? 0x01 : 0x00;

  Serial.printf("[%8lu ms] >>> %s: %s -> id=0x%lX data=0x%02X\n",
                millis(), ch.name, pressed ? "PRESSED " : "RELEASED",
                (unsigned long)ch.canId, payload);

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
  // --- Poll buttons and send on change ---
  for (int i = 0; i < NUM_CHANNELS; i++) {
    ButtonChannel &ch = channels[i];
    bool cur = digitalRead(ch.pin);
    if (cur != ch.lastState) {
      delay(30); // debounce
      cur = digitalRead(ch.pin);
      if (cur != ch.lastState) {
        sendButtonState(ch, cur == LOW);
        ch.lastState = cur;
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

    if ((uint32_t)id == CAN_ID_POT_SELECT && !mcp.packetRtr()) {
      int8_t relayIndex = (int8_t)firstByte;  // 0-3
      if (relayIndex != lastPotRelay) {
        lastPotRelay = relayIndex;
        Serial.printf("[%8lu ms] POT from ESP-B: relay %d selected (Relay %d)\n",
                      millis(), relayIndex, relayIndex + 1);
      }
    } else {
      Serial.printf("[%8lu ms] RX id=0x%lX (ignored)\n", millis(), id);
    }
  }

  // --- Heartbeat ---
  if (millis() - lastHeartbeat >= HEARTBEAT_PERIOD_MS) {
    lastHeartbeat = millis();
    Serial.printf("[%8lu ms] heartbeat — sends ok: %lu | failed: %lu | pot-selected relay: %s | ",
                  millis(), sendCount - sendFailCount, sendFailCount,
                  lastPotRelay >= 0 ? String(lastPotRelay + 1).c_str() : "none");
    for (int i = 0; i < NUM_CHANNELS; i++) {
      Serial.printf("%s=%s%s", channels[i].name,
                    digitalRead(channels[i].pin) == LOW ? "LOW " : "HIGH",
                    i < NUM_CHANNELS - 1 ? ", " : "");
    }
    Serial.println();
  }
}
