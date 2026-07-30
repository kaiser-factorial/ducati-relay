/*
 * Ducati v7 serial CAN terminal
 *
 * Hardware: classic ESP32 DevKit + SN65HVD230 CAN transceiver
 * Serial:   115200 baud, newline ending
 * CAN:      500 kbit/s, classic CAN (11-bit and 29-bit identifiers)
 */

#include <driver/twai.h>

constexpr gpio_num_t CAN_TX_PIN = GPIO_NUM_21;
constexpr gpio_num_t CAN_RX_PIN = GPIO_NUM_22;
constexpr size_t MAX_COMMAND_LENGTH = 160;

bool canReady = false;
bool printReceivedFrames = true;
bool singleShot = true;
String commandBuffer;

const char *twaiStateName(twai_state_t state) {
  switch (state) {
    case TWAI_STATE_STOPPED: return "STOPPED";
    case TWAI_STATE_RUNNING: return "RUNNING";
    case TWAI_STATE_BUS_OFF: return "BUS_OFF";
    case TWAI_STATE_RECOVERING: return "RECOVERING";
    default: return "UNKNOWN";
  }
}

void printHelp() {
  Serial.println();
  Serial.println("Commands (hex values; press Enter after each):");
  Serial.println("  send  <id> [b0 ... b7]   Send an 11-bit data frame");
  Serial.println("  sendx <id> [b0 ... b7]   Send a 29-bit data frame");
  Serial.println("  rtr   <id> <dlc>         Send an 11-bit remote frame");
  Serial.println("  rtrx  <id> <dlc>         Send a 29-bit remote frame");
  Serial.println("  listen on|off            Enable/disable received-frame printing");
  Serial.println("  single on|off            Single-shot TX on/off (default on)");
  Serial.println("  status                   Show CAN controller counters");
  Serial.println("  recover                  Start recovery after bus-off");
  Serial.println("  help                     Show this list");
  Serial.println();
  Serial.println("Examples:");
  Serial.println("  send 300 01");
  Serial.println("  send 123 DE AD BE EF");
  Serial.println("  sendx 18DAF110 02 10 03");
  Serial.println("  send 321               (valid zero-byte frame)");
  Serial.println();
}

void printCanStatus() {
  twai_status_info_t status = {};
  if (!canReady || twai_get_status_info(&status) != ESP_OK) {
    Serial.println("[CAN STATUS] unavailable");
    return;
  }

  Serial.printf(
      "[CAN STATUS] state=%s tx_queue=%lu rx_queue=%lu tx_failed=%lu "
      "rx_missed=%lu arb_lost=%lu bus_errors=%lu tx_err=%lu rx_err=%lu "
      "listen=%s single_shot=%s\n",
      twaiStateName(status.state),
      static_cast<unsigned long>(status.msgs_to_tx),
      static_cast<unsigned long>(status.msgs_to_rx),
      static_cast<unsigned long>(status.tx_failed_count),
      static_cast<unsigned long>(status.rx_missed_count),
      static_cast<unsigned long>(status.arb_lost_count),
      static_cast<unsigned long>(status.bus_error_count),
      static_cast<unsigned long>(status.tx_error_counter),
      static_cast<unsigned long>(status.rx_error_counter),
      printReceivedFrames ? "on" : "off",
      singleShot ? "on" : "off");
}

bool parseHex(const char *text, uint32_t maximum, uint32_t &value) {
  if (text == nullptr || *text == '\0' || *text == '-') {
    return false;
  }

  char *end = nullptr;
  const unsigned long parsed = strtoul(text, &end, 16);
  if (*end != '\0' || parsed > maximum) {
    return false;
  }
  value = static_cast<uint32_t>(parsed);
  return true;
}

void printFrame(const char *prefix, const twai_message_t &message) {
  Serial.printf("[%s %10lu ms] %s ID=0x%0*lX DLC=%u",
                prefix,
                static_cast<unsigned long>(millis()),
                message.extd ? "EXT" : "STD",
                message.extd ? 8 : 3,
                static_cast<unsigned long>(message.identifier),
                message.data_length_code);

  if (message.rtr) {
    Serial.print(" RTR");
  } else {
    Serial.print(" DATA=");
    for (uint8_t i = 0; i < message.data_length_code; ++i) {
      if (i != 0) Serial.print(' ');
      Serial.printf("%02X", message.data[i]);
    }
    if (message.data_length_code == 0) Serial.print("<empty>");
  }
  Serial.println();
}

