#ifndef CONFIG_H
#define CONFIG_H

// Pin definitions for the ST7789 TFT Display
#define TFT_CS 7
#define TFT_DC 39
#define TFT_RST 40
#define TFT_BL 45 // Backlight

// Pin definition for the MAX31855 Thermocouple
#define THERMOCOUPLE_CS 8
#define THERMOCOUPLE_MOSI 35
#define THERMOCOUPLE_MISO 37
#define THERMOCOUPLE_SCK 36

// WiFi Access Point credentials
#define WIFI_SSID "PyroVue"
#define WIFI_PASSWORD "pyrovue123"

#endif // CONFIG_H