/*
 * Ducati front lighting and dashboard controller - v7
 *
 * Board: ESP32-S3 plus separate 4-channel and single-channel relay modules
 * CAN:   MCP2515, 500 kbit/s, standard 11-bit frames
 *
 * Inputs use INPUT_PULLUP. Each switch/button connects its GPIO to GND.
 * Never connect a motorcycle 12 V signal directly to an ESP32 GPIO.
 *
 * Local outputs:
 *   A high beam  -> 4-relay board channel 1
 *   B low beam   -> 4-relay board channel 2
 *   C left arrow -> 4-relay board channel 3, blinking
 *   D right arrow-> 4-relay board channel 4, blinking
 *   E dashboard  -> single relay, controlled only by incoming CAN 0x305
 *
 * CAN protocol (one-byte payload: 0x01 ON, 0x00 OFF):
 *   TX 0x300 left indicator
 *   TX 0x301 right indicator
 *   TX 0x303 brake
 *   TX 0x304 both indicators atomically (used when entering hazard mode)
 *   RX 0x305 dashboard relay
 */

#include <Adafruit_MCP2515.h>

// ---------------------------------------------------------------------------
// Hardware configuration - verify these against the exact ESP32-S3 and modules.
// ---------------------------------------------------------------------------

// The CAN pins reuse the known ESP32-S3 mapping from this project's v3 front
// hardware. The four-pin constructor selects software SPI, so these pins will not
// be silently replaced by a board package's default hardware-SPI assignments.
constexpr uint8_t CAN_SCK_PIN = 6;
constexpr uint8_t CAN_MOSI_PIN = 7;
constexpr uint8_t CAN_MISO_PIN = 14;
constexpr uint8_t CAN_CS_PIN = 16;

// Provisional front-control GPIO map. Change only this block if the finished
// harness assigns different pins.
constexpr uint8_t INPUT_HIGH_BEAM_PIN = 47;
constexpr uint8_t INPUT_LOW_BEAM_PIN = 48;
constexpr uint8_t INPUT_LEFT_PIN = 17;
constexpr uint8_t INPUT_RIGHT_PIN = 18;
constexpr uint8_t INPUT_BRAKE_PIN = 21;

constexpr uint8_t RELAY_HIGH_BEAM_PIN = 8;  // 4-board channel A / IN1
constexpr uint8_t RELAY_LOW_BEAM_PIN = 9;   // 4-board channel B / IN2
constexpr uint8_t RELAY_LEFT_PIN = 10;      // 4-board channel C / IN3
constexpr uint8_t RELAY_RIGHT_PIN = 11;     // 4-board channel D / IN4
constexpr uint8_t RELAY_DASH_PIN = 12;      // separate single relay E

// v5's integrated relay board was active HIGH. Flip this to true if either new
// relay module is active LOW. If the two modules have different polarities, split
// this into one setting for the 4-board and one for the dashboard relay.
constexpr bool RELAYS_ACTIVE_LOW = false;

// Default: high/low inputs are maintained switches whose positions directly
// determine the relays. Set false if they are momentary pushbuttons that should
// toggle on each press. Left/right always toggle on press; brake always follows
// its switch state.
constexpr bool HEADLIGHT_INPUTS_ARE_MAINTAINED = true;

constexpr uint32_t CAN_BITRATE = 500000UL;
// v5 rear hardware used an MCP2515 marked "8.000". Verify the oscillator can on
// this front module and change to 12000000UL or 16000000UL if it is different.
constexpr uint32_t MCP2515_CLOCK_HZ = 8000000UL;

constexpr uint32_t CAN_LEFT_ID = 0x300;
constexpr uint32_t CAN_RIGHT_ID = 0x301;
constexpr uint32_t CAN_BRAKE_ID = 0x303;
constexpr uint32_t CAN_HAZARD_ID = 0x304;
constexpr uint32_t CAN_DASH_ID = 0x305;

constexpr uint32_t DEBOUNCE_MS = 35;
constexpr uint32_t BLINK_HALF_PERIOD_MS = 500;
constexpr uint32_t HEARTBEAT_MS = 2000;

Adafruit_MCP2515 mcp(CAN_CS_PIN, CAN_MOSI_PIN, CAN_MISO_PIN, CAN_SCK_PIN);

struct DebouncedInput {
  const char *name;
  uint8_t pin;
  bool rawActive;
  bool stableActive;
  uint32_t rawChangedMs;
};

DebouncedInput highBeamInput = {"HIGH", INPUT_HIGH_BEAM_PIN, false, false, 0};
DebouncedInput lowBeamInput = {"LOW", INPUT_LOW_BEAM_PIN, false, false, 0};
DebouncedInput leftInput = {"LEFT", INPUT_LEFT_PIN, false, false, 0};
DebouncedInput rightInput = {"RIGHT", INPUT_RIGHT_PIN, false, false, 0};
DebouncedInput brakeInput = {"BRAKE", INPUT_BRAKE_PIN, false, false, 0};

