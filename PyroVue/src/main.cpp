#include <Arduino.h>
#include <SPI.h>
#include "Adafruit_MAX31855.h"
#include "Adafruit_ST7789.h"
#include "config.h"
#include "protocol.h"
#include "DisplayManager.h"
#include "StatusManager.h"
#include "TelemetryBuffer.h"
#include "NetworkManager.h"
#include "PresetManager.h"
#include <ArduinoJson.h>

// --- Hardware SPI Bus Declaration for Thermocouple ---
SPIClass spi_thermo(FSPI);

// --- Global Object Declarations ---
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);
Adafruit_MAX31855 thermocouple(THERMOCOUPLE_CS, &spi_thermo);
DisplayManager displayManager(tft);
StatusManager statusManager(displayManager);
TelemetryBuffer telemetryBuffer;
PresetManager presetManager;
NetworkManager networkManager(telemetryBuffer, presetManager);

// --- Timers ---
unsigned long lastSampleTime = 0;
unsigned long lastStatusTime = 0;
const unsigned long sampleInterval = 1000 / SAMPLE_HZ;
const unsigned long statusInterval = 1000;

// --- Command Handler ---
void handleCommand(const String& cmd) {
    Serial.print("Command received: ");
    Serial.println(cmd);

    JsonDocument doc;
    deserializeJson(doc, cmd);
    const char* type = doc["type"];

    if (strcmp(type, "preset") == 0) {
        String preset = doc["value"];
        presetManager.setPreset(preset);
    }
}

// --- Main Program ---
void setup() {
  Serial.begin(115200);
  Serial.println("PyroVue Initializing...");

  // --- Initialize Managers ---
  presetManager.begin();

  // --- Initialize Hardware ---
  spi_thermo.begin(THERMOCOUPLE_SCK, THERMOCOUPLE_MISO, THERMOCOUPLE_MOSI);
  if (!thermocouple.begin()) {
    Serial.println("ERROR: Thermocouple initialization failed!");
    while (1) delay(10); // Halt
  }
  Serial.println("Thermocouple Initialized.");

  tft.init(135, 240);
  tft.setRotation(1);
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
  Serial.println("TFT Initialized.");

  // --- Initial Display Setup ---
  displayManager.begin();
  displayManager.showStartupMessage();
  delay(2000);
  displayManager.clearFullScreen(); // Clear startup message

  // --- Network Setup ---
  networkManager.begin();
  networkManager.onCommand = handleCommand;

  Serial.println("Setup Complete.");
}

void loop() {
  // --- Timed Sensor Reading ---
  if (millis() - lastSampleTime >= sampleInterval) {
    lastSampleTime = millis();

    // 1. Read sensor data
    double c = thermocouple.readCelsius();
    uint8_t fault = thermocouple.readError();

    // 2. Create a sample
    TempSample s = {lastSampleTime, (float)c, fault};

    // 3. Push to buffers and managers
    telemetryBuffer.push(s);
    networkManager.onNewSample(s);
    statusManager.update(c, fault);

  }

  // --- Timed Status Update ---
  if (millis() - lastStatusTime >= statusInterval) {
      lastStatusTime = millis();
      displayManager.displayNetworkStatus(networkManager.clientCount());
  }

  // --- Handle Network Clients ---
  networkManager.loop();
}
