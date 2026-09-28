# PyroVue

Kiln telemetry firmware and an offline browser monitor. The device owns thermocouple sampling, run identity, bounded history and WebSocket delivery; the browser owns graphing, derived metrics, markers and presentation. For **current completion, owner-reported device validation, the multi-client reconnect-storm caveat and future work**, read [`docs/PROJECT_STATUS.md`](docs/PROJECT_STATUS.md). [`docs/protocol-v2.md`](docs/protocol-v2.md) is the wire contract.

## Targets

- `adafruit_feather_esp32s3_tft`: ESP32-S3 with the existing ST7789 display.
- `esp8266_d1mini`: D1 mini-class ESP8266, TFT-free, with MAX31855 pins `D2` (CS), `D7` (MOSI), `D6` (MISO), and `D5` (SCK).
- `native`: core-test configuration. PlatformIO native tests require a host `gcc`/`g++` toolchain on Windows.

The ESP32 and ESP8266 targets use maintained ESP32Async networking libraries. SSE is removed; WebSocket `/ws` is the only live telemetry transport. HTTP remains available at `/`, `/api/now`, `/api/info`, and `POST /api/cmd`.

## History configuration
Sampling remains 2 Hz. ESP32 retains 10 minutes of raw samples; ESP8266 uses 5 minutes to protect its smaller heap. Coarse history aggregates finalized 30-second buckets with average, minimum, maximum, and ORed fault flags. History is persisted as validated coarse records in LittleFS at bucket cadence and on run stop; a bad or truncated record tail is ignored during recovery. The raw tier remains RAM-only and bounded.

## Protocol

Protocol v2 is documented in [`docs/protocol-v2.md`](docs/protocol-v2.md). A connection receives `hello`, an ordered snapshot transaction, then live `sample` messages. Active samples carry `runId`, `seq`, run-relative `ms`, `temp`, and `fault`; reserved idle samples use `(runId, seq, ms) = (0, 0, 0)` and update the current readout without entering run history.

Commands are `run.start`, `run.stop`, and `preset`. Recognized Start/Stop commands broadcast authoritative `state` after their effects complete, and the browser performs one bounded HTTP reconciliation if that push is missed. Run IDs are persisted once per start. A reboot does not silently resume an active run.

## Frontend development

Editable frontend assets live under `web/src/`. The browser code is dependency-free at runtime and is bundled into one offline HTML document before being gzip-embedded in `include/ui_index.h`.

The frontend provides run-scoped browser-local markers, rate-of-rise regions, a witness-cone-qualified Orton cone estimate, and active-run heat colors. The screen is a monitor/logger, not a heater controller; device clients do not share marker notes.

Install development tools and run the frontend checks:

```bash
npm install
npm test
npm run typecheck
npm run lint
```

Regenerate the browser bundle and firmware header after every frontend change:

```bash
npm run build
npm run check:bundle
```

`web/make_ui_header.py` inlines local CSS, JavaScript, the pinned Oxanium TTF, and its full OFL notice; it writes the preview bundle to ignored `web/dist/index.html` and deterministically regenerates `include/ui_index.h`. The production UI has no CDN, analytics, external font, script, or API dependency.

Build either firmware target after regenerating the embedded UI:

```bash
pio run -e adafruit_feather_esp32s3_tft
pio run -e esp8266_d1mini
```

The [current-state record](docs/PROJECT_STATUS.md) distinguishes owner-tested hardware from build-only targets and records the known low-priority multi-client reconnect storm. Its resource measurements are build reports, not heap-soak results.
