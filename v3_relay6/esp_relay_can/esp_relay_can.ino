/*
 * Transmitter code for the main ESP32-S3 ("esp_relay").
 * 
 * Includes verbose serial debugging for CAN packet transmission.
 */

#include <SPI.h>
#include <Adafruit_MCP2515.h>

#define PIN_BTN_1 47
#define PIN_BTN_2 48
#define PIN_BTN_3 17

#define SPI_MOSI 7
#define SPI_MISO 14
#define SPI_SCK  6
#define SPI_CS   16

Adafruit_MCP2515 mcp(SPI_CS);

struct Button {
  uint8_t pin;
  bool lastState;
  bool isPressed;
  unsigned long lastDebounceTime;
};

Button btn1 = { PIN_BTN_1, HIGH, false, 0 };
Button btn2 = { PIN_BTN_2, HIGH, false, 0 };
Button btn3 = { PIN_BTN_3, HIGH, false, 0 };

const unsigned long debounceDelay = 50;

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(PIN_BTN_1, INPUT_PULLUP);
  pinMode(PIN_BTN_2, INPUT_PULLUP);
  pinMode(PIN_BTN_3, INPUT_PULLUP);

  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, -1);

  Serial.println("\n--- ESP RELAY (TRANSMITTER) BOOTING ---");
  Serial.println("Initializing MCP2515...");
  mcp.setClockFrequency(12e6);

  if (!mcp.begin(500000)) {
    Serial.println("ERROR: Failed to initialize MCP2515. Check SPI wiring.");
    while (1) delay(10);
  }

  Serial.println("SUCCESS: MCP2515 Initialized at 500kbps, 12MHz clock.");
  Serial.println("Waiting for button presses...\n");
}

int checkButton(Button &btn) {
  bool reading = digitalRead(btn.pin);
  int result = 0;

  if (reading != btn.lastState) {
    btn.lastDebounceTime = millis();
  }

  if ((millis() - btn.lastDebounceTime) > debounceDelay) {
    if (reading != btn.isPressed) {
      btn.isPressed = reading;
      if (btn.isPressed == LOW) result = 1; 
      else result = 2; 
    }
  }

  btn.lastState = reading;
  return result;
}

void sendCanMessage(uint8_t buttonId, bool turnOn) {
  Serial.println("---------------------------------------------");
  Serial.print("EVENT: Button ");
  Serial.print(buttonId);
  Serial.println(turnOn ? " PUSHED IN" : " POPPED OUT");

  Serial.print("TX -> ID: 0x123 | DLC: 2 | Data: [0x");
  Serial.print(buttonId, HEX);
  Serial.print(", 0x");
  Serial.print(turnOn ? 1 : 0, HEX);
  Serial.println("]");

  mcp.beginPacket(0x123);
  mcp.write(buttonId);
  mcp.write(turnOn ? 1 : 0);
  int success = mcp.endPacket();

  if (success) {
    Serial.println("STATUS -> SUCCESS: Packet sent to CAN controller.");
  } else {
    Serial.println("STATUS -> FAILED: TX buffer full (no other node acknowledged the packet).");
  }
  Serial.println("---------------------------------------------\n");
}

void loop() {
  int state1 = checkButton(btn1);
  if (state1 == 1) sendCanMessage(1, true);
  else if (state1 == 2) sendCanMessage(1, false);
  
  int state2 = checkButton(btn2);
  if (state2 == 1) sendCanMessage(2, true);
  else if (state2 == 2) sendCanMessage(2, false);
  
  int state3 = checkButton(btn3);
  if (state3 == 1) sendCanMessage(3, true);
  else if (state3 == 2) sendCanMessage(3, false);
}