bool highBeamOn = false;
bool lowBeamOn = false;
bool leftEnabled = false;
bool rightEnabled = false;
bool brakeOn = false;
bool dashboardOn = false;
bool blinkPhaseOn = false;
bool canReady = false;

uint32_t lastBlinkMs = 0;
uint32_t lastHeartbeatMs = 0;
uint32_t receivedFrames = 0;
uint32_t sentFrames = 0;
uint32_t failedFrames = 0;

bool relayElectricalLevel(bool on) {
  return RELAYS_ACTIVE_LOW ? !on : on;
}

void prepareRelayOutput(uint8_t pin) {
  // Preload the safe OFF level before enabling the pin as an output.
  digitalWrite(pin, relayElectricalLevel(false) ? HIGH : LOW);
  pinMode(pin, OUTPUT);
}

void writeRelay(uint8_t pin, bool on) {
  digitalWrite(pin, relayElectricalLevel(on) ? HIGH : LOW);
}

void updateRelayOutputs() {
  writeRelay(RELAY_HIGH_BEAM_PIN, highBeamOn);
  writeRelay(RELAY_LOW_BEAM_PIN, lowBeamOn);
  writeRelay(RELAY_LEFT_PIN, leftEnabled && blinkPhaseOn);
  writeRelay(RELAY_RIGHT_PIN, rightEnabled && blinkPhaseOn);
  writeRelay(RELAY_DASH_PIN, dashboardOn);
}

void printState() {
  Serial.printf(
      "[STATE] high=%s low=%s left=%s right=%s brake=%s dash=%s phase=%s\n",
      highBeamOn ? "ON" : "OFF", lowBeamOn ? "ON" : "OFF",
      leftEnabled ? "ON" : "OFF", rightEnabled ? "ON" : "OFF",
      brakeOn ? "ON" : "OFF", dashboardOn ? "ON" : "OFF",
      blinkPhaseOn ? "ON" : "OFF");
}

bool sendCanState(uint32_t id, bool on) {
  if (!canReady) {
    failedFrames++;
    Serial.printf("[CAN TX] ID=0x%03lX state=%s FAILED: CAN unavailable\n",
                  static_cast<unsigned long>(id), on ? "ON" : "OFF");
    return false;
  }

  mcp.beginPacket(id);
  mcp.write(on ? 0x01 : 0x00);
  const bool sent = mcp.endPacket() != 0;
  if (sent) {
    sentFrames++;
  } else {
    failedFrames++;
  }

  Serial.printf("[CAN TX] ID=0x%03lX data[0]=0x%02X %s\n",
                static_cast<unsigned long>(id), on ? 0x01 : 0x00,
                sent ? "queued" : "FAILED");
  return sent;
}

void startBlinkCycleIfStopped() {
  if (!leftEnabled && !rightEnabled) {
    blinkPhaseOn = true;
    lastBlinkMs = millis();
  }
}

void toggleIndicator(bool toggleLeftSide) {
  bool &selected = toggleLeftSide ? leftEnabled : rightEnabled;
  const bool other = toggleLeftSide ? rightEnabled : leftEnabled;
  const uint32_t selectedId = toggleLeftSide ? CAN_LEFT_ID : CAN_RIGHT_ID;
  uint32_t outgoingId = selectedId;
  bool outgoingState = false;

  if (selected) {
    // Each button turns its own side back off, including while both are active.
    selected = false;
  } else if (other) {
    // Pressing the opposite side while one is active enters synchronized hazard
    // mode in one atomic frame. Both front relays already share one blink phase.
    selected = true;
    outgoingId = CAN_HAZARD_ID;
    outgoingState = true;
  } else {
    startBlinkCycleIfStopped();
    selected = true;
    outgoingState = true;
  }

  if (!leftEnabled && !rightEnabled) {
    blinkPhaseOn = false;
  }

  // Apply the front light before touching CAN. Adafruit_MCP2515::endPacket()
  // can wait while a disconnected bus accumulates transmit errors; local lights
  // must remain usable even when the rear node or CAN wiring is unavailable.
  updateRelayOutputs();
  sendCanState(outgoingId, outgoingState);
  printState();
}

// Returns -1 for no stable edge, 0 for a release/open edge, and 1 for a
// press/closed-to-GND edge.
int8_t updateInput(DebouncedInput &input, uint32_t now) {
  const bool rawActive = digitalRead(input.pin) == LOW;
  if (rawActive != input.rawActive) {
    input.rawActive = rawActive;
    input.rawChangedMs = now;
  }

  if (rawActive == input.stableActive || now - input.rawChangedMs < DEBOUNCE_MS) {
    return -1;
  }

  input.stableActive = rawActive;
  Serial.printf("[INPUT] %s %s\n", input.name, rawActive ? "ACTIVE" : "released");
  return rawActive ? 1 : 0;
}

