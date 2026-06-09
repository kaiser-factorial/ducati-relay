// ESP32 "B" v2 — bike firmware
//
// Receives CAN commands from ESP-A; drives 4 rear relays; implements
// turn-signal flash in software; broadcasts logical relay states for display.
//
// Turn signals flash at ~60 BPM (500ms on / 500ms off) while their
// logical state is ON. Brake light and stereo are direct (no flash).
//
// Library : Adafruit MCP2515  (arduino-cli lib install "Adafruit MCP2515")
// Board   : esp32:esp32:esp32
// Baud    : 115200
//
// ── GPIO layout — confirm before flashing ─────────────────────────────────
//   MCP2515 SPI   SCK=18  MISO=19  MOSI=23  CS=5
//   Status LED    GPIO 2 (onboard)
//
//   Rear relays (active-HIGH; flip RELAY_ACTIVE_LOW if yours are active-LOW):
//     GPIO  4  Relay B1 — left turn signal
//     GPIO 32  Relay B2 — right turn signal
//     GPIO 33  Relay B3 — brake light
//     GPIO 25  Relay B4 — stereo / accessory power
//
// ── CAN IDs consumed ───────────────────────────────────────────────────────
//   0x300  left turn signal  (from ESP-A) — 0x01=ON, 0x00=OFF
//   0x301  right turn signal (from ESP-A) — 0x01=ON, 0x00=OFF
//   0x302  stereo            (from ESP-A) — 0x01=ON, 0x00=OFF
//   0x???  brake light       (from rusEFI ECU — see ECU_BRAKE_CAN_ID below)
//
//   NOTE: rusEFI uses 0x100/0x102 for TunerStudio-over-CAN and 0x200–0x20F
//   for verbose telemetry broadcast. Our commands start at 0x300 to avoid all
//   rusEFI native IDs.
//
// ── CAN IDs produced ───────────────────────────────────────────────────────
//   0x160  ESP-B logical relay status bitmask (bits 0-3 = B1-B4)

#include <Adafruit_MCP2515.h>

// ── Compile-time config ───────────────────────────────────────────────────
#define CS_PIN               5
#define CAN_BAUDRATE         500000
#define RELAY_ACTIVE_LOW     false
#define HEARTBEAT_PERIOD_MS  2000
#define TURN_FLASH_HALF_MS   500    // 500ms on + 500ms off = ~60 flashes/min

// Placeholder CAN ID for the brake light signal from the rusEFI ECU.
// Set to 0x000 to disable until the actual ID is confirmed from the
// rusEFI .ini / tuning config. Replace 0x000 with the real ID before use.
// TODO: confirm actual ECU brake CAN ID (expected range: 0x600–0x6FF)
#define ECU_BRAKE_CAN_ID     0x000UL  // 0x000 = disabled

// Uncomment if your MCP2515 module has an 8 MHz crystal:
// #define MCP2515_CRYSTAL_8MHZ

// Uncomment if the onboard LED lights when its pin is LOW (some boards):
// #define STATUS_LED_ACTIVE_LOW

#define STATUS_LED_PIN       2
#define CAN_ID_ESP_B_STATUS  0x160UL

// ── Relay table ───────────────────────────────────────────────────────────
enum FlashMode { FLASH_NONE, FLASH_TURN };

struct RelayChannel {
  const char *name;
  uint8_t     pin;
  uint32_t    canId;
  bool        enabled;   // false disables CAN ID matching (for stubs)
  FlashMode   flash;
  bool        logical;   // commanded state (true = should be active)
  bool        physical;  // actual current pin state (toggles during flash)
};

RelayChannel relays[] = {
  // name           pin  canId              enabled  flash       logical physical
  { "B1-TurnLeft",  4,   0x300UL,           true,    FLASH_TURN, false,  false },
  { "B2-TurnRight", 32,  0x301UL,           true,    FLASH_TURN, false,  false },
  { "B3-Brake",     33,  ECU_BRAKE_CAN_ID,  false,   FLASH_NONE, false,  false },
  { "B4-Stereo",    25,  0x302UL,           true,    FLASH_NONE, false,  false },
};
const int NUM_RELAYS = sizeof(relays) / sizeof(relays[0]);

// ── Globals ───────────────────────────────────────────────────────────────
Adafruit_MCP2515 mcp(CS_PIN);
unsigned long lastHeartbeat  = 0;
unsigned long packetsSeen    = 0;
unsigned long packetsMatched = 0;
unsigned long txOk = 0, txFail = 0;

// ── Relay pin helpers ─────────────────────────────────────────────────────
void writeRelayPin(int idx, bool on) {
  bool level = RELAY_ACTIVE_LOW ? !on : on;
  digitalWrite(relays[idx].pin, level ? HIGH : LOW);
  relays[idx].physical = on;
}

void setRelayLogical(int idx, bool on) {
  relays[idx].logical = on;
  if (relays[idx].flash == FLASH_NONE) {
    writeRelayPin(idx, on);  // direct; FLASH_TURN relays are handled in updateFlash()
  }
  Serial.printf("[%8lu ms]   %s logical -> %s\n",
                millis(), relays[idx].name, on ? "ON" : "OFF");
}

// Advances flash state for turn-signal relays. Non-blocking — call every loop().
void updateFlash() {
  for (int i = 0; i < NUM_RELAYS; i++) {
    if (relays[i].flash != FLASH_TURN) continue;
    if (!relays[i].logical) {
      if (relays[i].physical) writeRelayPin(i, false);  // ensure off when inactive
      continue;
    }
    bool lit = ((millis() / TURN_FLASH_HALF_MS) % 2 == 0);
    if (lit != relays[i].physical) writeRelayPin(i, lit);
  }
}

