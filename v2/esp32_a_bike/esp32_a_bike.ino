// ESP32 "A" v2 — bike firmware
//
// Reads 7 rider buttons; drives 4 front relays directly; sends CAN commands
// to ESP-B for rear relay control; receives ESP-B status and (future) ECU
// telemetry. Broadcasts front relay states on CAN 0x130.
//
// Library : Adafruit MCP2515  (arduino-cli lib install "Adafruit MCP2515")
// Board   : esp32:esp32:esp32
// Baud    : 115200
//
// ── GPIO layout — confirm before flashing ─────────────────────────────────
//   MCP2515 SPI   SCK=18  MISO=19  MOSI=23  CS=5
//
//   Buttons (INPUT_PULLUP — press connects pin to GND):
//     GPIO 27  Power / ignition toggle
//     GPIO 25  Motor start (momentary; requires ignition ON)
//     GPIO 26  Headlight low beam toggle
//     GPIO 14  Headlight high beam toggle
//     GPIO 13  Left turn signal toggle
//     GPIO 17  Right turn signal toggle
//     GPIO 22  Stereo toggle
//
//   Front relays (active-HIGH; flip RELAY_ACTIVE_LOW if yours are active-LOW):
//     GPIO  4  Relay A1 — ignition / bike power rail
//     GPIO 32  Relay A2 — starter motor circuit
//     GPIO 33  Relay A3 — headlight low beam
//     GPIO 16  Relay A4 — headlight high beam
//
// ── CAN IDs ────────────────────────────────────────────────────────────────
//   Sent   0x100  left turn signal command  (0x01=ON, 0x00=OFF) → ESP-B
//          0x101  right turn signal command (0x01=ON, 0x00=OFF) → ESP-B
//          0x102  stereo command            (0x01=ON, 0x00=OFF) → ESP-B
//          0x130  ESP-A relay status bitmask (bits 0-3 = A1-A4)
//   Heard  0x160  ESP-B relay logical status (logged; reserved for display)
//          0x6xx  rusEFI ECU telemetry (TODO: parse once IDs are confirmed)

#include <Adafruit_MCP2515.h>

// ── Compile-time config ───────────────────────────────────────────────────
#define CS_PIN              5
#define CAN_BAUDRATE        500000
#define RELAY_ACTIVE_LOW    false
#define HEARTBEAT_PERIOD_MS 2000
#define DEBOUNCE_MS         30

// Uncomment if your MCP2515 module has an 8 MHz crystal (check the oscillator
// can on the board — if marked 8MHz, this define is required):
// #define MCP2515_CRYSTAL_8MHZ

// ── CAN IDs ───────────────────────────────────────────────────────────────
#define CAN_ID_TURN_LEFT    0x100UL
#define CAN_ID_TURN_RIGHT   0x101UL
#define CAN_ID_STEREO       0x102UL
#define CAN_ID_ESP_A_STATUS 0x130UL
#define CAN_ID_ESP_B_STATUS 0x160UL

// ── Front relay table ─────────────────────────────────────────────────────
struct LocalRelay {
  const char *name;
  uint8_t     pin;
  bool        isOn;
};

LocalRelay relays[] = {
  { "A1-Ignition",    4,  false },  // index 0
  { "A2-Starter",     32, false },  // index 1
  { "A3-HeadlightLo", 33, false },  // index 2
  { "A4-HeadlightHi", 16, false },  // index 3
};
const int NUM_RELAYS = sizeof(relays) / sizeof(relays[0]);
#define RELAY_IGNITION 0
#define RELAY_STARTER  1

// ── CAN command table (sent to ESP-B) ────────────────────────────────────
struct CanCommand {
  const char *name;
  uint32_t    id;
  bool        isOn;
};

CanCommand canCmds[] = {
  { "B1-TurnLeft",  CAN_ID_TURN_LEFT,  false },  // index 0
  { "B2-TurnRight", CAN_ID_TURN_RIGHT, false },  // index 1
  { "B4-Stereo",    CAN_ID_STEREO,     false },  // index 2
};
const int NUM_CAN_CMDS = sizeof(canCmds) / sizeof(canCmds[0]);
#define CAN_IDX_TURN_LEFT  0
#define CAN_IDX_TURN_RIGHT 1

