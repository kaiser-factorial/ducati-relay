// ESP32 "B" v1.5 — drives FOUR relays from CAN commands (Adafruit MCP2515 library)
// and reads a brake switch whose state is broadcast over CAN to ESP-A.
//
// Library: install "Adafruit MCP2515" via Arduino Library Manager
// (arduino-cli lib install "Adafruit MCP2515")
//
// Relay wiring (signal pin → relay module IN; relay module VCC/GND from 3.3V/GND):
//   Relay 1: signal → GPIO 4   (CAN ID 0x100)
//   Relay 2: signal → GPIO 32  (CAN ID 0x101)
//   Relay 3: signal → GPIO 33  (CAN ID 0x102)
//   Relay 4: signal → GPIO 25  (CAN ID 0x103)
//
// Brake switch wiring (same as a pushbutton):
//   One leg → GPIO 26, other leg → GND; internal pull-up, no external resistor needed.
//   Brake applied = pin LOW; released = pin HIGH.
//   Broadcasts CAN 0x110: 0x01 = applied, 0x00 = released.
//
// MCP2515 wiring (same VSPI pins as v1):
//   VCC → 3.3V, GND → GND
//   SCK → GPIO 18, MISO → GPIO 19, MOSI → GPIO 23, CS → GPIO 5
//
// Status LED (GPIO 2, onboard): indicates the lowest-numbered active relay.
//   Relay 1 active → solid on
//   Relay 2 active → slow flash  (500 ms half-period)
//   Relay 3 active → rapid flash (100 ms half-period)
//   Relay 4 active → ultra flash  (50 ms half-period)
//   None active    → off
//
// Passive piezo buzzer wiring:
//   Signal → GPIO 13, + → 3.3V (or 5V), − → GND
//   Beeps once per relay ON event; each relay has a distinct pitch.
//
// NOTE: many cheap relay modules are "active LOW". If relays turn on when
// they should be off, flip RELAY_ACTIVE_LOW to true.
//
// IMPORTANT: library assumes 16 MHz crystal. Uncomment MCP2515_CRYSTAL_8MHZ if yours is 8 MHz.

#include <Adafruit_MCP2515.h>

#define CS_PIN               5
#define CAN_BAUDRATE         500000
#define RELAY_ACTIVE_LOW     false
#define HEARTBEAT_PERIOD_MS  2000

#define STATUS_LED_PIN         2
#define STATUS_LED_ACTIVE_LOW  false

#define BUZZER_PIN           13
#define BUZZER_DURATION_MS   80

#define BRAKE_PIN            26
#define CAN_ID_BRAKE         0x110   // ESP-B broadcasts brake state here

// #define MCP2515_CRYSTAL_8MHZ

Adafruit_MCP2515 mcp(CS_PIN);

struct RelayChannel {
  const char *name;
  uint8_t     pin;
  uint32_t    canId;
  bool        isOn;
};

RelayChannel channels[] = {
  { "Relay 1", 4,  0x100, false },
  { "Relay 2", 32, 0x101, false },
  { "Relay 3", 33, 0x102, false },
  { "Relay 4", 25, 0x103, false },
};
const int NUM_CHANNELS = sizeof(channels) / sizeof(channels[0]);

const unsigned long LED_HALF_PERIOD_MS[] = { 0, 500, 100, 50 };
const uint32_t      RELAY_BEEP_HZ[]      = { 880, 1047, 1319, 1568 }; // A5, C6, E6, G6

unsigned long lastHeartbeat  = 0;
unsigned long packetsSeen    = 0;
unsigned long packetsMatched = 0;
int           lastLedChannel = -2;
bool          brakeLastState = HIGH;
bool          brakeOn        = false;

void setRelay(RelayChannel &ch, bool on) {
  ch.isOn = on;
  bool level = RELAY_ACTIVE_LOW ? !on : on;
  digitalWrite(ch.pin, level ? HIGH : LOW);
  Serial.printf("[%8lu ms]     >>> %s -> %s (pin %d %s)\n",
                millis(), ch.name, on ? "ON " : "OFF",
                ch.pin, level ? "HIGH" : "LOW");
  if (on) {
    int chIdx = &ch - channels;
    tone(BUZZER_PIN, RELAY_BEEP_HZ[chIdx], BUZZER_DURATION_MS);
  }
}

void writeStatusLed(bool lit) {
  digitalWrite(STATUS_LED_PIN, (STATUS_LED_ACTIVE_LOW ? !lit : lit) ? HIGH : LOW);
}

void updateStatusLed() {
  int active = -1;
  for (int i = 0; i < NUM_CHANNELS; i++) {
    if (channels[i].isOn) { active = i; break; }
  }

  if (active != lastLedChannel) {
    lastLedChannel = active;
    if (active < 0) {
      Serial.printf("[%8lu ms]     status LED -> off\n", millis());
    } else {
      unsigned long hp = LED_HALF_PERIOD_MS[active];
      const char *pat = (hp == 0) ? "solid on"
                      : (hp >= 300 ? "slow flash" : (hp >= 75 ? "rapid flash" : "ultra flash"));
      Serial.printf("[%8lu ms]     status LED -> %s (tracking %s)\n",
                    millis(), pat, channels[active].name);
    }
  }

  if (active < 0) { writeStatusLed(false); return; }
  unsigned long hp = LED_HALF_PERIOD_MS[active];
  bool lit = (hp == 0) ? true : ((millis() / hp) % 2 == 0);
  writeStatusLed(lit);
}

