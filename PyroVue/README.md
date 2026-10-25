# PyroVue

Kiln telemetry firmware and an offline browser monitor. The device owns thermocouple sampling, run identity, bounded history, WebSocket delivery and the Feather's single-run temperature CSV; the browser owns graphing, derived metrics, markers and presentation. For **current completion, owner-reported device validation, the multi-client reconnect-storm caveat and future work**, read [`docs/PROJECT_STATUS.md`](docs/PROJECT_STATUS.md). [`docs/protocol-v2.md`](docs/protocol-v2.md) is the wire contract.

## Targets

- `adafruit_feather_esp32s3_tft`: ESP32-S3 with the existing ST7789 display.
- `esp8266_d1mini`: D1 mini-class ESP8266, TFT-free, with MAX31855 pins `D2` (CS), `D7` (MOSI), `D6` (MISO), and `D5` (SCK).
- `esp32s3_devkitc_n16r8`: headless ESP32-S3 DevKitC-1, 16 MB flash / 8 MB octal PSRAM. Source/build bring-up support is available; physical verification is pending. The Feather remains the default.
- `native`: portable core/codec tests; requires host `gcc`/`g++`. This Windows host runs the native gate in Ubuntu-24.04 through WSL.

The ESP32 and ESP8266 targets use maintained ESP32Async networking libraries. WebSocket `/ws` supplies live telemetry; HTTP remains available at `/`, `/api/now`, `/api/info`, and `POST /api/cmd`. The Feather additionally exposes the single-run CSV routes described below; other targets return 404 for them.

## History configuration
The retained Feather/ESP8266 monitors acquire at 2 Hz. Feather retains 10 minutes of raw samples; ESP8266 uses 5 minutes to protect its smaller heap. Coarse RAM history aggregates finalized 30-second buckets with average, minimum, maximum, and ORed fault flags. Feather separately saves selected unfiltered readings every three seconds to its existing FAT partition. ESP8266 retains its coarse LittleFS adapter; neither target persists every acquired 2 Hz reading.

The N16R8 profile acquires at 1 Hz, keeps 300 raw readings and 60 five-second graph buckets in RAM, and mounts its dedicated LittleFS partition without auto-formatting. Current firmware supports **monitor sessions only**, not durable run recording, device notes, or export. It does not use the old replacing single-file coarse writer. M0 supplies executable archive codecs and validators; M2 Phase A adds portable record selection and journal reduction, not device storage/lifecycle integration. M2–M6 implement the recorder/API/browser/export cutover described in the [migration specification](docs/n16r8-migration-plan.md). See [M0](docs/milestones/n16r8-m0.md), [M1](docs/milestones/n16r8-m1.md) and [M2](docs/milestones/n16r8-m2.md) handoffs for evidence and remaining gates.

## Feather single-run CSV bridge

**Closed interim software checkpoint; further TFT work is deferred.** Active development follows the [N16R8 migration milestones](docs/n16r8-migration-plan.md). These instructions remain available for the retained Feather target; closeout does not constitute physical deployment acceptance.

This is the interim patch for one firing of **less than six hours**, including any cooling you want recorded. It does not implement the N16R8 multi-run archive or device-owned notes. Live acquisition/display stays at 2 Hz; the file selects the first acquired reading, then the first reading at least 3,000 ms after the last recorded one, and an unselected latest reading on Stop. Actual timestamps/float32 values are retained; faults between selected readings are ORed into `interval_fault`, and acquisition gaps are flagged rather than interpolated.

### Deploy before the firing

From this project directory:

```bash
pio device list
pio run -e adafruit_feather_esp32s3_tft -t upload --upload-port <verified-USB-port>
```

Use the actual USB board port, not motherboard COM1. **Do not run `uploadfs`, erase flash, or change partitions for this patch.** The existing 4 MiB tinyUF2 layout is retained: the `ffat` region is 960 KiB at `0x310000`; both application slots, UF2 and NVS remain separate. The logger mounts with `FFat.begin(false, "/ffat", 10, "ffat")` and owns only `/ffat/pyrovue-run.csv`.

1. Reload the device UI and check **Device CSV**. Start requires mounted storage, at least **512 KiB free**, and no saved run. Mount/space/identity/write failures reject Start; temperature monitoring stays usable.
2. If storage is unmounted, **Format Storage** offers explicit first-time provisioning. Its confirmation erases **all data in `ffat`**, not just this application's file. Use it only after confirming nothing there needs preserving; unknown contents are not permission to format. Normal boot never formats.
3. Before relying on the patch, record a short bench run, disconnect/reconnect the browser, Stop, and download both files. Reboot and download the temperature CSV again; verify the same rows survived. Export the bench notes before erasing the saved run.
4. Explicitly erase the bench run, then **Start Session** for the firing. Logging is owned by the device loop, not the client connection. Keep device power stable.
5. **Stop Session** after the desired firing/cooling interval, then use **Download Temperatures** and **Download Notes CSV**. Keep the matching `run-<id>-temperatures.csv` / `run-<id>-notes.csv` pair in a per-firing folder. Export notes from the same browser/origin that created them, after final edits.

An existing saved file blocks a new Start; it is never replaced automatically. **Erase Saved Run** requires confirmation and the exact saved run ID, and refuses active recording/downloads. It does not delete browser notes. Download/export notes before beginning a different run; the shell's notes export selects its current run. Run IDs are per device, not globally unique.

### File interpretation and limits

- Temperature CSV has six `#` metadata lines, a column header, data rows and a checked end footer. For PC import, skip lines beginning with `#`. Columns: `run_id,log_seq,acq_seq,elapsed_ms,temp_c,fault,interval_fault,flags`. `log_seq` is consecutive; `acq_seq` is the original 2 Hz identity. Temperatures are Celsius with nine significant digits; non-finite readings have a blank temperature field.
- Fault bits: 1 open thermocouple, 2 short to ground, 4 short to VCC, 8 non-finite. `flags` bit 0 marks a terminal selected reading; bit 1 marks an acquisition gap before that row. Do not interpolate across fault/gap regions.
- Notes CSV is browser-local UTF-8 with BOM and quoted multiline fields: `run_id,marker_id,elapsed_ms,elapsed_hms,time_basis,note`. Match files by run ID and place notes by `elapsed_ms` on the temperature timeline; notes need not land on a three-second sample. Capture time is estimated from live telemetry, preserved through edits, and exported as `time_basis=estimated`. New marker capture still requires a fresh live run reading. Notes can be edited/exported offline, but are not on the ESP32 or shared between clients; clearing browser storage loses them. Import the note column as text if notes begin with spreadsheet formula characters.
- Recording stops at **six hours** with `duration_limit`; monitoring continues. This limit ends logging, not kiln power. The file is one run, not every 2 Hz acquisition. Browser graph history remains bounded and is not an archive loader.
- Header, selected rows and footer use checked writes/fsync. Reboot never resumes firing/logging: readable files without a valid final footer are exposed as **interrupted**, bounded to their structurally validated prefix. Invalid tails remain on disk, not silently discarded. CSV has no checksum, and FAT power-loss corruption can prevent mounting/recovery; do not assume a guaranteed one-row loss bound.

Current proof is build/native/host-disk/browser only; the physical mount, thermocouple cadence, disconnect soak and power/reboot behavior must be checked before relying on a Feather deployment. The [current-state record](docs/PROJECT_STATUS.md) contains the exercised evidence.


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
