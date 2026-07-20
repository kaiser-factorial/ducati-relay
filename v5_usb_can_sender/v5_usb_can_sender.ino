/*
 * Ducati v5 USB-to-CAN bench sender
 *
 * Hardware: USB-capable classic ESP32 DevKit + SN65HVD230 CAN transceiver
 * Serial:   115200 baud, newline ending
 * CAN:      500 kbit/s, standard 11-bit frames
 */

#include <driver/twai.h>

// ESP32's built-in TWAI controller can route TX/RX to these free GPIOs.
constexpr gpio_num_t CAN_TX_PIN = GPIO_NUM_21;
constexpr gpio_num_t CAN_RX_PIN = GPIO_NUM_22;
constexpr uint8_t LEFT_TEST_SWITCH_PIN = 18; // momentary switch to GND
constexpr uint32_t SWITCH_DEBOUNCE_MS = 35;

constexpr uint32_t CAN_LEFT_ID  = 0x300;
constexpr uint32_t CAN_RIGHT_ID = 0x301;
constexpr uint32_t CAN_BRAKE_ID = 0x303;
constexpr uint32_t CAN_HAZARD_ID = 0x304;

bool canReady = false;
bool leftState = false;
bool rightState = false;
bool brakeState = false;
String commandBuffer;
uint32_t lastHeartbeatMs = 0;
uint32_t busErrorsSinceHeartbeat = 0;
bool leftSwitchRawPressed = false;
bool leftSwitchStablePressed = false;
uint32_t leftSwitchChangedMs = 0;

const char *twaiStateName(twai_state_t state) {
  switch (state) {
    case TWAI_STATE_STOPPED: return "STOPPED";
    case TWAI_STATE_RUNNING: return "RUNNING";
    case TWAI_STATE_BUS_OFF: return "BUS_OFF";
    case TWAI_STATE_RECOVERING: return "RECOVERING";
    default: return "UNKNOWN";
  }
}

void printCanStatus() {
  twai_status_info_t status = {};
  if (twai_get_status_info(&status) != ESP_OK) {
    Serial.println("[CAN STATUS] unavailable");
    return;
  }

  Serial.printf(
      "[CAN STATUS] state=%s queued_tx=%lu tx_failed=%lu rx_missed=%lu "
      "arb_lost=%lu bus_errors=%lu tx_err=%lu rx_err=%lu\n",
      twaiStateName(status.state),
      static_cast<unsigned long>(status.msgs_to_tx),
      static_cast<unsigned long>(status.tx_failed_count),
      static_cast<unsigned long>(status.rx_missed_count),
      static_cast<unsigned long>(status.arb_lost_count),
      static_cast<unsigned long>(status.bus_error_count),
      static_cast<unsigned long>(status.tx_error_counter),
      static_cast<unsigned long>(status.rx_error_counter));
}

void processCanAlerts() {
  if (!canReady) {
    return;
  }

  uint32_t alerts = 0;
  if (twai_read_alerts(&alerts, 0) != ESP_OK || alerts == 0) {
    return;
  }

  if (alerts & TWAI_ALERT_TX_SUCCESS) {
    Serial.println("[CAN ACK] Frame transmitted and acknowledged by another CAN node.");
  }
  if (alerts & TWAI_ALERT_TX_FAILED) {
    Serial.println("[CAN ERROR] Transmit failed - no ACK; check wiring, bitrate, and termination.");
  }
  if (alerts & TWAI_ALERT_BUS_ERROR) {
    busErrorsSinceHeartbeat++;
  }
  if (alerts & TWAI_ALERT_ARB_LOST) {
    Serial.println("[CAN] Arbitration lost; controller will retry automatically.");
  }
  if (alerts & TWAI_ALERT_ABOVE_ERR_WARN) {
    Serial.println("[CAN WARN] Error counters crossed the warning threshold.");
  }
  if (alerts & TWAI_ALERT_BUS_OFF) {
    Serial.println("[CAN ERROR] BUS OFF - fix the bus, then power-cycle the sender.");
  }
}

void printHelp() {
  Serial.println();
  Serial.println("Commands (press Enter after each):");
  Serial.println("  l       Toggle left indicator");
  Serial.println("  r       Toggle right indicator");
  Serial.println("  h       Toggle both indicators (hazards)");
  Serial.println("  bon     Brake ON");
  Serial.println("  boff    Brake OFF");
  Serial.println("  status  Show light state and CAN controller counters");
  Serial.println("  help    Show this list");
  Serial.println();
}

void printState() {
  Serial.printf("[STATE] left=%s right=%s brake=%s\n",
                leftState ? "ON" : "OFF",
                rightState ? "ON" : "OFF",
                brakeState ? "ON" : "OFF");
}

bool sendFrame(uint32_t id, uint8_t value) {
  if (!canReady) {
    Serial.println("[TX] CAN is not ready.");
    return false;
  }

  twai_message_t message = {};
  message.identifier = id;
  message.data_length_code = 1;
  message.data[0] = value;
  // Single-shot mode gives one clear success/failure result instead of retrying forever
  // when the bench bus is disconnected or the receiver is not acknowledging frames.
  message.flags = TWAI_MSG_FLAG_SS;

  if (twai_transmit(&message, pdMS_TO_TICKS(100)) != ESP_OK) {
    Serial.println("[TX] CAN packet failed (check bus wiring and termination).");
    return false;
  }

  Serial.printf("[TX QUEUED] ID=0x%03lX DLC=1 data[0]=0x%02X; awaiting CAN ACK...\n",
                static_cast<unsigned long>(id), value);
  return true;
}

