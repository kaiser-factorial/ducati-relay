/*
 * Receiver code for the secondary ESP32 ("esp_b") using MCP2515 via SPI.
 * 
 * NOTE: Relay functionality has been temporarily removed for debugging.
 * It will now just print EVERY received CAN packet to the Serial Monitor
 * and turn on the Blue Onboard LED for 1 second if it receives the button packet.
 */

#include <SPI.h>
#include <Adafruit_MCP2515.h>

#define LED_PIN 2

#define SPI_MOSI 23
#define SPI_MISO 19
#define SPI_SCK  18
#define SPI_CS   5

Adafruit_MCP2515 mcp(SPI_CS);

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, -1);

  Serial.println("\n--- ESP_B (RECEIVER) BOOTING ---");
  Serial.println("Initializing MCP2515...");
  
  // Set the clock to 8MHz to match the crystal
  mcp.setClockFrequency(8e6);

  if (!mcp.begin(500000)) {
    Serial.println("ERROR: Failed to initialize MCP2515. Check SPI wiring.");
    while (1) delay(10);
  }

  Serial.println("SUCCESS: MCP2515 Initialized at 500kbps, 8MHz clock.");
  Serial.println("Listening for CAN packets on the bus...\n");
}

void loop() {
  int packetSize = mcp.parsePacket();
  
  if (packetSize) { 
    long id = mcp.packetId();
    bool isRtr = mcp.packetRtr();
    
    Serial.println("---------------------------------------------");
    Serial.print("RX <- ID: 0x");
    Serial.print(id, HEX);
    Serial.print(" | DLC: ");
    Serial.print(packetSize);
    Serial.print(" | RTR: ");
    Serial.print(isRtr ? "Yes" : "No");
    Serial.print(" | Data: [");
    
    // Create an array to hold the payload temporarily so we can process it
    uint8_t payload[8] = {0};
    
    for (int i = 0; i < packetSize; i++) {
      payload[i] = mcp.read();
      Serial.print(" 0x");
      Serial.print(payload[i], HEX);
    }
    Serial.println(" ]");
    
    // If we received our specific button payload
    if (id == 0x123 && !isRtr && packetSize >= 2) {
      Serial.print("ACTION -> Triggering Blue LED for 1 second (Button ");
      Serial.print(payload[0]);
      Serial.println(")");
      
      digitalWrite(LED_PIN, HIGH);
      delay(1000); // Wait 1 second
      digitalWrite(LED_PIN, LOW);
    }
    
    Serial.println("---------------------------------------------\n");
  }
}
