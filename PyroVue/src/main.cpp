#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include "Adafruit_MAX31855.h"
#include "Adafruit_ST7789.h"
#include "config.h"
#include "DisplayManager.h"
#include "StatusManager.h"

// --- Hardware SPI Bus Declaration for Thermocouple ---
SPIClass spi_thermo(FSPI);

// --- Global Object Declarations ---
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);
Adafruit_MAX31855 thermocouple(THERMOCOUPLE_CS, &spi_thermo);
DisplayManager displayManager(tft);
StatusManager statusManager(displayManager);

// --- WiFi Event Handlers ---
void onAPClientConnected(WiFiEvent_t event, WiFiEventInfo_t info) {
  Serial.println("Client connected");
  displayManager.displayWifiStatus();
}

void onAPClientDisconnected(WiFiEvent_t event, WiFiEventInfo_t info) {
  Serial.println("Client disconnected");
  displayManager.displayWifiStatus();
}

// --- Main Program ---
void setup() {
  Serial.begin(115200);
  Serial.println("PyroVue Initializing...");

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

  // --- WiFi Access Point Setup ---
  Serial.println("Setting up WiFi Access Point...");
  WiFi.softAP(WIFI_SSID, WIFI_PASSWORD);
  displayManager.displayWifiStatus(); // Initial WiFi status display
  Serial.print("AP IP address: ");
  Serial.println(WiFi.softAPIP());

  // --- Register WiFi Events ---
  WiFi.onEvent(onAPClientConnected, ARDUINO_EVENT_WIFI_AP_STACONNECTED);
  WiFi.onEvent(onAPClientDisconnected, ARDUINO_EVENT_WIFI_AP_STADISCONNECTED);

  Serial.println("Setup Complete.");
}

void loop() {
  // 1. Read sensor data
  double c = thermocouple.readCelsius();
  uint8_t fault = thermocouple.readError();

  // 2. Update system status (this handles all display changes)
  statusManager.update(c, fault);

  // 3. Print temperature to serial if the system is in a normal state
  if (statusManager.getCurrentState() == STATE_NORMAL) {
    Serial.print("Temperature: ");
    Serial.print(c);
    Serial.println(" C");
  }

  delay(1000);
}