// ESP32 "B" v1.5 — drives FOUR relays from CAN commands (Adafruit MCP2515 library)
// and reads a potentiometer whose position selects which relay is active locally.
// The pot selection is broadcast over CAN so ESP-A can track it.
//
// Relay active = (CAN button command from ESP-A says ON)  OR  (pot is pointing at it).
// Both controls work simultaneously and independently.
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
// Potentiometer wiring:
//   Wiper (middle leg) → GPIO 34 (ADC input-only pin, no pull-up conflict)
//   One outer leg → 3.3V, other outer leg → GND
//   Pot position maps to 4 equal zones → selects Relay 1, 2, 3, or 4
//
// MCP2515 wiring (same VSPI pins as v1):
//   VCC → 3.3V, GND → GND
//   SCK → GPIO 18, MISO → GPIO 19, MOSI → GPIO 23, CS → GPIO 5
//
// Status LED (GPIO 2, onboard LED): indicates the lowest-numbered active relay.
//   Relay 1 active → solid on
//   Relay 2 active → slow flash  (500 ms half-period)
//   Relay 3 active → rapid flash (100 ms half-period)
//   Relay 4 active → ultra flash  (50 ms half-period)
//   None active    → off
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

#define POT_PIN              34     // ADC1 CH6 — input-only, no digital drive issues
#define POT_BROADCAST_MS     100    // send CAN 0x110 when pot zone changes; also re-send every N ms
#define CAN_ID_POT_SELECT    0x110  // this board broadcasts its pot selection here

// #define MCP2515_CRYSTAL_8MHZ

Adafruit_MCP2515 mcp(CS_PIN);

struct RelayChannel {
  const char *name;
  uint8_t     pin;
  uint32_t    canId;
  bool        btnOn;   // ON commanded by ESP-A button press
  bool        potOn;   // ON because pot is currently pointing here
};

RelayChannel channels[] = {
  { "Relay 1", 4,  0x100, false, false },
  { "Relay 2", 32, 0x101, false, false },
  { "Relay 3", 33, 0x102, false, false },
  { "Relay 4", 25, 0x103, false, false },
};
const int NUM_CHANNELS = sizeof(channels) / sizeof(channels[0]);

// Status LED blink half-period per channel (ms); 0 = solid on.
const unsigned long LED_HALF_PERIOD_MS[] = { 0, 500, 100, 50 };

unsigned long lastHeartbeat    = 0;
unsigned long lastPotBroadcast = 0;
unsigned long packetsSeen      = 0;
unsigned long packetsMatched   = 0;
int           lastLedChannel   = -2;
int8_t        lastPotZone      = -1;

void applyRelay(RelayChannel &ch) {
  bool on = ch.btnOn || ch.potOn;
  bool level = RELAY_ACTIVE_LOW ? !on : on;
  digitalWrite(ch.pin, level ? HIGH : LOW);
}

void setRelayBtn(RelayChannel &ch, bool on) {
  bool wasActive = ch.btnOn || ch.potOn;
  ch.btnOn = on;
  bool isActive = ch.btnOn || ch.potOn;
  applyRelay(ch);
  if (wasActive != isActive) {
    Serial.printf("[%8lu ms]     >>> %s -> %s (btn=%s pot=%s, pin %d %s)\n",
                  millis(), ch.name, isActive ? "ON " : "OFF",
                  ch.btnOn ? "ON" : "OFF", ch.potOn ? "ON" : "OFF",
                  ch.pin, isActive ? "HIGH" : "LOW");
  }
}

void setRelayPot(int zoneIndex) {
  for (int i = 0; i < NUM_CHANNELS; i++) {
    bool wasActive = channels[i].btnOn || channels[i].potOn;
    channels[i].potOn = (i == zoneIndex);
    bool isActive = channels[i].btnOn || channels[i].potOn;
    applyRelay(channels[i]);
    if (wasActive != isActive) {
      Serial.printf("[%8lu ms]     >>> %s -> %s (btn=%s pot=%s, pin %d %s)\n",
                    millis(), channels[i].name, isActive ? "ON " : "OFF",
                    channels[i].btnOn ? "ON" : "OFF", channels[i].potOn ? "ON" : "OFF",
                    channels[i].pin, isActive ? "HIGH" : "LOW");
    }
  }
}

