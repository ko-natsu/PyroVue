#ifndef CONFIG_H
#define CONFIG_H

// N16R8 octal PSRAM occupies GPIO35..37; use the DevKitC header SPI pins.
#if defined(PYROVUE_BOARD_N16R8)
  #define THERMOCOUPLE_CS 10
  #define THERMOCOUPLE_MOSI -1
  #define THERMOCOUPLE_MISO 13
  #define THERMOCOUPLE_SCK 12
#elif defined(ARDUINO_ARCH_ESP8266)
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
#if defined(PYROVUE_BOARD_N16R8)
  #define SAMPLE_HZ                 1
  #define RECENT_HISTORY_SECONDS    300
  #define RAW_HISTORY_CAPACITY      300
  #define COARSE_BUCKET_MS          5000UL
  #define COARSE_HISTORY_CAPACITY   60
  #define RUN_LOG_INTERVAL_MS       2000UL
  #define RUN_LOG_FLUSH_MS          5000UL
  #define RUN_MAX_DURATION_MS       86400000UL
  #define RUN_MAX_RECORDS           43202UL
  #define RUN_EVENT_BUDGET_BYTES    262144UL
  #define RUN_EVENT_LIFECYCLE_RESERVE_BYTES 8192UL
  #define RUN_META_BUDGET_BYTES     65536UL
  #define RUN_FS_HEADROOM_BYTES     1048576UL
  #define RUN_SAMPLE_QUEUE_CAPACITY 64
  #define RUN_PENDING_RECORD_CAPACITY 32
  #define RUN_REQUEST_QUEUE_CAPACITY 8
  #define RUN_RESPONSE_SLOTS        4
  #define RUN_RESPONSE_BYTES        32768UL
#else
  #define SAMPLE_HZ                 2
  #define RECENT_HISTORY_SECONDS    600
  #define RAW_HISTORY_CAPACITY      (SAMPLE_HZ * RECENT_HISTORY_SECONDS)
  #define COARSE_BUCKET_MS          30000UL
  #define COARSE_HISTORY_CAPACITY   480
#endif
#define ESP8266_RAW_HISTORY_CAPACITY (SAMPLE_HZ * 300)
#define HISTORY_CHUNK_BYTES       1024
#define PROTOCOL_VERSION          2
#define FIRMWARE_VERSION          "rework-2"

#endif // CONFIG_H