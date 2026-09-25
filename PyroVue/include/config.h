#ifndef CONFIG_H
#define CONFIG_H

// ESP32-S3 Feather TFT pins. ESP8266 values are supplied by the target env.
#if defined(ARDUINO_ARCH_ESP8266)
  #define THERMOCOUPLE_CS  D2
  #define THERMOCOUPLE_MOSI D7
  #define THERMOCOUPLE_MISO D6
  #define THERMOCOUPLE_SCK  D5
#else
  #define TFT_CS 7
  #define TFT_DC 39
  #define TFT_RST 40
  #define TFT_BL 45
  #define THERMOCOUPLE_CS 8
  #define THERMOCOUPLE_MOSI 35
  #define THERMOCOUPLE_MISO 37
  #define THERMOCOUPLE_SCK 36
#endif

#define WIFI_SSID                 "Kiln-AP"
#define WIFI_PASSWORD             "kiln-password"
#define AP_CHANNEL                6
#define WS_MAX_CLIENTS            1
#define SAMPLE_HZ                 2
#define RECENT_HISTORY_SECONDS    600
#define RAW_HISTORY_CAPACITY      (SAMPLE_HZ * RECENT_HISTORY_SECONDS)
#define ESP8266_RAW_HISTORY_CAPACITY (SAMPLE_HZ * 300)
#define COARSE_BUCKET_MS          30000UL
#define COARSE_HISTORY_CAPACITY   480
#define HISTORY_CHUNK_BYTES       1024
#define PROTOCOL_VERSION          2
#define FIRMWARE_VERSION          "rework-2"

#endif // CONFIG_H