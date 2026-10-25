# PyroVue — current project state

**Updated 2026-10-24: the Feather CSV bridge is closed as an interim software checkpoint; further TFT work is deferred in favor of the N16R8 milestones. Physical frontend acceptance remains the owner's 2026-09-27 report.** This document records what exists and what was observed; it is not a new implementation plan. Start with [`../README.md`](../README.md) for commands, this page for status and future work, and [`protocol-v2.md`](protocol-v2.md) for the normative wire contract. Source code is authoritative for implementation details. The archived [visual reference](pyrovue-ui-concept-v2.png) and [browser screenshots](visual-validation/) are design/evidence artifacts, not current-state specifications.

## Delivered

Frontend milestones **1–8 are implemented and accepted by the owner on ESP32-S3 with iPad and iPhone clients**. This is a monitoring/logger UI, not kiln-power control.

| Area | Current behavior |
|---|---|
| Telemetry | Protocol-v2 `hello`/transactional history/live samples; run isolation, raw-over-coarse merge, fault/gap handling, reconnect and HTTP fallback; idle readings never enter run history. |
| Interface | Touch-first responsive temperature/readout, full-run graph with inspection/expansion, light/dark themes, timing, C/F, Start/Stop reconciliation, stale/fault status. |
| Metrics | Regression-based rate of rise and graph regions; Orton 2016 self-supporting cone estimate using the final 100°C rise when available and a qualified recent-rise fallback otherwise. The estimate is **not** a witness-cone substitute. |
| Markers | One-tap run-local timestamps, graph display, editable notes/deletion and separate timestamped notes CSV export. Stored in the browser's `localStorage`, not on the device or synchronized between clients. |
| Visual system | Offline-bundled Oxanium/OFL, active fresh-sample heat stages, sparse dither, reduced-motion behavior, and neutral treatment for idle/stale/disconnected/faulted readings. |
| Firmware | ESP32-S3 TFT, N16R8 and ESP8266 targets build; device serves one gzipped HTML bundle. Feather has an independent three-second, single-run FAT CSV logger; other targets remain monitors with their existing history configuration. |

Editable UI: `web/src/`; tests: `web/test/`; bundler: `web/make_ui_header.py`; generated preview: ignored `web/dist/index.html`; generated firmware header: `include/ui_index.h` (**never edit it manually**). Firmware and device services live under `src/`, `lib/`, and `include/` inside the `PyroVue/` project. The unrelated repository-root `lib/SensorService/` is not part of its PlatformIO build.

## N16R8 migration execution

Approved cross-session specification: [`n16r8-migration-plan.md`](n16r8-migration-plan.md).

- **M0 complete:** shared archive/domain contracts, portable PVR1/PVE1 codecs with shared CRC, strict browser validators, and native/Node behavior fixtures. [M0 handoff](milestones/n16r8-m0.md) records interfaces, commands and cross-language runtime evidence.
- **M1 SOURCE handoff complete; physical gate blocked:** headless `esp32s3_devkitc_n16r8` builds with the 16 MB / 8 MB profile, PSRAM-safe MAX31855 pins and dedicated no-auto-format LittleFS partition. Firmware and empty FS image build/inspection passed. Only motherboard COM1 was visible; the owner confirmed hardware is unavailable this session. No upload/uploadfs occurred, and actual flash/PSRAM, thermocouple and reboot persistence are **not verified**. [M1 handoff](milestones/n16r8-m1.md) contains the exact physical resume procedure.
- **Recorded M0/M1 checks:** 12/12 native and 104/104 Node cases; cross-language codec/validator smoke and all three target builds passed at that handoff. Those reports are historical snapshots; current bridge checks/measurements are below.

The Feather remains the default and wire protocol stays v2 until M3. N16R8 still runs RAM-only monitor sessions at 1 Hz, not durable archive logging, shared notes, or run export; those remain M2–M6. The shared RunManager prepare/activate lifecycle is now implemented for the Feather bridge, but that does not complete the N16R8 recorder/API milestones. The earlier M0/M1 handoffs and approved archive contracts remain the migration baseline.

## Feather six-hour CSV bridge

Previous M0/M1 work was committed and pushed first as `e339785` (`Complete N16R8 M0 contracts and M1 source bring-up`). The interim patch adds real device storage and separate browser notes export without changing Feather hardware, partition layout, live 2 Hz acquisition, bounded graph history or wire version.

