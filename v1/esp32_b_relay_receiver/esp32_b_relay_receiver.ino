// ESP32 "B" — listens on CAN (via an MCP2515 SPI CAN controller module, using
// Adafruit's "Adafruit MCP2515" library) and drives three relays based on
// three independently-addressed CAN messages.
//
// Library: install "Adafruit MCP2515" via Arduino Library Manager
// (arduino-cli lib install "Adafruit MCP2515")
//
// Wiring:
//   Relay 1: signal -> GPIO 4   (responds to CAN ID 0x100)
//   Relay 2: signal -> GPIO 32  (responds to CAN ID 0x101)
//   Relay 3: signal -> GPIO 33  (responds to CAN ID 0x102)
//   Each relay module: VCC -> 3.3V or 5V (per its spec), GND -> ESP32 GND
//   Relay COM/NO contacts -> battery + LED per channel (isolated load side)
//   MCP2515 module: VCC -> 3.3V or 5V, GND -> GND
//
// Status LED: this sketch also drives the board's ONBOARD LED (GPIO 2 on most
// ESP32 DevKit boards) to indicate which relay is currently active, since a
// single-color LED can't show different colors:
//   Relay 1 active -> solid on
//   Relay 2 active -> slow flash
//   Relay 3 active -> rapid flash
//   none active    -> off
// (If more than one relay is on at once, the lowest-numbered active channel's
// pattern wins.) NOTE: GPIO 2 was previously used for Relay 1 — it's been
// moved to GPIO 4 to free GPIO 2 up exclusively for the onboard LED. If you
// had Relay 1's signal wire on GPIO 2, move it to GPIO 4.
//                   SCK -> GPIO 18, MISO -> GPIO 19, MOSI -> GPIO 23, CS -> GPIO 5
//   (those SPI pins are the ESP32's default VSPI bus, so the library's default
//    constructor — which uses the default SPI bus — matches this wiring with
//    no extra configuration)
//
// IMPORTANT: this library assumes a 16MHz crystal on the MCP2515 module by
// default. Check the small oscillator can on your board — if it's marked 8MHz,
// uncomment MCP2515_CRYSTAL_8MHZ below.
//
// NOTE: many cheap relay modules are "active LOW" (energize when the signal
// pin is driven LOW). If a relay turns on when its button is *released*
// instead of pressed, flip RELAY_ACTIVE_LOW to true below (it applies to all
// three channels — swap individual modules if only one behaves oddly).

#include <Adafruit_MCP2515.h>

#define CS_PIN              5
#define CAN_BAUDRATE        500000   // bits per second; must match the sender
#define RELAY_ACTIVE_LOW    false
#define HEARTBEAT_PERIOD_MS 2000

#define STATUS_LED_PIN        2
#define STATUS_LED_ACTIVE_LOW false  // flip to true if the onboard LED lights when the pin is driven LOW

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
};
const int NUM_CHANNELS = sizeof(channels) / sizeof(channels[0]);

// Status-LED blink pattern per channel: half-period in ms (time spent on,
// and separately off); 0 means "solid on" rather than blinking.
const unsigned long LED_HALF_PERIOD_MS[] = { 0, 500, 100 };

unsigned long lastHeartbeat = 0;
unsigned long packetsSeen = 0;
unsigned long packetsMatched = 0;
int lastLedChannel = -2; // -2 = "uninitialized", forces the first state to log

void setRelay(RelayChannel &ch, bool on) {
  ch.isOn = on;
  bool level = RELAY_ACTIVE_LOW ? !on : on;
  digitalWrite(ch.pin, level ? HIGH : LOW);
  Serial.printf("[%8lu ms]     >>> %s -> %s (driving pin %d %s)\n",
                millis(), ch.name, on ? "ON " : "OFF", ch.pin, level ? "HIGH" : "LOW");
}

void writeStatusLed(bool lit) {
  digitalWrite(STATUS_LED_PIN, (STATUS_LED_ACTIVE_LOW ? !lit : lit) ? HIGH : LOW);
}

