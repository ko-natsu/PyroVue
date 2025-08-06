#include <Arduino.h>
#include <SPI.h>
#include "Adafruit_MAX31855.h"
#include "Adafruit_ST7789.h"
#include "config.h"
#include "DisplayManager.h"

// --- Hardware SPI Bus Declaration for Thermocouple ---
// The ESP32-S3 has two main hardware SPI controllers. We will use HSPI for the thermocouple.
SPIClass spi_thermo(FSPI);

// --- Global Object Declarations ---

// 1. The TFT Display Object
// We use the simple constructor. The library will automatically use the default hardware
// SPI bus (FSPI) and the correct hardwired pins for the built-in display.
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// 2. The Thermocouple Object
// We tell the library to use our dedicated HSPI bus.
Adafruit_MAX31855 thermocouple(THERMOCOUPLE_CS, &spi_thermo);

// 3. The Display Manager
// It gets a reference to the global TFT object.
DisplayManager displayManager(tft);

// --- Main Program ---

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10); // Wait for serial port
  Serial.println("PyroVue Initializing...");

  // --- Initialize Hardware ---

  // Initialize the dedicated HSPI bus for the thermocouple
  spi_thermo.begin(THERMOCOUPLE_SCK, THERMOCOUPLE_MISO, THERMOCOUPLE_MOSI);

  // Initialize the Thermocouple on its dedicated bus
  if (!thermocouple.begin()) {
    Serial.println("ERROR: Thermocouple initialization failed!");
    while (1) delay(10); // Halt on critical error
  }
  Serial.println("Thermocouple Initialized on HSPI.");

  // Now, initialize the TFT display. The library handles the SPI bus.
  tft.init(135, 240);
  tft.setRotation(1);
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
  Serial.println("TFT Initialized on default FSPI.");

  // Set up the display manager and show the startup message
  displayManager.begin();
  displayManager.showStartupMessage();
  delay(2000);

  Serial.println("Setup Complete.");
}

void loop() {
  double c = thermocouple.readCelsius();
  uint8_t fault = thermocouple.readError();

  if (fault) {
    displayManager.displayError("FAULT");
    Serial.print("Thermocouple Fault: ");
    if (fault & MAX31855_FAULT_OPEN) Serial.println("Open circuit");
    if (fault & MAX31855_FAULT_SHORT_GND) Serial.println("Shorted to GND");
    if (fault & MAX31855_FAULT_SHORT_VCC) Serial.println("Shorted to VCC");
  } else if (isnan(c)) {
    displayManager.displayError("NaN");
    Serial.println("Thermocouple reading failed (NaN).");
  } else {
    displayManager.displayTemperature(c);
    Serial.print("Temperature: ");
    Serial.print(c);
    Serial.println(" C");
  }

  delay(1000);
}
