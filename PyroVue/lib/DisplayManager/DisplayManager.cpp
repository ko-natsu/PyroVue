#include "DisplayManager.h"
#include <config.h>
#include <Arduino.h>

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

void DisplayManager::displayTemperature(float temperature) {
    // Clear the entire screen to prevent old text from persisting
    tft.fillScreen(ST77XX_BLACK);

    // Set cursor to the starting position for the text
    tft.setCursor(10, 10);
    tft.setTextColor(ST77XX_WHITE);
    tft.setTextSize(2);

    // Print the new temperature reading
    tft.print("Temperature: ");
    tft.print(temperature);
    tft.println(" C");
}

void DisplayManager::displayError(const char* error) {
    tft.fillScreen(ST77XX_RED);
    tft.setCursor(10, 10);
    tft.setTextColor(ST77XX_WHITE);
    tft.setTextSize(2);
    tft.println(error);
}