// ── Button table ──────────────────────────────────────────────────────────
enum BtnMode   { TOGGLE, MOMENTARY };
enum BtnTarget { TARGET_RELAY, TARGET_CAN };

struct Button {
  const char *name;
  uint8_t     pin;
  BtnMode     mode;
  BtnTarget   target;
  int         idx;   // index into relays[] or canCmds[]
  bool        last;  // last debounced pin level
};

Button buttons[] = {
  // name           pin  mode       target         idx  last
  { "Power",        27,  TOGGLE,    TARGET_RELAY,  0,   HIGH },
  { "Starter",      25,  MOMENTARY, TARGET_RELAY,  1,   HIGH },
  { "HdlightLo",    26,  TOGGLE,    TARGET_RELAY,  2,   HIGH },
  { "HdlightHi",    14,  TOGGLE,    TARGET_RELAY,  3,   HIGH },
  { "TurnLeft",     13,  TOGGLE,    TARGET_CAN,    0,   HIGH },
  { "TurnRight",    17,  TOGGLE,    TARGET_CAN,    1,   HIGH },
  { "Stereo",       22,  TOGGLE,    TARGET_CAN,    2,   HIGH },
};
const int NUM_BUTTONS = sizeof(buttons) / sizeof(buttons[0]);

// ── Globals ───────────────────────────────────────────────────────────────
Adafruit_MCP2515 mcp(CS_PIN);
unsigned long lastHeartbeat = 0;
unsigned long txOk = 0, txFail = 0, rxCount = 0;
uint8_t espBStatus = 0;  // last-known ESP-B relay bitmask (from 0x160)

// ── Low-level helpers ─────────────────────────────────────────────────────
void applyRelay(int idx, bool on) {
  relays[idx].isOn = on;
  bool level = RELAY_ACTIVE_LOW ? !on : on;
  digitalWrite(relays[idx].pin, level ? HIGH : LOW);
  Serial.printf("[%8lu ms]   %s -> %s\n", millis(), relays[idx].name, on ? "ON" : "OFF");
}

bool sendFrame(uint32_t id, uint8_t data) {
  mcp.beginPacket(id);
  mcp.write(data);
  bool ok = mcp.endPacket();
  ok ? txOk++ : txFail++;
  return ok;
}

void broadcastStatus() {
  uint8_t mask = 0;
  for (int i = 0; i < NUM_RELAYS; i++) {
    if (relays[i].isOn) mask |= (1 << i);
  }
  bool ok = sendFrame(CAN_ID_ESP_A_STATUS, mask);
  Serial.printf("[%8lu ms]   ESP-A status 0x%02X (%s)\n", millis(), mask, ok ? "ok" : "FAIL");
}

void sendCanCmd(int idx, bool on) {
  canCmds[idx].isOn = on;
  bool ok = sendFrame(canCmds[idx].id, on ? 0x01 : 0x00);
  Serial.printf("[%8lu ms]   CAN %s -> %s (%s)\n",
                millis(), canCmds[idx].name, on ? "ON" : "OFF", ok ? "ok" : "FAIL");
}

// Turn signals are mutually exclusive: pressing one cancels the other, then toggles.
void toggleTurnSignal(int idx) {
  int other = (idx == CAN_IDX_TURN_LEFT) ? CAN_IDX_TURN_RIGHT : CAN_IDX_TURN_LEFT;
  if (canCmds[other].isOn) sendCanCmd(other, false);
  sendCanCmd(idx, !canCmds[idx].isOn);
}

// Shuts down all outputs except ignition itself; called when ignition is turned off.
void shutdownAllOutputs() {
  for (int i = 1; i < NUM_RELAYS; i++) applyRelay(i, false);
  for (int i = 0; i < NUM_CAN_CMDS; i++) {
    if (canCmds[i].isOn) sendCanCmd(i, false);
  }
}