bool transmitFrame(twai_message_t &message) {
  if (!canReady) {
    Serial.println("[TX ERROR] CAN is not ready.");
    return false;
  }

  if (singleShot) message.ss = 1;
  const esp_err_t result = twai_transmit(&message, pdMS_TO_TICKS(100));
  if (result != ESP_OK) {
    Serial.printf("[TX ERROR] Queue failed (code 0x%X). Check status and bus state.\n",
                  static_cast<unsigned int>(result));
    return false;
  }

  printFrame("TX QUEUED", message);
  return true;
}

void processSendCommand(char *savePointer, bool extended, bool remote) {
  char *idToken = strtok_r(nullptr, " \t,", &savePointer);
  uint32_t identifier = 0;
  const uint32_t maximumId = extended ? 0x1FFFFFFF : 0x7FF;
  if (!parseHex(idToken, maximumId, identifier)) {
    Serial.printf("[INPUT ERROR] Expected a %s CAN ID from 0 to 0x%lX.\n",
                  extended ? "29-bit" : "11-bit",
                  static_cast<unsigned long>(maximumId));
    return;
  }

  twai_message_t message = {};
  message.identifier = identifier;
  message.extd = extended ? 1 : 0;
  message.rtr = remote ? 1 : 0;

  if (remote) {
    char *dlcToken = strtok_r(nullptr, " \t,", &savePointer);
    uint32_t dlc = 0;
    if (!parseHex(dlcToken, 8, dlc) || strtok_r(nullptr, " \t,", &savePointer) != nullptr) {
      Serial.println("[INPUT ERROR] Remote frame syntax is: rtr <id> <dlc>, where DLC is 0-8.");
      return;
    }
    message.data_length_code = static_cast<uint8_t>(dlc);
  } else {
    char *byteToken = nullptr;
    while ((byteToken = strtok_r(nullptr, " \t,", &savePointer)) != nullptr) {
      if (message.data_length_code >= 8) {
        Serial.println("[INPUT ERROR] Classic CAN carries at most 8 data bytes.");
        return;
      }
      uint32_t byteValue = 0;
      if (!parseHex(byteToken, 0xFF, byteValue)) {
        Serial.printf("[INPUT ERROR] Invalid hex byte: %s\n", byteToken);
        return;
      }
      message.data[message.data_length_code++] = static_cast<uint8_t>(byteValue);
    }
  }

  transmitFrame(message);
}

void processCommand(String command) {
  command.trim();
  if (command.length() == 0) return;

  char input[MAX_COMMAND_LENGTH + 1];
  command.toCharArray(input, sizeof(input));
  char *savePointer = nullptr;
  char *verb = strtok_r(input, " \t,", &savePointer);
  if (verb == nullptr) return;
  for (char *p = verb; *p; ++p) *p = static_cast<char>(tolower(*p));

  if (strcmp(verb, "send") == 0) {
    processSendCommand(savePointer, false, false);
  } else if (strcmp(verb, "sendx") == 0) {
    processSendCommand(savePointer, true, false);
  } else if (strcmp(verb, "rtr") == 0) {
    processSendCommand(savePointer, false, true);
  } else if (strcmp(verb, "rtrx") == 0) {
    processSendCommand(savePointer, true, true);
  } else if (strcmp(verb, "listen") == 0 || strcmp(verb, "single") == 0) {
    char *setting = strtok_r(nullptr, " \t,", &savePointer);
    const bool valid = setting != nullptr &&
        (strcmp(setting, "on") == 0 || strcmp(setting, "off") == 0) &&
        strtok_r(nullptr, " \t,", &savePointer) == nullptr;
    if (!valid) {
      Serial.printf("[INPUT ERROR] Usage: %s on|off\n", verb);
      return;
    }
    const bool enabled = strcmp(setting, "on") == 0;
    if (strcmp(verb, "listen") == 0) printReceivedFrames = enabled;
    else singleShot = enabled;
    Serial.printf("[CONFIG] %s=%s\n", verb, enabled ? "on" : "off");
  } else if (strcmp(verb, "status") == 0) {
    printCanStatus();
  } else if (strcmp(verb, "recover") == 0) {
    const esp_err_t result = twai_initiate_recovery();
    if (result == ESP_OK) Serial.println("[CAN] Bus-off recovery started.");
    else Serial.printf("[CAN] Recovery not started (code 0x%X; controller must be BUS_OFF).\n",
                       static_cast<unsigned int>(result));
  } else if (strcmp(verb, "help") == 0 || strcmp(verb, "?") == 0) {
    printHelp();
  } else {
    Serial.printf("[INPUT ERROR] Unknown command: %s\n", verb);
    printHelp();
  }
}

