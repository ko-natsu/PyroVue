#ifndef DISPLAY_MANAGER_H
#define DISPLAY_MANAGER_H

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

class DisplayManager {
public:
    // Constructor now takes a reference to the TFT object
    DisplayManager(Adafruit_ST7789& tft_ref);
    void begin();
    void showStartupMessage();
    void displayTemperature(float temperature);
    void displayError(const char* error);

private:
    // It no longer owns the tft object, it just holds a reference
    Adafruit_ST7789& tft;
};

#endif // DISPLAY_MANAGER_H