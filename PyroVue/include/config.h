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
#define WIFI_SSID                 "Kiln-AP"
#define WIFI_PASSWORD             "kiln-password"
#define AP_CHANNEL                6
#define WS_MAX_CLIENTS            4
#define SAMPLE_HZ                 2          // sensor read rate
#define HISTORY_LEN               1200       // ~10 min @ 2 Hz
#define UI_HISTORY_ON_CONNECT     300        // send last 2.5 min on connect
#define PROTOCOL_VERSION          "lv1"

#endif // CONFIG_H