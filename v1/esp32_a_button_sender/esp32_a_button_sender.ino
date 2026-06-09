// ESP32 "A" — reads three pushbuttons and broadcasts each one's state over CAN
// via an MCP2515 SPI CAN controller module, using Adafruit's "Adafruit MCP2515"
// library. Each button has its own CAN ID, so they can be controlled
// independently on the receiving end.
//
// Library: install "Adafruit MCP2515" via Arduino Library Manager
// (arduino-cli lib install "Adafruit MCP2515")
//
// Wiring (each button: one leg -> its GPIO pin, other leg -> GND; uses the
// ESP32's internal pull-up, so no external resistor is needed):
//   Button 1 -> GPIO 27  (sends CAN ID 0x100)
//   Button 2 -> GPIO 25  (sends CAN ID 0x101)
//   Button 3 -> GPIO 26  (sends CAN ID 0x102)
//   MCP2515 module: VCC -> 3.3V or 5V, GND -> GND
//                   SCK -> GPIO 18, MISO -> GPIO 19, MOSI -> GPIO 23, CS -> GPIO 5
//   (those SPI pins are the ESP32's default VSPI bus, so the library's default
//    constructor — which uses the default SPI bus — matches this wiring with
//    no extra configuration)
//
// IMPORTANT: this library assumes a 16MHz crystal on the MCP2515 module by
// default. Check the small oscillator can on your board — if it's marked 8MHz,
// uncomment MCP2515_CRYSTAL_8MHZ below.

#include <Adafruit_MCP2515.h>

#define CS_PIN              5
#define CAN_BAUDRATE        500000   // bits per second; must match the receiver
#define HEARTBEAT_PERIOD_MS 2000

// #define MCP2515_CRYSTAL_8MHZ

Adafruit_MCP2515 mcp(CS_PIN);

struct ButtonChannel {
  const char *name;
  uint8_t     pin;
  uint32_t    canId;
  bool        lastState;
};

ButtonChannel channels[] = {
  { "Button 1", 27, 0x100, HIGH },
  { "Button 2", 25, 0x101, HIGH },
  { "Button 3", 26, 0x102, HIGH },
};
const int NUM_CHANNELS = sizeof(channels) / sizeof(channels[0]);

unsigned long lastHeartbeat = 0;
unsigned long sendCount = 0;
unsigned long sendFailCount = 0;

void setup() {
  Serial.begin(115200);
  delay(500); // give the USB-serial bridge a moment so early prints aren't lost

  Serial.println();
  Serial.println("======================================");
  Serial.println("ESP32 A — CAN button sender booting up");
  Serial.println("======================================");
  Serial.printf("CS_PIN=%d  CAN_BAUDRATE=%ld\n", CS_PIN, (long)CAN_BAUDRATE);

  for (int i = 0; i < NUM_CHANNELS; i++) {
    pinMode(channels[i].pin, INPUT_PULLUP);
    Serial.printf("  %s: pin=%d  canId=0x%lX  initial level=%s\n",
                  channels[i].name, channels[i].pin, (unsigned long)channels[i].canId,
                  digitalRead(channels[i].pin) == LOW ? "LOW (pressed)" : "HIGH (released)");
  }

#ifdef MCP2515_CRYSTAL_8MHZ
  Serial.println("Setting MCP2515 clock frequency to 8 MHz");
  mcp.setClockFrequency(8e6);
#else
  Serial.println("Using default MCP2515 clock frequency assumption (16 MHz)");
#endif

  Serial.println("Calling mcp.begin() ...");
  if (!mcp.begin(CAN_BAUDRATE)) {
    Serial.println("!!! mcp.begin() FAILED — MCP2515 not found or not responding.");
    Serial.println("!!! Check: CS/SCK/MISO/MOSI wiring, module power (VCC/GND),");
    Serial.println("!!! and whether MCP2515_CRYSTAL_8MHZ needs to be uncommented.");
    while (1) delay(10);
  }
  Serial.println("mcp.begin() succeeded — MCP2515 found and configured.");

  Serial.println("--- MCP2515 register dump (for deep debugging) ---");
  mcp.dumpRegisters(Serial);
  Serial.println("---------------------------------------------------");

  Serial.println("Sender ready. Press/release any button to transmit its CAN frame.");
  Serial.println();
}

void sendButtonState(ButtonChannel &ch, bool pressed) {
  uint8_t payload = pressed ? 0x01 : 0x00;

  Serial.printf("[%8lu ms] >>> %s edge: %s -> transmitting id=0x%lX data=0x%02X\n",
                millis(), ch.name, pressed ? "PRESSED " : "RELEASED",
                (unsigned long)ch.canId, payload);

  mcp.beginPacket(ch.canId);
  mcp.write(payload);
  int ok = mcp.endPacket();

  sendCount++;
  if (ok) {
    Serial.printf("[%8lu ms]     send #%lu: OK\n", millis(), sendCount);
  } else {
    sendFailCount++;
    Serial.printf("[%8lu ms]     send #%lu: FAILED (endPacket()==0) — total failures: %lu\n",
                  millis(), sendCount, sendFailCount);
  }
}

void loop() {
  for (int i = 0; i < NUM_CHANNELS; i++) {
    ButtonChannel &ch = channels[i];
    bool currentState = digitalRead(ch.pin);

    if (currentState != ch.lastState) {
      delay(30); // simple debounce
      currentState = digitalRead(ch.pin);
      if (currentState != ch.lastState) {
        bool pressed = (currentState == LOW); // INPUT_PULLUP: LOW = pressed
        sendButtonState(ch, pressed);
        ch.lastState = currentState;
      }
    }
  }

  // Heartbeat: confirms the board is alive and shows the live level of every
  // button plus running send/fail counts.
  if (millis() - lastHeartbeat >= HEARTBEAT_PERIOD_MS) {
    lastHeartbeat = millis();
    Serial.printf("[%8lu ms] heartbeat — sends ok: %lu | sends failed: %lu | ",
                  millis(), sendCount - sendFailCount, sendFailCount);
    for (int i = 0; i < NUM_CHANNELS; i++) {
      Serial.printf("%s=%s%s", channels[i].name,
                    digitalRead(channels[i].pin) == LOW ? "LOW " : "HIGH",
                    i < NUM_CHANNELS - 1 ? ", " : "");
    }
    Serial.println();
  }
}