void writeStatusLed(bool lit) {
  digitalWrite(STATUS_LED_PIN, (STATUS_LED_ACTIVE_LOW ? !lit : lit) ? HIGH : LOW);
}

void updateStatusLed() {
  int active = -1;
  for (int i = 0; i < NUM_CHANNELS; i++) {
    if (channels[i].btnOn || channels[i].potOn) { active = i; break; }
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

// Read pot, return zone 0-3.
int8_t readPotZone() {
  int raw = analogRead(POT_PIN);   // 0-4095 (12-bit ADC)
  return (int8_t)(raw / 1024);     // 0,1,2,3 (4096/4 = 1024 per zone)
}

void broadcastPotSelection(int8_t zone) {
  mcp.beginPacket(CAN_ID_POT_SELECT);
  mcp.write((uint8_t)zone);
  int ok = mcp.endPacket();
  Serial.printf("[%8lu ms] POT broadcast: zone=%d (Relay %d) -> CAN 0x%03X %s\n",
                millis(), zone, zone + 1, CAN_ID_POT_SELECT, ok ? "OK" : "FAILED");
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
    applyRelay(channels[i]);
  }

  pinMode(STATUS_LED_PIN, OUTPUT);
  writeStatusLed(false);
  Serial.printf("  Status LED: pin=%d\n", STATUS_LED_PIN);
  Serial.printf("  Potentiometer: pin=%d  broadcast CAN ID=0x%03X\n", POT_PIN, CAN_ID_POT_SELECT);

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

  // Capture initial pot position and broadcast it.
  lastPotZone = readPotZone();
  setRelayPot(lastPotZone);
  broadcastPotSelection(lastPotZone);

  Serial.println("Receiver ready. Waiting for CAN packets + pot input...");
  Serial.println();
}

void loop() {
  updateStatusLed();

  // --- Read potentiometer and update relay states on zone change ---
  int8_t zone = readPotZone();
  unsigned long now = millis();
  bool zoneChanged = (zone != lastPotZone);
  if (zoneChanged) {
    lastPotZone = zone;
    Serial.printf("[%8lu ms] POT zone changed -> %d (Relay %d)\n", now, zone, zone + 1);
    setRelayPot(zone);
  }
  // Rebroadcast on zone change or periodically.
  if (zoneChanged || (now - lastPotBroadcast >= POT_BROADCAST_MS)) {
    broadcastPotSelection(lastPotZone);
    lastPotBroadcast = now;
  }

  // --- Receive CAN frames from ESP-A ---
  int packetSize = mcp.parsePacket();
  if (packetSize > 0) {
    packetsSeen++;
    long id = mcp.packetId();
    bool rtr = mcp.packetRtr();
    int chIdx = findChannel((uint32_t)id);

    Serial.printf("[%8lu ms] RX #%lu: id=0x%lX (%s%s) len=%d",
                  now, packetsSeen, id,
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
        setRelayBtn(channels[chIdx], firstByte == 0x01);
      } else {
        Serial.printf("  (ignored — id 0x%lX not mapped)\n", id);
      }
    }
  }

  // --- Heartbeat ---
  if (now - lastHeartbeat >= HEARTBEAT_PERIOD_MS) {
    lastHeartbeat = now;
    Serial.printf("[%8lu ms] heartbeat — pkts seen: %lu | matched: %lu | pot zone: %d (Relay %d) | relays:",
                  now, packetsSeen, packetsMatched, lastPotZone, lastPotZone + 1);
    for (int i = 0; i < NUM_CHANNELS; i++) {
      bool active = channels[i].btnOn || channels[i].potOn;
      Serial.printf(" %s=%s(btn=%s,pot=%s)",
                    channels[i].name, active ? "ON " : "OFF",
                    channels[i].btnOn ? "Y" : "N",
                    channels[i].potOn ? "Y" : "N");
    }
    Serial.println();
  }
}
