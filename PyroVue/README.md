# PyroVue backend

Current project status and next-session startup instructions: [`docs/NEXT_SESSION_HANDOFF.md`](docs/NEXT_SESSION_HANDOFF.md).

PyroVue is an Arduino/PlatformIO kiln telemetry firmware. The backend owns thermocouple sampling, explicit run identity, bounded telemetry history, and WebSocket delivery. The browser owns graphing and presentation.

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

`web/make_ui_header.py` inlines local CSS and JavaScript, writes the preview bundle to ignored `web/dist/index.html`, and deterministically regenerates `include/ui_index.h`. The production UI has no CDN, analytics, font, script, or API dependency.

## Resource measurements

Measured with `pio run` after the idle-telemetry and run-state reconciliation bundle was embedded:

| Environment | RAM | Flash |
|---|---:|---:|
| ESP32-S3 TFT | 91,856 / 327,680 (28.0%) | 899,229 / 1,441,792 (62.4%) |
| ESP8266 D1 mini | 54,828 / 81,920 (66.9%) | 387,553 / 1,044,464 (37.1%) |

These are static build reports, not minimum-free-heap measurements under a live history replay. Hardware soak and reconnect qualification remain required before deployment.

## Hardware validation handoff

The frontend test harness covers protocol replay, reconnect fallback, overlap precedence, faults, responsive layout, graph interaction, bounded session-command reconciliation, idle current readings, and generated-asset consistency without physical hardware. Before deployment, validate on both device targets:

1. Open the device AP from an iPad in landscape and an iPhone; confirm the embedded page loads fully offline.
2. On a cold-booted idle device, confirm the browser shows the same changing current temperature and fault state as the TFT without a reload.
3. Start a session and confirm the button, run ID, elapsed time, temperature, and graph all advance without a reload.
4. Stop the session and confirm the button returns to Start while live idle temperature continues and the completed run graph remains intact.
5. Reconnect during an active run and confirm coarse history, raw overlap, pre-end catch-up samples, elapsed time, and rate regions remain continuous.
6. Exercise open-thermocouple, short-to-ground, short-to-VCC, non-finite, and combined fault bits; confirm no invalid value is displayed as zero or connected through the graph.
7. Replay a full raw window and a long coarse history on each target; watch browser responsiveness and device free heap.
8. Open a second tab to confirm the configured one-client limit degrades honestly to polling and recovers without a reconnect storm.
9. Verify touch inspection, graph expansion, light/dark themes, and Start/Stop controls while operating at normal kiln-side viewing distance.

The idle-reading and run-state changes are an explicit protocol-v2 behavior correction documented in `docs/protocol-v2.md`; they do not change the backend architecture.