void updateHeadlightInput(DebouncedInput &input, bool &state, uint32_t now) {
  const int8_t edge = updateInput(input, now);
  if (edge < 0) {
    return;
  }

  if (HEADLIGHT_INPUTS_ARE_MAINTAINED) {
    state = input.stableActive;
  } else if (edge == 1) {
    state = !state;
  } else {
    return;
  }

  updateRelayOutputs();
  printState();
}

void updateBrakeInput(uint32_t now) {
  const int8_t edge = updateInput(brakeInput, now);
  if (edge < 0) {
    return;
  }

  brakeOn = brakeInput.stableActive;
  sendCanState(CAN_BRAKE_ID, brakeOn);
  printState();
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

  // Dashboard accepts only an ordinary 11-bit data frame with exactly the
  // documented Boolean values. All other CAN traffic remains untouched.
  if (extended || remote || packetSize < 1 || length < 1 || id != CAN_DASH_ID ||
      (data[0] != 0x00 && data[0] != 0x01)) {
    return;
  }

  const bool requestedState = data[0] == 0x01;
  if (dashboardOn == requestedState) {
    return;
  }

  dashboardOn = requestedState;
  updateRelayOutputs();
  Serial.printf("[CAN RX] ID=0x305 dashboard=%s\n", dashboardOn ? "ON" : "OFF");
  printState();
}

void initializeInput(DebouncedInput &input) {
  pinMode(input.pin, INPUT_PULLUP);
  input.rawActive = digitalRead(input.pin) == LOW;
  input.stableActive = input.rawActive;
  input.rawChangedMs = millis();
}

void setupCan() {
  mcp.setClockFrequency(MCP2515_CLOCK_HZ);
  if (!mcp.begin(CAN_BITRATE)) {
    Serial.println("[CAN] ERROR: MCP2515 initialization failed.");
    Serial.println("[CAN] Local front lights still work; dashboard remains OFF.");
    return;
  }

  canReady = true;
  Serial.printf("[CAN] Ready: 500 kbit/s, MCP2515 clock %lu Hz\n",
                static_cast<unsigned long>(MCP2515_CLOCK_HZ));

  // This front controller is the command source. Clear stale rear indicators
  // after a front reset, then publish the brake switch's actual boot state.
  sendCanState(CAN_HAZARD_ID, false);
  sendCanState(CAN_BRAKE_ID, brakeOn);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== Ducati Front CAN Controller v7 ===");

  prepareRelayOutput(RELAY_HIGH_BEAM_PIN);
  prepareRelayOutput(RELAY_LOW_BEAM_PIN);
  prepareRelayOutput(RELAY_LEFT_PIN);
  prepareRelayOutput(RELAY_RIGHT_PIN);
  prepareRelayOutput(RELAY_DASH_PIN);

  initializeInput(highBeamInput);
  initializeInput(lowBeamInput);
  initializeInput(leftInput);
  initializeInput(rightInput);
  initializeInput(brakeInput);

  // Maintained headlight and brake switches may already be active at boot.
  highBeamOn = HEADLIGHT_INPUTS_ARE_MAINTAINED && highBeamInput.stableActive;
  lowBeamOn = HEADLIGHT_INPUTS_ARE_MAINTAINED && lowBeamInput.stableActive;
  brakeOn = brakeInput.stableActive;
  leftEnabled = false;
  rightEnabled = false;
  dashboardOn = false;
  blinkPhaseOn = false;
  updateRelayOutputs();

  setupCan();
  printState();
}

void loop() {
  const uint32_t now = millis();

  updateHeadlightInput(highBeamInput, highBeamOn, now);
  updateHeadlightInput(lowBeamInput, lowBeamOn, now);
  updateBrakeInput(now);

  if (updateInput(leftInput, now) == 1) {
    toggleIndicator(true);
  }
  if (updateInput(rightInput, now) == 1) {
    toggleIndicator(false);
  }

  if (canReady) {
    int packetSize = 0;
    while ((packetSize = mcp.parsePacket()) > 0) {
      handleCanFrame(packetSize);
    }
  }

  if ((leftEnabled || rightEnabled) && now - lastBlinkMs >= BLINK_HALF_PERIOD_MS) {
    // Advance by one half-period. This remains non-blocking so brake and CAN
    // events are serviced throughout the blink cycle.
    lastBlinkMs = now;
    blinkPhaseOn = !blinkPhaseOn;
    updateRelayOutputs();
  }

  if (now - lastHeartbeatMs >= HEARTBEAT_MS) {
    lastHeartbeatMs = now;
    Serial.printf("[HB] CAN=%s tx=%lu failed=%lu rx=%lu\n",
                  canReady ? "ready" : "FAILED",
                  static_cast<unsigned long>(sentFrames),
                  static_cast<unsigned long>(failedFrames),
                  static_cast<unsigned long>(receivedFrames));
  }
}
