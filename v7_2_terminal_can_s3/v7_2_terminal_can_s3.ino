/*
 * Ducati v7.2 native-USB CAN terminal for ESP32-S3
 *
 * USB: native USB Serial/JTAG CDC
 * CAN: classic CAN at 500 kbit/s through an external 3.3 V transceiver
 * Pins: GPIO4 = TWAI TX, GPIO5 = TWAI RX
 */

#include <Arduino.h>
#include <driver/twai.h>

constexpr gpio_num_t CAN_TX_PIN = GPIO_NUM_4;
constexpr gpio_num_t CAN_RX_PIN = GPIO_NUM_5;
constexpr uint32_t CAN_BITRATE = 500000;
constexpr size_t COMMAND_CAPACITY = 192;

char commandBuffer[COMMAND_CAPACITY];
size_t commandLength = 0;
bool canReady = false;
bool listenEnabled = true;
bool singleShot = true;

const char *stateName(twai_state_t state) {
  switch (state) {
    case TWAI_STATE_STOPPED: return "stopped";
    case TWAI_STATE_RUNNING: return "running";
    case TWAI_STATE_BUS_OFF: return "bus-off";
    case TWAI_STATE_RECOVERING: return "recovering";
    default: return "unknown";
  }
}

void printFrame(const char *direction, const twai_message_t &frame) {
  Serial.print(direction);
  Serial.print(' ');
  Serial.printf(frame.extd ? "%08lX#" : "%03lX#",
                static_cast<unsigned long>(frame.identifier));
  if (frame.rtr) {
    Serial.printf("R%u", frame.data_length_code);
  } else {
    for (uint8_t i = 0; i < frame.data_length_code; ++i) {
      Serial.printf("%02X", frame.data[i]);
    }
  }
  Serial.println();
}

bool parseHexRange(const char *text, size_t length, uint32_t maximum, uint32_t &value) {
  if (length == 0 || length > 8) return false;
  value = 0;
  for (size_t i = 0; i < length; ++i) {
    const char c = text[i];
    uint8_t digit;
    if (c >= '0' && c <= '9') digit = c - '0';
    else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
    else return false;
    value = (value << 4) | digit;
  }
  return value <= maximum;
}

bool sendFrame(twai_message_t &frame) {
  if (!canReady) {
    Serial.println("err can-not-ready");
    return false;
  }
  frame.ss = singleShot ? 1 : 0;
  const esp_err_t result = twai_transmit(&frame, pdMS_TO_TICKS(20));
  if (result != ESP_OK) {
    Serial.printf("err tx-queue 0x%X\n", static_cast<unsigned int>(result));
    return false;
  }
  printFrame("tx", frame);
  return true;
}

void parseCompactFrame(char *command) {
  char *separator = strchr(command, '#');
  if (separator == nullptr || strchr(separator + 1, '#') != nullptr) {
    Serial.println("err syntax (expected 123#AABB or 12345678#AABB)");
    return;
  }

  const size_t idLength = separator - command;
  const bool extended = idLength == 8;
  if (idLength != 3 && !extended) {
    Serial.println("err id-width (use exactly 3 hex digits for standard or 8 for extended)");
    return;
  }

  uint32_t identifier = 0;
  if (!parseHexRange(command, idLength, extended ? 0x1FFFFFFF : 0x7FF, identifier)) {
    Serial.println("err invalid-id");
    return;
  }

  twai_message_t frame = {};
  frame.identifier = identifier;
  frame.extd = extended ? 1 : 0;
  char *payload = separator + 1;
  const size_t payloadLength = strlen(payload);

  if (payloadLength > 0 && (payload[0] == 'R' || payload[0] == 'r')) {
    uint32_t dlc = 0;
    if (!parseHexRange(payload + 1, payloadLength - 1, 8, dlc)) {
      Serial.println("err invalid-rtr-dlc");
      return;
    }
    frame.rtr = 1;
    frame.data_length_code = static_cast<uint8_t>(dlc);
  } else {
    if ((payloadLength & 1) != 0 || payloadLength > 16) {
      Serial.println("err payload (use 0 to 16 hex digits / 0 to 8 bytes)");
      return;
    }
    for (size_t offset = 0; offset < payloadLength; offset += 2) {
      uint32_t byteValue = 0;
      if (!parseHexRange(payload + offset, 2, 0xFF, byteValue)) {
        Serial.println("err invalid-data");
        return;
      }
      frame.data[frame.data_length_code++] = static_cast<uint8_t>(byteValue);
    }
  }
  sendFrame(frame);
}

void printStatus() {
  twai_status_info_t status = {};
  if (!canReady || twai_get_status_info(&status) != ESP_OK) {
    Serial.println("status unavailable");
    return;
  }
  Serial.printf(
      "status state=%s bitrate=%lu txq=%lu rxq=%lu failed=%lu missed=%lu "
      "arb_lost=%lu bus_errors=%lu txerr=%lu rxerr=%lu listen=%s single=%s\n",
      stateName(status.state), static_cast<unsigned long>(CAN_BITRATE),
      static_cast<unsigned long>(status.msgs_to_tx),
      static_cast<unsigned long>(status.msgs_to_rx),
      static_cast<unsigned long>(status.tx_failed_count),
      static_cast<unsigned long>(status.rx_missed_count),
      static_cast<unsigned long>(status.arb_lost_count),
      static_cast<unsigned long>(status.bus_error_count),
      static_cast<unsigned long>(status.tx_error_counter),
      static_cast<unsigned long>(status.rx_error_counter),
      listenEnabled ? "on" : "off", singleShot ? "on" : "off");
}