int findChannel(uint32_t canId) {
  for (int i = 0; i < NUM_CHANNELS; i++) {
    if (channels[i].canId == canId) return i;
  }
  return -1;
}

void sendBrakeState(bool applied) {
  brakeOn = applied;
  uint8_t payload = applied ? 0x01 : 0x00;
  Serial.printf("[%8lu ms] BRAKE %s -> CAN 0x%03X data=0x%02X\n",
                millis(), applied ? "APPLIED " : "RELEASED", CAN_ID_BRAKE, payload);
  mcp.beginPacket(CAN_ID_BRAKE);
  mcp.write(payload);
  int ok = mcp.endPacket();
  Serial.printf("[%8lu ms]     send %s\n", millis(), ok ? "OK" : "FAILED");
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
    unsigned long hp = LED_HALF_PERIOD_MS[i];
    const char *pat = (hp == 0) ? "solid"
                    : (hp >= 300 ? "slow flash" : (hp >= 75 ? "rapid flash" : "ultra flash"));
    Serial.printf("  %s: pin=%d  canId=0x%lX  LED=%s\n",
                  channels[i].name, channels[i].pin,
                  (unsigned long)channels[i].canId, pat);
    setRelay(channels[i], false);
  }

  pinMode(STATUS_LED_PIN, OUTPUT);
  writeStatusLed(false);
  Serial.printf("  Status LED: pin=%d\n", STATUS_LED_PIN);

  pinMode(BUZZER_PIN, OUTPUT);
  Serial.printf("  Buzzer: pin=%d  pitches(Hz)=%u/%u/%u/%u\n",
                BUZZER_PIN,
                RELAY_BEEP_HZ[0], RELAY_BEEP_HZ[1],
                RELAY_BEEP_HZ[2], RELAY_BEEP_HZ[3]);

  pinMode(BRAKE_PIN, INPUT_PULLUP);
  brakeLastState = digitalRead(BRAKE_PIN);
  Serial.printf("  Brake switch: pin=%d  initial=%s\n",
                BRAKE_PIN, brakeLastState == LOW ? "LOW (applied)" : "HIGH (released)");

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

  Serial.println("Receiver ready. Waiting for CAN packets + brake input...");
  Serial.println();
}

void loop() {
  updateStatusLed();

  // --- Poll brake switch ---
  bool cur = digitalRead(BRAKE_PIN);
  if (cur != brakeLastState) {
    delay(30); // debounce
    cur = digitalRead(BRAKE_PIN);
    if (cur != brakeLastState) {
      brakeLastState = cur;
      sendBrakeState(cur == LOW);
    }
  }

  // --- Receive CAN frames from ESP-A ---
  int packetSize = mcp.parsePacket();
  if (packetSize > 0) {
    packetsSeen++;
    long id = mcp.packetId();
    bool rtr = mcp.packetRtr();
    int chIdx = findChannel((uint32_t)id);

    Serial.printf("[%8lu ms] RX #%lu: id=0x%lX (%s%s) len=%d",
                  millis(), packetsSeen, id,
                  mcp.packetExtended() ? "extended" : "standard",
                  rtr ? ", RTR" : "",
                  packetSize);

    if (rtr) {
      Serial.println("  (ignored — RTR)");
    } else {
      uint8_t firstByte = 0;
      Serial.print(" data=[");
      for (int i = 0; i < packetSize; i++) {
        int b = mcp.read();
        if (i == 0) firstByte = (uint8_t)b;
        Serial.printf("%s0x%02X", i > 0 ? " " : "", b);
      }
      Serial.print("]");

      if (chIdx >= 0) {
        packetsMatched++;
        Serial.printf("  <- MATCH (#%lu) -> %s\n", packetsMatched, channels[chIdx].name);
        setRelay(channels[chIdx], firstByte == 0x01);
      } else {
        Serial.printf("  (ignored — id 0x%lX not mapped)\n", id);
      }
    }
  }

  // --- Heartbeat ---
  if (millis() - lastHeartbeat >= HEARTBEAT_PERIOD_MS) {
    lastHeartbeat = millis();
    Serial.printf("[%8lu ms] heartbeat — pkts seen: %lu | matched: %lu | brake: %s | relays:",
                  millis(), packetsSeen, packetsMatched, brakeOn ? "APPLIED" : "released");
    for (int i = 0; i < NUM_CHANNELS; i++) {
      Serial.printf(" %s=%s", channels[i].name, channels[i].isOn ? "ON " : "OFF");
    }
    Serial.println();
  }
}