void toggleLeft() {
  const bool requestedState = !leftState;
  if (sendFrame(CAN_LEFT_ID, requestedState ? 0x01 : 0x00)) {
    leftState = requestedState;
  }
}

void updateLeftTestSwitch(uint32_t now) {
  const bool rawPressed = digitalRead(LEFT_TEST_SWITCH_PIN) == LOW;

  if (rawPressed != leftSwitchRawPressed) {
    leftSwitchRawPressed = rawPressed;
    leftSwitchChangedMs = now;
  }

  if (rawPressed == leftSwitchStablePressed ||
      now - leftSwitchChangedMs < SWITCH_DEBOUNCE_MS) {
    return;
  }

  leftSwitchStablePressed = rawPressed;
  if (leftSwitchStablePressed) {
    Serial.println("[SWITCH] GPIO18 pressed -> sending left-toggle CAN frame.");
    toggleLeft();
    printState();
  } else {
    Serial.println("[SWITCH] GPIO18 released.");
  }
}

void toggleRight() {
  const bool requestedState = !rightState;
  if (sendFrame(CAN_RIGHT_ID, requestedState ? 0x01 : 0x00)) {
    rightState = requestedState;
  }
}

void toggleHazards() {
  // If both are already on, turn both off. Otherwise explicitly turn both on.
  const bool requestedState = !(leftState && rightState);
  if (sendFrame(CAN_HAZARD_ID, requestedState ? 0x01 : 0x00)) {
    leftState = requestedState;
    rightState = requestedState;
  }
}

void processCommand(String command) {
  command.trim();
  command.toLowerCase();

  if (command.length() == 0) {
    return;
  }

  if (command == "l") {
    toggleLeft();
  } else if (command == "r") {
    toggleRight();
  } else if (command == "h") {
    toggleHazards();
  } else if (command == "bon") {
    if (sendFrame(CAN_BRAKE_ID, 0x01)) {
      brakeState = true;
    }
  } else if (command == "boff") {
    if (sendFrame(CAN_BRAKE_ID, 0x00)) {
      brakeState = false;
    }
  } else if (command == "status") {
    printState();
    printCanStatus();
    return;
  } else if (command == "help" || command == "?") {
    printHelp();
    return;
  } else {
    Serial.print("[USB] Unknown command: ");
    Serial.println(command);
    printHelp();
    return;
  }

  printState();
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== Ducati v5 USB-to-CAN Sender ===");

  pinMode(LEFT_TEST_SWITCH_PIN, INPUT_PULLUP);
  leftSwitchRawPressed = digitalRead(LEFT_TEST_SWITCH_PIN) == LOW;
  leftSwitchStablePressed = leftSwitchRawPressed;

  twai_general_config_t generalConfig =
      TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_PIN, CAN_RX_PIN, TWAI_MODE_NORMAL);
  generalConfig.alerts_enabled =
      TWAI_ALERT_TX_SUCCESS |
      TWAI_ALERT_TX_FAILED |
      TWAI_ALERT_BUS_ERROR |
      TWAI_ALERT_ARB_LOST |
      TWAI_ALERT_ABOVE_ERR_WARN |
      TWAI_ALERT_BUS_OFF;
  const twai_timing_config_t timingConfig = TWAI_TIMING_CONFIG_500KBITS();
  const twai_filter_config_t filterConfig = TWAI_FILTER_CONFIG_ACCEPT_ALL();

  if (twai_driver_install(&generalConfig, &timingConfig, &filterConfig) != ESP_OK) {
    Serial.println("[CAN] ERROR: ESP32 TWAI driver installation failed.");
  } else if (twai_start() != ESP_OK) {
    Serial.println("[CAN] ERROR: ESP32 TWAI controller failed to start.");
  } else {
    canReady = true;
    Serial.println("[CAN] TWAI ready at 500 kbit/s; TX=GPIO21 RX=GPIO22");
  }

  commandBuffer.reserve(32);
  printHelp();
}

void loop() {
  processCanAlerts();

  const uint32_t now = millis();
  updateLeftTestSwitch(now);

  while (Serial.available()) {
    const char incoming = static_cast<char>(Serial.read());
    if (incoming == '\n' || incoming == '\r') {
      if (commandBuffer.length() > 0) {
        processCommand(commandBuffer);
        commandBuffer = "";
      }
    } else if (commandBuffer.length() < 31) {
      commandBuffer += incoming;
    }
  }

  if (now - lastHeartbeatMs >= 2000) {
    lastHeartbeatMs = now;
    Serial.println("[HB] USB sender alive");
    if (busErrorsSinceHeartbeat > 0) {
      Serial.printf("[CAN ERROR] %lu bus error event(s) since last heartbeat.\n",
                    static_cast<unsigned long>(busErrorsSinceHeartbeat));
      busErrorsSinceHeartbeat = 0;
    }
    printCanStatus();
  }
}