**Closed for further feature work at the owner's request on 2026-10-24.** Preserve the implementation and its deployment instructions; physical Feather acceptance is not implied by this closeout. Resume the approved N16R8 milestones, starting with M2's portable recorder and journal reducer. M1 hardware verification, M2 storage/lifecycle integration and subsequent API/UI milestones remain open.

- **Recorder:** `lib/SingleRunCsv/` owns one `/ffat/pyrovue-run.csv`, mounted on the existing 960 KiB `ffat` partition without auto-formatting. First/three-second/terminal selected readings retain actual elapsed timestamps and float32 values; interval faults and acquisition gaps remain visible. Every header/row/footer write and fsync is checked. Start requires 512 KiB free and refuses an existing saved file; explicit erase requires its ID and refuses recording/downloads. Six-hour limit is reported as `duration_limit`, not kiln-power control.
- **Lifecycle/network:** NVS identity preparation -> checked CSV header sync -> activation, with failed preparations consuming rather than reusing IDs. Boot recovers a saved-run identity floor and never resumes a firing. `LoopRequestQueue` owns bounded command/read jobs; file work is in the loop, and the download response copies a 1 KiB producer/consumer buffer rather than reading flash in AsyncTCP. A nonblocking monitor gate serializes run/preset commands and state HTTP reads with legacy callback replay; Start/Stop HTTP errors are honest and final state is published even on storage failure.
- **Browser:** device storage/recording/error status, temperature download, explicitly confirmed saved-run erase and full-partition provisioning. Separate notes CSV uses the final run-local marker list, original estimated elapsed timestamps, UTF-8 and correctly quoted multiline notes; it works offline and is never described as device-owned. Temperature and browser-note run IDs are labelled independently. See the [deployment/import instructions](../README.md#feather-single-run-csv-bridge).
- **Current automated proof:** 41/41 native cases (15 core + 26 CSV) under Ubuntu-24.04 WSL; 120/120 Node cases; typecheck, lint, bundle and bundle-current checks passed. HTML is 245,939 B; gzip is 75,880 B. All three firmware builds passed (table below); existing library deprecation/extra-UF2 warnings remain.
- **Actual host-disk smoke:** compiled the real SensorService/RunManager/CSV core with gnu++11 and checked POSIX writes/fsync. Accelerated 5:59:59 run: 43,198 acquisitions, 7,201 recorded rows, **252,368 B**, complete; fresh-instance recovery and bounded fragmented download returned identical bytes. Six-hour cutoff: 7,201 rows, 252,375 B, `duration_limit`. A deliberately incomplete disk tail recovered/downloaded 252,316 validated bytes from a 252,330 B file without deleting the tail. Independent Python CSV/float32 decoding preserved exact finite values, negative zero, blank faulted temperatures, interval faults and terminal flags. A concurrent real MonitorStateGate/RunManager smoke exercised 2,000 mutations and coherent active snapshots.
- **Actual browser surface, controlled HTTP responses:** production bundle exercised unavailable storage/explicit format confirmation, Start/Stop, marker capture/edit, offline notes download, full temperature download using the host recorder's actual CSV, explicit erase, corrupt-ID recovery controls, unsupported-target hiding and failed Stop reconciliation. The downloaded temperature file matched the host CSV byte-for-byte (SHA-256 `dfa55b9f8213ceeb539c5bba038cadb4d58eb22240fc68e3f90d370a70475eeb`). Notes retained captured time, quotes, Unicode and a multiline note. 1024×768, 390×844 and 320×568 layouts had no horizontal overflow; portrait data-copy height was corrected from 260 px to about 39–57 px. These are host/browser proofs, not device soak evidence.
- **Closeout recheck (2026-10-24):** 41/41 native and 120/120 Node cases passed; typecheck, lint and bundle-current check passed. A fresh POSIX-disk smoke recorded three selected rows, retained negative zero, an intervening fault and terminal identity, recovered after constructing a new recorder, and downloaded the same 314 bytes in 17-byte chunks. The saved run refused overwrite. No additional physical validation is claimed.

| Current build | Static RAM / limit (B) | Flash / application limit (B) |
|---|---:|---:|
| Feather TFT | 93,332 / 327,680 | 988,401 / 1,441,792 |
| ESP8266 D1 mini | 56,232 / 81,920 | 442,069 / 1,044,464 |
| N16R8 | 52,812 / 327,680 | 915,801 / 3,145,728 |

The freshly built Feather partition table decoded to the unchanged `nvs`, `otadata`, two 1,408 KiB application slots, 256 KiB UF2 and 960 KiB `ffat` regions. **Physical deployment is not verified:** device inventory exposed only motherboard COM1; no ESP USB port, upload, uploadfs, device format or physical storage test occurred. Any future Feather deployment still needs the firmware-only upload and short disconnected-client/Stop/download/reboot preflight in the README. Checked fsync is not a guaranteed power-cut recovery bound; FAT corruption may prevent mounting. The multi-client reconnect storm is unchanged.



## Validation and evidence

- **Physical, owner-reported:** ESP32-S3 deployment tested with iPad and iPhone clients. The owner judged implementation and validation of the frontend milestones satisfactory. Multiple clients were tested: functionality worked, **but a reconnect storm was observed**. No second device validation is being claimed for ESP8266.
- **Browser, previously exercised:** the production bundle was run against controlled protocol-v2 WebSocket/HTTP scenarios: idle/start/stop, history catch-up and overlap, faults, stale/disconnected, markers, graph interactions, C/F and long history. The 1024×768, 1180×820, 390×844 and 320×568 light/dark × idle/active matrix and distinct safety/interaction captures are in [`visual-validation/`](visual-validation/). These images do **not** prove physical hardware behavior.
- **Prior recorded automated checks (before N16R8 migration):** `npm test` (59 passing), `npm run typecheck`, `npm run lint`, `npm run build`, and `npm run check:bundle` passed. The ESP32-S3 and ESP8266 PlatformIO builds passed. These are historical results; current migration execution checks are recorded above.
- **Prior recorded build measurements:** HTML 211,286 B; gzip 67,336 B. ESP32-S3 RAM 91,856 / 327,680 B, flash 939,869 / 1,441,792 B. ESP8266 RAM 54,828 / 81,920 B, flash 428,209 / 1,044,464 B. Build reports are not runtime free-heap data. ESP8266 physical deployment and three-cycle serial heap/history replay measurements were **not reported**; treat them as optional future hardening, not as completed evidence.

## Known limitations and future iteration

1. **Low priority — multi-client reconnect storm (owner observed).** The firmware allows one WebSocket client. With multiple clients, functionality worked but repeated reconnection behavior appeared. Do not describe multi-client operation as storm-free. Reproduce on two clients and inspect connection/close timing before changing retry, client-limit, or fallback behavior; do not suppress logs as a substitute for fixing it. Single-client use remains the owner's primary use case.
2. ESP8266 advertises a 10-minute `recentWindowMs` despite allocating five minutes of raw history. Its firmware built, but no physical ESP8266 validation was reported.
3. Snapshot replay uses a global replay-suppression flag and a three-pass catch-up cap. A multi-client design change must revisit that boundary rather than assuming independent client snapshots already exist.
4. Marker notes live only in the local browser origin/run-ID namespace. Site-data clearing erases them, and clients do not share them. Device-side persistence would need an explicit protocol/API and migration design.
5. Feather CSV persistence health is exposed via the separate `/api/csv` bridge, not a protocol-v3 archive capability. Other targets do not advertise a fake archive. Physical free-heap/flash-latency/long-duration soak and power-cut measurements remain unavailable; native/host storage checks are not physical durability proof.

## Design and change guardrails

Keep measured run/sensor/history state on the device and presentation/derived metrics in the browser. Preserve separate connected/disconnected, ACTIVE/IDLE, fresh/stale and valid/faulted meanings; a socket alone does not make temperature valid. The graph must show faults and gaps without inventing values, and physical witness cones remain the authority for heatwork. Prefer readable temperature and graph on iPad landscape and iPhone, large touch targets, visible keyboard focus, restrained technical styling, and no hover-only interactions. Do not imply burner/relay/PID control, add decorative controls without function, or introduce CDN/runtime network dependencies.

Future UI changes should retain the offline single-asset workflow: edit `web/src`, run the frontend checks, rebuild (`npm run build && npm run check:bundle`), then build all three firmware targets and exercise the changed path in a browser. Device-specific changes need physical verification on the target actually changed. The concept image is inspiration, not a requirement to copy or a source of behavior claims.
