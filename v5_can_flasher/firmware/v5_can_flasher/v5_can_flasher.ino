/*
 * Ducati relay v5 CAN flasher bridge
 *
 * Turns a classic ESP32 DevKit plus an SN65HVD230 transceiver into a
 * Lawicel/SLCAN serial-to-CAN adapter for Linux SocketCAN.
 *
 * USB serial: 921600 baud, SLCAN ASCII protocol
 * CAN: selected with S6 (500 kbit/s); standard and extended classic CAN
 * Pins: TWAI TX GPIO21, TWAI RX GPIO22
 *
 * Intentionally emits no boot banners, diagnostics, or heartbeats because
 * unsolicited text would corrupt the SLCAN protocol stream.
 */

#include <Arduino.h>
#include <driver/twai.h>

constexpr gpio_num_t CAN_TX_PIN = GPIO_NUM_21;
constexpr gpio_num_t CAN_RX_PIN = GPIO_NUM_22;
constexpr uint32_t SERIAL_BAUD = 921600;
constexpr size_t COMMAND_CAPACITY = 32;

char commandBuffer[COMMAND_CAPACITY];
size_t commandLength = 0;
bool driverInstalled = false;
bool channelOpen = false;
bool bitrateSelected = false;
uint8_t statusFlags = 0;

constexpr uint8_t STATUS_RX_FIFO_FULL = 0x01;
constexpr uint8_t STATUS_TX_FIFO_FULL = 0x02;
constexpr uint8_t STATUS_ERROR_WARNING = 0x04;
constexpr uint8_t STATUS_DATA_OVERRUN = 0x08;
constexpr uint8_t STATUS_ERROR_PASSIVE = 0x20;
constexpr uint8_t STATUS_ARBITRATION_LOST = 0x40;
constexpr uint8_t STATUS_BUS_ERROR = 0x80;

int hexNibble(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  return -1;
}

bool parseHex(const char *text, size_t count, uint32_t &result) {
  result = 0;
  for (size_t index = 0; index < count; index++) {
    const int nibble = hexNibble(text[index]);
    if (nibble < 0) return false;
    result = (result << 4) | static_cast<uint32_t>(nibble);
  }
  return true;
}

void acknowledge() {
  Serial.write('\r');
}

void rejectCommand() {
  Serial.write('\a');
}

void closeChannel() {
  if (channelOpen) {
    twai_stop();
    channelOpen = false;
  }
  if (driverInstalled) {
    twai_driver_uninstall();
    driverInstalled = false;
  }
}

bool openChannel() {
  if (channelOpen) return false;
  if (!bitrateSelected) return false;

  twai_general_config_t generalConfig =
      TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_PIN, CAN_RX_PIN, TWAI_MODE_NORMAL);
  generalConfig.tx_queue_len = 16;
  generalConfig.rx_queue_len = 32;
  generalConfig.alerts_enabled =
      TWAI_ALERT_RX_QUEUE_FULL |
      TWAI_ALERT_ABOVE_ERR_WARN |
      TWAI_ALERT_ERR_PASS |
      TWAI_ALERT_ARB_LOST |
      TWAI_ALERT_BUS_ERROR |
      TWAI_ALERT_BUS_OFF;

  const twai_timing_config_t timingConfig = TWAI_TIMING_CONFIG_500KBITS();
  const twai_filter_config_t filterConfig = TWAI_FILTER_CONFIG_ACCEPT_ALL();

  if (twai_driver_install(&generalConfig, &timingConfig, &filterConfig) != ESP_OK) {
    return false;
  }
  driverInstalled = true;

  if (twai_start() != ESP_OK) {
    closeChannel();
    return false;
  }

  channelOpen = true;
  statusFlags = 0;
  return true;
}

bool transmitFrame(const char *command, size_t length) {
  if (!channelOpen || length < 5) return false;

  const bool extended = command[0] == 'T' || command[0] == 'R';
  const bool remote = command[0] == 'r' || command[0] == 'R';
  const size_t idDigits = extended ? 8 : 3;
  const size_t dlcIndex = 1 + idDigits;

  if (length <= dlcIndex) return false;

  uint32_t identifier = 0;
  if (!parseHex(command + 1, idDigits, identifier)) return false;
  if ((!extended && identifier > 0x7FF) || (extended && identifier > 0x1FFFFFFF)) {
    return false;
  }

  const int dlcValue = hexNibble(command[dlcIndex]);
  if (dlcValue < 0 || dlcValue > 8) return false;
  const uint8_t dlc = static_cast<uint8_t>(dlcValue);
  const size_t expectedLength = dlcIndex + 1 + (remote ? 0 : dlc * 2);
  if (length != expectedLength) return false;

  twai_message_t message = {};
  message.identifier = identifier;
  message.data_length_code = dlc;
  if (extended) message.flags |= TWAI_MSG_FLAG_EXTD;
  if (remote) message.flags |= TWAI_MSG_FLAG_RTR;

  for (uint8_t index = 0; index < dlc && !remote; index++) {
    uint32_t byteValue = 0;
    if (!parseHex(command + dlcIndex + 1 + index * 2, 2, byteValue)) return false;
    message.data[index] = static_cast<uint8_t>(byteValue);
  }

  const esp_err_t result = twai_transmit(&message, pdMS_TO_TICKS(100));
  if (result == ESP_ERR_TIMEOUT) statusFlags |= STATUS_TX_FIFO_FULL;
  return result == ESP_OK;
}