// Drives the onboard status LED to reflect whichever relay is currently on.
// Lowest-numbered active channel wins if more than one relay is on. Uses
// millis()-based timing so it never blocks the CAN receive loop.
void updateStatusLed() {
  int activeChannel = -1;
  for (int i = 0; i < NUM_CHANNELS; i++) {
    if (channels[i].isOn) { activeChannel = i; break; }
  }

  if (activeChannel != lastLedChannel) {
    lastLedChannel = activeChannel;
    if (activeChannel < 0) {
      Serial.printf("[%8lu ms]     status LED -> off (no relay active)\n", millis());
    } else {
      unsigned long halfPeriod = LED_HALF_PERIOD_MS[activeChannel];
      const char *pattern = (halfPeriod == 0) ? "solid on"
                          : (halfPeriod >= 300 ? "slow flash" : "rapid flash");
      Serial.printf("[%8lu ms]     status LED -> %s (tracking %s)\n",
                    millis(), pattern, channels[activeChannel].name);
    }
  }

  if (activeChannel < 0) {
    writeStatusLed(false);
    return;
  }

  unsigned long halfPeriod = LED_HALF_PERIOD_MS[activeChannel];
  bool lit = (halfPeriod == 0) ? true : ((millis() / halfPeriod) % 2 == 0);
  writeStatusLed(lit);
}

int findChannel(uint32_t canId) {
  for (int i = 0; i < NUM_CHANNELS; i++) {
    if (channels[i].canId == canId) return i;
  }
  return -1;
}

void setup() {
  Serial.begin(115200);
  delay(500); // give the USB-serial bridge a moment so early prints aren't lost

  Serial.println();
  Serial.println("========================================");
  Serial.println("ESP32 B — CAN relay receiver booting up");
  Serial.println("========================================");
  Serial.printf("CS_PIN=%d  CAN_BAUDRATE=%ld  RELAY_ACTIVE_LOW=%s\n",
                CS_PIN, (long)CAN_BAUDRATE, RELAY_ACTIVE_LOW ? "true" : "false");

  for (int i = 0; i < NUM_CHANNELS; i++) {
    pinMode(channels[i].pin, OUTPUT);
    Serial.printf("  %s: pin=%d  canId=0x%lX  LED pattern=%s\n",
                  channels[i].name, channels[i].pin, (unsigned long)channels[i].canId,
                  LED_HALF_PERIOD_MS[i] == 0 ? "solid"
                  : (LED_HALF_PERIOD_MS[i] >= 300 ? "slow flash" : "rapid flash"));
    setRelay(channels[i], false);
  }

  pinMode(STATUS_LED_PIN, OUTPUT);
  writeStatusLed(false);
  Serial.printf("  Status LED: pin=%d (mirrors whichever relay is active)\n", STATUS_LED_PIN);

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

  Serial.println("Receiver ready. Waiting for CAN packets...");
  Serial.println();
}

void loop() {
  updateStatusLed();

  int packetSize = mcp.parsePacket();

  if (packetSize > 0) {
    packetsSeen++;
    long id = mcp.packetId();
    bool extended = mcp.packetExtended();
    bool rtr = mcp.packetRtr();
    int chIndex = findChannel((uint32_t)id);

    Serial.printf("[%8lu ms] RX #%lu: id=0x%lX (%s%s) len=%d",
                  millis(), packetsSeen, id,
                  extended ? "extended" : "standard",
                  rtr ? ", RTR" : "",
                  packetSize);

    if (rtr) {
      Serial.println(chIndex >= 0 ? "" : "  (ignored — RTR frame)");
    } else {
      uint8_t firstByte = 0;
      Serial.print(" data=[");
      for (int i = 0; i < packetSize; i++) {
        int b = mcp.read();
        if (i == 0) firstByte = (uint8_t)b;
        Serial.printf("%s0x%02X", i > 0 ? " " : "", b);
      }
      Serial.print("]");

      if (chIndex >= 0) {
        packetsMatched++;
        Serial.printf("  <- MATCH (#%lu) -> %s\n", packetsMatched, channels[chIndex].name);
        setRelay(channels[chIndex], firstByte == 0x01);
      } else {
        Serial.printf("  (ignored — id 0x%lX not mapped to a relay)\n", id);
      }
    }
  }

  // Heartbeat: confirms the board is alive and shows running totals — handy
  // for spotting "nothing arriving" vs. "arriving but not matching any channel".
  if (millis() - lastHeartbeat >= HEARTBEAT_PERIOD_MS) {
    lastHeartbeat = millis();
    Serial.printf("[%8lu ms] heartbeat — packets seen: %lu | matched: %lu\n",
                  millis(), packetsSeen, packetsMatched);
  }
}
