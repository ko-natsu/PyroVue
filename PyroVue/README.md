# PyroVue backend

PyroVue is an Arduino/PlatformIO kiln telemetry firmware. The backend owns thermocouple sampling, explicit run identity, bounded telemetry history, and WebSocket delivery. The browser owns graphing and presentation.

## Targets

- `adafruit_feather_esp32s3_tft`: ESP32-S3 with the existing ST7789 display.
- `esp8266_d1mini`: D1 mini-class ESP8266, TFT-free, with MAX31855 pins `D2` (CS), `D7` (MOSI), `D6` (MISO), and `D5` (SCK).
- `native`: core-test configuration. PlatformIO native tests require a host `gcc`/`g++` toolchain on Windows.

The ESP32 and ESP8266 targets use maintained ESP32Async networking libraries. SSE is removed; WebSocket `/ws` is the only live telemetry transport. HTTP remains available at `/`, `/api/now`, `/api/info`, and `POST /api/cmd`.

## History configuration
Sampling remains 2 Hz. ESP32 retains 10 minutes of raw samples; ESP8266 uses 5 minutes to protect its smaller heap. Coarse history aggregates finalized 30-second buckets with average, minimum, maximum, and ORed fault flags. History is persisted as validated coarse records in LittleFS at bucket cadence and on run stop; a bad or truncated record tail is ignored during recovery. The raw tier remains RAM-only and bounded.

## Protocol

Protocol v2 is documented in [`docs/protocol-v2.md`](docs/protocol-v2.md). A connection receives `hello`, ordered `history.begin`/`history`/`history.end` messages, then live `sample` messages. Samples carry `runId`, `seq`, run-relative `ms`, `temp`, and `fault`; clients must merge by those identifiers rather than by reconnect time.

Commands are `run.start`, `run.stop`, and `preset`. Run IDs are persisted once per start. A reboot does not silently resume an active run.

## Resource measurements

Measured with `pio run` after the protocol and target split:

| Environment | RAM | Flash |
|---|---:|---:|
| ESP32-S3 TFT | 91,832 / 327,680 (28.0%) | 878,021 / 1,441,792 (60.9%) |
| ESP8266 D1 mini | 54,804 / 81,920 (66.9%) | 366,449 / 1,044,464 (35.1%) |

These are static build reports, not minimum-free-heap measurements under a live history replay. Hardware soak and reconnect qualification remain required before deployment.
