#include "DisplayManager.h"
#include <config.h>
#include <Arduino.h>
#include <WiFi.h>

// The constructor now initializes the reference to the TFT object
DisplayManager::DisplayManager(Adafruit_ST7789& tft_ref) : tft(tft_ref) {
}

void DisplayManager::begin() {
    // The initialization is now done in main.cpp before calling this
    // This function will just set up the initial state of the display
    tft.setRotation(1);
    tft.fillScreen(ST77XX_BLACK);
}

void DisplayManager::showStartupMessage() {
    tft.setCursor(10, 10);
    tft.setTextColor(ST77XX_WHITE);
    tft.setTextSize(2);
    tft.println("Hello, PyroVue!");
}

void DisplayManager::clearFullScreen() {
    tft.fillScreen(ST77XX_BLACK);
}

void DisplayManager::displayTemperature(float temperature) {
    // Only update the display if the temperature has changed
    if (abs(temperature - last_temp) < 0.1) {
        return; // Exit if the change is negligible
    }

    clearTemperatureArea(); // Clear the specific area for the temperature

    // Set cursor to the starting position for the text
    tft.setCursor(10, 10);
    tft.setTextColor(ST77XX_WHITE);
    tft.setTextSize(2);

    // Print the new temperature reading
    tft.print("Temperature: ");
    tft.print(temperature);
    tft.println(" C");

    last_temp = temperature; // Update the last known temperature
}

void DisplayManager::clearTemperatureArea() {
    // Define the rectangle area where the temperature is displayed
    // This needs to be adjusted based on your font size and layout
    int16_t x = 10; // Starting X coordinate
    int16_t y = 10; // Starting Y coordinate
    uint16_t w = 220; // Width of the area to clear
    uint16_t h = 16;  // Height of the area (adjust for font size 2)
    tft.fillRect(x, y, w, h, ST77XX_BLACK);
}

void DisplayManager::displayError(const char* error) {
    tft.fillScreen(ST77XX_RED);
    tft.setCursor(10, 10);
    tft.setTextColor(ST77XX_WHITE);
    tft.setTextSize(2);
    tft.println(error);
}

void DisplayManager::clearWifiArea() {
    int16_t x = 0; // Starting X coordinate
    int16_t y = 58; // Starting Y

    uint16_t w = 220; // Width
    uint16_t h = 48;  // Height
    tft.fillRect(x, y, w, h, ST77XX_BLACK);
}


void DisplayManager::displayWifiStatus() {
    clearWifiArea();
    tft.setCursor(10, 58);

    tft.setTextColor(ST77XX_WHITE);
    tft.setTextSize(2);
    tft.println("WiFi Status:");
    int numClients = WiFi.softAPgetStationNum();
    if (numClients > 0) {
        tft.setTextColor(ST77XX_GREEN);
        tft.print("Connected");
        tft.print(" (");
        tft.print(numClients);
        tft.print(")");
    } else {
        tft.setTextColor(ST77XX_RED);
        tft.println("Not connected");
    }
}

void DisplayManager::displayErrorHalt(const char* message) {
    tft.fillScreen(ST77XX_RED);
    tft.setCursor(10, 10);
    tft.setTextColor(ST77XX_WHITE);
    tft.setTextSize(2);
    tft.println("FATAL ERROR:");
    tft.setTextSize(1);
    tft.setCursor(10, 40);
    tft.println(message);
    while(1) { delay(100); } // Halt execution
}