void processCommand(const char *command, size_t length) {
  if (length == 0) {
    rejectCommand();
    return;
  }

  switch (command[0]) {
    case 'S':
      // Route 1 is deliberately locked to the rusEFI/OpenBLT 500 kbit/s bus.
      if (length == 2 && command[1] == '6' && !channelOpen) {
        bitrateSelected = true;
        acknowledge();
      } else {
        rejectCommand();
      }
      break;

    case 'O':
      if (length == 1 && openChannel()) acknowledge();
      else rejectCommand();
      break;

    case 'C':
      if (length == 1) {
        closeChannel();
        acknowledge();
      } else {
        rejectCommand();
      }
      break;

    case 'F':
      if (length == 1) {
        char response[5];
        snprintf(response, sizeof(response), "F%02X\r", statusFlags);
        Serial.print(response);
        statusFlags = 0;
      } else {
        rejectCommand();
      }
      break;

    case 'V':
      if (length == 1) Serial.print("V0101\r");
      else rejectCommand();
      break;

    case 'N':
      if (length == 1) Serial.print("ND5C1\r");
      else rejectCommand();
      break;

    case 'Z':
      // Timestamps are unsupported. Accept Z0 because some clients disable them.
      if (length == 2 && command[1] == '0' && !channelOpen) acknowledge();
      else rejectCommand();
      break;

    case 't':
    case 'T':
    case 'r':
    case 'R':
      if (transmitFrame(command, length)) acknowledge();
      else rejectCommand();
      break;

    default:
      rejectCommand();
      break;
  }
}

void processSerial() {
  while (Serial.available() > 0) {
    const char incoming = static_cast<char>(Serial.read());

    if (incoming == '\r') {
      processCommand(commandBuffer, commandLength);
      commandLength = 0;
    } else if (incoming == '\n') {
      // Ignore LF so CRLF terminals do not generate a second command.
    } else if (commandLength < COMMAND_CAPACITY - 1) {
      commandBuffer[commandLength++] = incoming;
    } else {
      commandLength = 0;
      rejectCommand();
    }
  }
}

void forwardReceivedFrames() {
  if (!channelOpen) return;

  twai_message_t message = {};
  while (twai_receive(&message, 0) == ESP_OK) {
    const bool extended = (message.flags & TWAI_MSG_FLAG_EXTD) != 0;
    const bool remote = (message.flags & TWAI_MSG_FLAG_RTR) != 0;
    const char type = extended ? (remote ? 'R' : 'T') : (remote ? 'r' : 't');

    Serial.write(type);
    if (extended) Serial.printf("%08lX", static_cast<unsigned long>(message.identifier));
    else Serial.printf("%03lX", static_cast<unsigned long>(message.identifier));
    Serial.printf("%X", message.data_length_code);

    if (!remote) {
      for (uint8_t index = 0; index < message.data_length_code; index++) {
        Serial.printf("%02X", message.data[index]);
      }
    }
    Serial.write('\r');
  }
}

void processAlerts() {
  if (!channelOpen) return;

  uint32_t alerts = 0;
  if (twai_read_alerts(&alerts, 0) != ESP_OK) return;

  if (alerts & TWAI_ALERT_RX_QUEUE_FULL) statusFlags |= STATUS_RX_FIFO_FULL;
  if (alerts & TWAI_ALERT_ABOVE_ERR_WARN) statusFlags |= STATUS_ERROR_WARNING;
  if (alerts & TWAI_ALERT_ERR_PASS) statusFlags |= STATUS_ERROR_PASSIVE;
  if (alerts & TWAI_ALERT_ARB_LOST) statusFlags |= STATUS_ARBITRATION_LOST;
  if (alerts & (TWAI_ALERT_BUS_ERROR | TWAI_ALERT_BUS_OFF)) statusFlags |= STATUS_BUS_ERROR;
}

void setup() {
  Serial.begin(SERIAL_BAUD);
  commandBuffer[0] = '\0';
}

void loop() {
  processSerial();
  forwardReceivedFrames();
  processAlerts();
  delay(1);
}