// ── Status LED ────────────────────────────────────────────────────────────
void writeStatusLed(bool on) {
#ifdef STATUS_LED_ACTIVE_LOW
  digitalWrite(STATUS_LED_PIN, on ? LOW : HIGH);
#else
  digitalWrite(STATUS_LED_PIN, on ? HIGH : LOW);
#endif
}

// Mirrors turn-signal flash if either turn is active; solid if only brake/stereo on.
void updateStatusLed() {
  bool anyTurn  = relays[0].logical || relays[1].logical;
  bool anyOther = relays[2].logical || relays[3].logical;
  if (!anyTurn && !anyOther) {
    writeStatusLed(false);
  } else if (anyTurn) {
    writeStatusLed((millis() / TURN_FLASH_HALF_MS) % 2 == 0);
  } else {
    writeStatusLed(true);
  }
}

// ── Status broadcast ──────────────────────────────────────────────────────
void broadcastStatus() {
  uint8_t mask = 0;
  for (int i = 0; i < NUM_RELAYS; i++) {
    if (relays[i].logical) mask |= (1 << i);
  }
  mcp.beginPacket(CAN_ID_ESP_B_STATUS);
  mcp.write(mask);
  bool ok = mcp.endPacket();
  ok ? txOk++ : txFail++;
  Serial.printf("[%8lu ms]   ESP-B status 0x%02X (%s)\n", millis(), mask, ok ? "ok" : "FAIL");
}

// ── CAN ID lookup ─────────────────────────────────────────────────────────
int findRelay(uint32_t id) {
  for (int i = 0; i < NUM_RELAYS; i++) {
    if (relays[i].enabled && relays[i].canId == id) return i;
  }
  return -1;
}

// ── Setup / loop ──────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("=======================================");
  Serial.println("ESP32 B v2 — bike firmware booting");
  Serial.println("=======================================");
  Serial.printf("CS_PIN=%d  CAN_BAUDRATE=%ld  RELAY_ACTIVE_LOW=%s\n",
                CS_PIN, (long)CAN_BAUDRATE, RELAY_ACTIVE_LOW ? "true" : "false");

  for (int i = 0; i < NUM_RELAYS; i++) {
    pinMode(relays[i].pin, OUTPUT);
    writeRelayPin(i, false);
    Serial.printf("  %s: pin=%d  canId=0x%03lX  enabled=%s  flash=%s\n",
                  relays[i].name, relays[i].pin,
                  (unsigned long)relays[i].canId,
                  relays[i].enabled ? "yes" : "no (stub)",
                  relays[i].flash == FLASH_NONE ? "none" : "turn");
  }

  pinMode(STATUS_LED_PIN, OUTPUT);
  writeStatusLed(false);
  Serial.printf("  Status LED: pin=%d\n", STATUS_LED_PIN);

#ifdef MCP2515_CRYSTAL_8MHZ
  mcp.setClockFrequency(8e6);
  Serial.println("MCP2515 clock set to 8 MHz");
#else
  Serial.println("MCP2515 clock: 16 MHz (default)");
#endif

  Serial.println("Calling mcp.begin() ...");
  if (!mcp.begin(CAN_BAUDRATE)) {
    Serial.println("!!! mcp.begin() FAILED — check wiring / crystal frequency");
    while (1) delay(10);
  }
  Serial.println("mcp.begin() ok");
  Serial.println("--- MCP2515 register dump ---");
  mcp.dumpRegisters(Serial);
  Serial.println("-----------------------------");
  Serial.println("Ready.\n");
}

void loop() {
  updateFlash();
  updateStatusLed();

  int packetSize = mcp.parsePacket();
  if (packetSize > 0) {
    packetsSeen++;
    uint32_t id = (uint32_t)mcp.packetId();
    bool extended = mcp.packetExtended();
    bool rtr      = mcp.packetRtr();
    int  rIdx     = findRelay(id);

    Serial.printf("[%8lu ms] RX #%lu: id=0x%03lX (%s%s) len=%d",
                  millis(), packetsSeen, id,
                  extended ? "extended" : "standard",
                  rtr ? ",RTR" : "",
                  packetSize);

    if (!rtr) {
      uint8_t first = 0;
      Serial.print(" data=[");
      for (int i = 0; i < packetSize; i++) {
        int b = mcp.read();
        if (i == 0) first = (uint8_t)b;
        Serial.printf("%s0x%02X", i > 0 ? " " : "", b);
      }
      Serial.print("]");

      if (rIdx >= 0) {
        packetsMatched++;
        Serial.printf("  <- MATCH (#%lu) -> %s\n", packetsMatched, relays[rIdx].name);
        setRelayLogical(rIdx, first == 0x01);
        broadcastStatus();
      } else {
        Serial.printf("  (ignored — id 0x%03lX not mapped)\n", id);
      }
    } else {
      Serial.println("  (ignored — RTR)");
    }
  }

  if (millis() - lastHeartbeat >= HEARTBEAT_PERIOD_MS) {
    lastHeartbeat = millis();
    Serial.printf("[%8lu ms] heartbeat — rx seen=%lu matched=%lu | tx ok=%lu fail=%lu\n",
                  millis(), packetsSeen, packetsMatched, txOk, txFail);
    for (int i = 0; i < NUM_RELAYS; i++) {
      Serial.printf("  %s logical=%s  physical=%s\n",
                    relays[i].name,
                    relays[i].logical  ? "ON" : "off",
                    relays[i].physical ? "ON" : "off");
    }
  }
}