void processCanAlerts() {
  uint32_t alerts = 0;
  if (!canReady || twai_read_alerts(&alerts, 0) != ESP_OK || alerts == 0) return;

  if (alerts & TWAI_ALERT_TX_SUCCESS) Serial.println("[TX ACK] A CAN node acknowledged the frame.");
  if (alerts & TWAI_ALERT_TX_FAILED) Serial.println("[TX FAILED] No ACK; check bitrate, wiring, and termination.");
  if (alerts & TWAI_ALERT_ARB_LOST) Serial.println("[CAN] Arbitration lost.");
  if (alerts & TWAI_ALERT_ABOVE_ERR_WARN) Serial.println("[CAN WARN] Error warning threshold crossed.");
  if (alerts & TWAI_ALERT_BUS_OFF) Serial.println("[CAN ERROR] BUS OFF. Fix the bus, then type: recover");
  if (alerts & TWAI_ALERT_BUS_RECOVERED) {
    if (twai_start() == ESP_OK) Serial.println("[CAN] Bus recovered and controller restarted.");
    else Serial.println("[CAN ERROR] Bus recovered, but the controller did not restart.");
  }
}

void drainReceivedFrames() {
  if (!canReady) return;
  twai_message_t message = {};
  while (twai_receive(&message, 0) == ESP_OK) {
    if (printReceivedFrames) printFrame("RX", message);
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== Ducati v7 Serial CAN Terminal ===");

  twai_general_config_t generalConfig =
      TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_PIN, CAN_RX_PIN, TWAI_MODE_NORMAL);
  generalConfig.alerts_enabled =
      TWAI_ALERT_TX_SUCCESS | TWAI_ALERT_TX_FAILED | TWAI_ALERT_ARB_LOST |
      TWAI_ALERT_ABOVE_ERR_WARN | TWAI_ALERT_BUS_OFF | TWAI_ALERT_BUS_RECOVERED;
  const twai_timing_config_t timingConfig = TWAI_TIMING_CONFIG_500KBITS();
  const twai_filter_config_t filterConfig = TWAI_FILTER_CONFIG_ACCEPT_ALL();

  if (twai_driver_install(&generalConfig, &timingConfig, &filterConfig) != ESP_OK) {
    Serial.println("[CAN ERROR] TWAI driver installation failed.");
  } else if (twai_start() != ESP_OK) {
    Serial.println("[CAN ERROR] TWAI controller failed to start.");
  } else {
    canReady = true;
    Serial.println("[CAN] Ready: 500 kbit/s, TX=GPIO21, RX=GPIO22, accept-all filter");
  }

  commandBuffer.reserve(MAX_COMMAND_LENGTH);
  printHelp();
}

void loop() {
  processCanAlerts();
  drainReceivedFrames();

  while (Serial.available()) {
    const char incoming = static_cast<char>(Serial.read());
    if (incoming == '\n' || incoming == '\r') {
      if (commandBuffer.length() > 0) {
        processCommand(commandBuffer);
        commandBuffer = "";
      }
    } else if (commandBuffer.length() < MAX_COMMAND_LENGTH) {
      commandBuffer += incoming;
    } else {
      commandBuffer = "";
      Serial.println("[INPUT ERROR] Command was too long and has been discarded.");
    }
  }
}