void printHelp() {
  Serial.println("Ducati v7.2 ESP32-S3 native-USB CAN terminal");
  Serial.println("  300#01              standard ID 0x300, data 01");
  Serial.println("  123#DEADBEEF        standard ID 0x123, four bytes");
  Serial.println("  00000123#DEAD       extended ID 0x123, two bytes");
  Serial.println("  18DAF110#021003     extended ID 0x18DAF110, three bytes");
  Serial.println("  123#R8              standard remote frame, DLC 8");
  Serial.println("  :listen on|off      print or discard received frames");
  Serial.println("  :single on|off      single-shot transmission (default on)");
  Serial.println("  :status             CAN state and counters");
  Serial.println("  :recover            recover from bus-off");
  Serial.println("  :help               this help");
}

void processControlCommand(char *command) {
  for (char *p = command; *p != '\0'; ++p) *p = static_cast<char>(tolower(*p));
  if (strcmp(command, ":help") == 0 || strcmp(command, "?") == 0) {
    printHelp();
  } else if (strcmp(command, ":status") == 0) {
    printStatus();
  } else if (strcmp(command, ":listen on") == 0) {
    listenEnabled = true;
    Serial.println("ok listen=on");
  } else if (strcmp(command, ":listen off") == 0) {
    listenEnabled = false;
    Serial.println("ok listen=off");
  } else if (strcmp(command, ":single on") == 0) {
    singleShot = true;
    Serial.println("ok single=on");
  } else if (strcmp(command, ":single off") == 0) {
    singleShot = false;
    Serial.println("ok single=off");
  } else if (strcmp(command, ":recover") == 0) {
    const esp_err_t result = twai_initiate_recovery();
    if (result == ESP_OK) Serial.println("ok recovery-started");
    else Serial.printf("err recovery 0x%X\n", static_cast<unsigned int>(result));
  } else {
    Serial.println("err unknown-command (:help)");
  }
}

void processCommand() {
  commandBuffer[commandLength] = '\0';
  char *start = commandBuffer;
  while (*start == ' ' || *start == '\t') ++start;
  char *end = start + strlen(start);
  while (end > start && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
  if (*start == '\0') return;
  if (*start == ':' || strcmp(start, "?") == 0) processControlCommand(start);
  else parseCompactFrame(start);
}

void processUsbInput() {
  while (Serial.available() > 0) {
    const char incoming = static_cast<char>(Serial.read());
    if (incoming == '\n' || incoming == '\r') {
      if (commandLength > 0) processCommand();
      commandLength = 0;
    } else if (commandLength < COMMAND_CAPACITY - 1) {
      commandBuffer[commandLength++] = incoming;
    } else {
      commandLength = 0;
      Serial.println("err command-too-long");
    }
  }
}

void processCan() {
  twai_message_t frame = {};
  while (canReady && twai_receive(&frame, 0) == ESP_OK) {
    if (listenEnabled) printFrame("rx", frame);
  }

  uint32_t alerts = 0;
  if (!canReady || twai_read_alerts(&alerts, 0) != ESP_OK) return;
  if (alerts & TWAI_ALERT_TX_SUCCESS) Serial.println("ack");
  if (alerts & TWAI_ALERT_TX_FAILED) Serial.println("err no-ack");
  if (alerts & TWAI_ALERT_BUS_OFF) Serial.println("err bus-off (:recover after fixing bus)");
  if (alerts & TWAI_ALERT_BUS_RECOVERED) {
    if (twai_start() == ESP_OK) Serial.println("ok bus-recovered");
    else Serial.println("err restart-after-recovery");
  }
}

void setup() {
  Serial.begin(115200);  // Baud is ignored by native USB CDC.
  const uint32_t waitStarted = millis();
  while (!Serial && millis() - waitStarted < 2500) delay(10);

  twai_general_config_t general =
      TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_PIN, CAN_RX_PIN, TWAI_MODE_NORMAL);
  general.tx_queue_len = 32;
  general.rx_queue_len = 64;
  general.alerts_enabled = TWAI_ALERT_TX_SUCCESS | TWAI_ALERT_TX_FAILED |
                           TWAI_ALERT_BUS_OFF | TWAI_ALERT_BUS_RECOVERED;
  const twai_timing_config_t timing = TWAI_TIMING_CONFIG_500KBITS();
  const twai_filter_config_t filter = TWAI_FILTER_CONFIG_ACCEPT_ALL();

  if (twai_driver_install(&general, &timing, &filter) != ESP_OK) {
    Serial.println("err twai-install");
  } else if (twai_start() != ESP_OK) {
    Serial.println("err twai-start");
  } else {
    canReady = true;
    Serial.println("ready v7.2 s3 usb-cdc can=500000 tx=4 rx=5");
  }
  printHelp();
}

void loop() {
  processUsbInput();
  processCan();
  delay(1);
}