// ── Button action dispatch ────────────────────────────────────────────────
void handlePress(Button &btn, bool pressed) {
  if (btn.target == TARGET_RELAY) {
    if (btn.mode == MOMENTARY) {
      if (btn.idx == RELAY_STARTER && pressed && !relays[RELAY_IGNITION].isOn) {
        Serial.printf("[%8lu ms]   Starter blocked — ignition not ON\n", millis());
        return;
      }
      applyRelay(btn.idx, pressed);
      broadcastStatus();
    } else if (pressed) {  // TOGGLE, act only on press edge
      bool newState = !relays[btn.idx].isOn;
      applyRelay(btn.idx, newState);
      if (btn.idx == RELAY_IGNITION && !newState) {
        shutdownAllOutputs();
      }
      broadcastStatus();
    }
  } else if (pressed) {  // TARGET_CAN, act only on press edge
    if (btn.idx == CAN_IDX_TURN_LEFT || btn.idx == CAN_IDX_TURN_RIGHT) {
      toggleTurnSignal(btn.idx);
    } else {
      sendCanCmd(btn.idx, !canCmds[btn.idx].isOn);
    }
  }
}

// ── CAN receive (telemetry aggregation — display rendering is TODO) ───────
void handleIncoming() {
  int n = mcp.parsePacket();
  if (n <= 0) return;
  rxCount++;

  uint32_t id = (uint32_t)mcp.packetId();
  if (mcp.packetRtr()) {
    Serial.printf("[%8lu ms] RX RTR id=0x%03lX — ignored\n", millis(), id);
    return;
  }

  uint8_t buf[8] = {};
  int len = 0;
  while (mcp.available() && len < 8) buf[len++] = mcp.read();

  if (id == CAN_ID_ESP_B_STATUS) {
    espBStatus = buf[0];
    Serial.printf("[%8lu ms] ESP-B status 0x%02X\n", millis(), espBStatus);
  } else {
    // TODO: parse rusEFI ECU telemetry (RPM, TPS, coolant temp, battery, gear)
    //       once the actual CAN IDs are confirmed from the rusEFI .ini config.
    //       Expected range: 0x600–0x6FF (verify against the friend's ECU setup).
    Serial.printf("[%8lu ms] RX id=0x%03lX len=%d (unhandled)\n", millis(), id, len);
  }
}

// ── Setup / loop ──────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("====================================");
  Serial.println("ESP32 A v2 — bike firmware booting");
  Serial.println("====================================");
  Serial.printf("CS_PIN=%d  CAN_BAUDRATE=%ld  RELAY_ACTIVE_LOW=%s\n",
                CS_PIN, (long)CAN_BAUDRATE, RELAY_ACTIVE_LOW ? "true" : "false");

  for (int i = 0; i < NUM_RELAYS; i++) {
    pinMode(relays[i].pin, OUTPUT);
    applyRelay(i, false);
    Serial.printf("  %s: pin=%d\n", relays[i].name, relays[i].pin);
  }
  for (int i = 0; i < NUM_BUTTONS; i++) {
    pinMode(buttons[i].pin, INPUT_PULLUP);
    Serial.printf("  %-12s pin=%d  %-9s %s[%d]\n",
                  buttons[i].name, buttons[i].pin,
                  buttons[i].mode == TOGGLE ? "TOGGLE" : "MOMENTARY",
                  buttons[i].target == TARGET_RELAY ? "relay" : "CAN",
                  buttons[i].idx);
  }

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
  for (int i = 0; i < NUM_BUTTONS; i++) {
    Button &btn = buttons[i];
    bool raw = digitalRead(btn.pin);
    if (raw != btn.last) {
      delay(DEBOUNCE_MS);
      raw = digitalRead(btn.pin);
      if (raw != btn.last) {
        btn.last = raw;
        bool pressed = (raw == LOW);
        Serial.printf("[%8lu ms] %s %s\n",
                      millis(), btn.name, pressed ? "PRESSED" : "RELEASED");
        handlePress(btn, pressed);
      }
    }
  }

  handleIncoming();

  if (millis() - lastHeartbeat >= HEARTBEAT_PERIOD_MS) {
    lastHeartbeat = millis();
    Serial.printf("[%8lu ms] heartbeat — tx ok=%lu fail=%lu  rx=%lu  ESP-B=0x%02X\n",
                  millis(), txOk, txFail, rxCount, espBStatus);
    for (int i = 0; i < NUM_RELAYS; i++) {
      Serial.printf("  %s=%s\n", relays[i].name, relays[i].isOn ? "ON" : "off");
    }
    for (int i = 0; i < NUM_CAN_CMDS; i++) {
      Serial.printf("  %s=%s (cmd)\n", canCmds[i].name, canCmds[i].isOn ? "ON" : "off");
    }
  }
}
