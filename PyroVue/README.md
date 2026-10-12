# PyroVue

Kiln telemetry firmware and an offline browser monitor. The device owns thermocouple sampling, run identity, bounded history and WebSocket delivery; the browser owns graphing, derived metrics, markers and presentation. For **current completion, owner-reported device validation, the multi-client reconnect-storm caveat and future work**, read [`docs/PROJECT_STATUS.md`](docs/PROJECT_STATUS.md). [`docs/protocol-v2.md`](docs/protocol-v2.md) is the wire contract.

## Targets

- `adafruit_feather_esp32s3_tft`: ESP32-S3 with the existing ST7789 display.
- `esp8266_d1mini`: D1 mini-class ESP8266, TFT-free, with MAX31855 pins `D2` (CS), `D7` (MOSI), `D6` (MISO), and `D5` (SCK).
- `esp32s3_devkitc_n16r8`: headless ESP32-S3 DevKitC-1, 16 MB flash / 8 MB octal PSRAM. Source/build bring-up support is available; physical verification is pending. The Feather remains the default.
- `native`: portable core/codec tests; requires host `gcc`/`g++`. This Windows host runs the native gate in Ubuntu-24.04 through WSL.

The ESP32 and ESP8266 targets use maintained ESP32Async networking libraries. SSE is removed; WebSocket `/ws` is the only live telemetry transport. HTTP remains available at `/`, `/api/now`, `/api/info`, and `POST /api/cmd`.

## History configuration
The retained Feather/ESP8266 monitors sample at 2 Hz. Feather retains 10 minutes of raw samples; ESP8266 uses 5 minutes to protect its smaller heap. Coarse history aggregates finalized 30-second buckets with average, minimum, maximum, and ORed fault flags. These targets retain their existing coarse LittleFS adapter; the raw tier remains RAM-only and bounded.

The N16R8 profile acquires at 1 Hz, keeps 300 raw readings and 60 five-second graph buckets in RAM, and mounts its dedicated LittleFS partition without auto-formatting. At M1 it supports **monitor sessions only**, not durable run recording, device notes, or export. It does not use the old replacing single-file coarse writer. M0 supplies executable archive codecs and validators; M2–M6 implement the recorder/API/browser/export cutover described in the [migration specification](docs/n16r8-migration-plan.md). See [M0 handoff](docs/milestones/n16r8-m0.md) and [M1 handoff](docs/milestones/n16r8-m1.md) for actual evidence and remaining gates.

## N16R8 bring-up and provisioning

Wire the MAX31855 to **3.3 V**, common GND, GPIO12 SCK, GPIO13 DO/MISO, and GPIO10 CS; no MOSI wire. Verify the actual board revision/header labels before connecting. GPIO35–37 are occupied by octal PSRAM; GPIO19/20 remain available for native USB.

The pinned N16R8 environment uses QIO flash at 80 MHz with OPI PSRAM, two reserved 3 MiB application partitions, and a 9.875 MiB `spiffs`-labelled LittleFS data partition. No OTA updater is added. Startup reports actual flash/PSRAM and filesystem state; missing memory or mount failure rejects Start while live monitoring remains available.

Build firmware and the empty filesystem image without touching a device:

```bash
pio run -e esp32s3_devkitc_n16r8
pio run -e esp32s3_devkitc_n16r8 -t buildfs
pio device list
```

Only after identifying the correct native-USB port and confirming the new log partition is empty, obtain explicit approval before the **destructive** first `uploadfs`. The `data/` image contains only `.gitkeep`, not run files. Use `pio run -e esp32s3_devkitc_n16r8 -t uploadfs --upload-port <verified-port>` exactly once for initial provisioning, then `-t upload --upload-port <verified-port>` for firmware. Subsequent updates must **never** upload the filesystem image over saved runs. Normal boot never formats storage. Physical flash/PSRAM, thermocouple, and reboot write/read persistence checks are still required before M1 is complete.

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

Build the desired firmware target after regenerating the embedded UI:

```bash
pio run -e adafruit_feather_esp32s3_tft
pio run -e esp8266_d1mini
pio run -e esp32s3_devkitc_n16r8
```

The [current-state record](docs/PROJECT_STATUS.md) distinguishes owner-tested hardware from build-only targets and records the known low-priority multi-client reconnect storm. Its resource measurements are build reports, not heap-soak results.
