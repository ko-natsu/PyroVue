# PyroVue — current project state

**Updated 2026-09-27 from the project owner's device report.** This document records what exists and what was observed; it is not a new implementation plan. Start with [`../README.md`](../README.md) for commands, this page for status and future work, and [`protocol-v2.md`](protocol-v2.md) for the normative wire contract. Source code is authoritative for implementation details. The archived [visual reference](pyrovue-ui-concept-v2.png) and [browser screenshots](visual-validation/) are design/evidence artifacts, not current-state specifications.

## Delivered

Frontend milestones **1–8 are implemented and accepted by the owner on ESP32-S3 with iPad and iPhone clients**. This is a monitoring/logger UI, not kiln-power control.

| Area | Current behavior |
|---|---|
| Telemetry | Protocol-v2 `hello`/transactional history/live samples; run isolation, raw-over-coarse merge, fault/gap handling, reconnect and HTTP fallback; idle readings never enter run history. |
| Interface | Touch-first responsive temperature/readout, full-run graph with inspection/expansion, light/dark themes, timing, C/F, Start/Stop reconciliation, stale/fault status. |
| Metrics | Regression-based rate of rise and graph regions; Orton 2016 self-supporting cone estimate using the final 100°C rise when available and a qualified recent-rise fallback otherwise. The estimate is **not** a witness-cone substitute. |
| Markers | One-tap run-local timestamps, graph display, editable notes and deletion. Stored in the browser's `localStorage`, not on the device or synchronized between clients. |
| Visual system | Offline-bundled Oxanium/OFL, active fresh-sample heat stages, sparse dither, reduced-motion behavior, and neutral treatment for idle/stale/disconnected/faulted readings. |
| Firmware | ESP32-S3 TFT and ESP8266 D1 mini build targets; device serves one gzipped HTML bundle from flash. Backend retains bounded raw/coarse history and persists coarse records. |

Editable UI: `web/src/`; tests: `web/test/`; bundler: `web/make_ui_header.py`; generated preview: ignored `web/dist/index.html`; generated firmware header: `include/ui_index.h` (**never edit it manually**). Firmware and device services live under `src/`, `lib/`, and `include/` inside the `PyroVue/` project. The unrelated repository-root `lib/SensorService/` is not part of its PlatformIO build.

## N16R8 migration execution

Approved cross-session specification: [`n16r8-migration-plan.md`](n16r8-migration-plan.md).

- **M0 complete:** shared archive/domain contracts, portable PVR1/PVE1 codecs with shared CRC, strict browser validators, and native/Node behavior fixtures. [M0 handoff](milestones/n16r8-m0.md) records interfaces, commands and cross-language runtime evidence.
- **M1 SOURCE handoff complete; physical gate blocked:** headless `esp32s3_devkitc_n16r8` builds with the 16 MB / 8 MB profile, PSRAM-safe MAX31855 pins and dedicated no-auto-format LittleFS partition. Firmware and empty FS image build/inspection passed. Only motherboard COM1 was visible; the owner confirmed hardware is unavailable this session. No upload/uploadfs occurred, and actual flash/PSRAM, thermocouple and reboot persistence are **not verified**. [M1 handoff](milestones/n16r8-m1.md) contains the exact physical resume procedure.
- **Current execution checks:** 12/12 native cases under Ubuntu-24.04 WSL; 104/104 Node cases; typecheck/lint/build/bundle-current checks passed. A compiled C++ -> independent Python CRC/float/UTF-8 decoder -> JS validator smoke preserved exact readings, fault/gap flags and Unicode notes; scaffolds were removed.
- **All retained firmware targets build:** N16R8 static RAM 51,596 / 327,680 B and flash 901,693 / 3,145,728 B; Feather RAM 91,856 / 327,680 B and flash 940,037 / 1,441,792 B; ESP8266 RAM 54,828 / 81,920 B and flash 428,209 / 1,044,464 B. These are compile/link reports, not physical memory/free-heap measurements.

The Feather remains the default. Wire protocol stays v2 until M3. N16R8 currently runs RAM-only monitor sessions at 1 Hz, not durable archive logging, shared notes, or run export; those are M2–M6. Portable declarations for later milestones have no fake logger/API implementation. This update does not replace the owner's earlier physical acceptance or resolve the known reconnect storm.


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
5. Persistence health is not exposed over protocol v2; long-duration serial heap/soak measurements were not supplied. Native testing was previously unavailable on the Windows PATH without `gcc`/`g++`; M0 now has executed WSL native evidence, not physical storage evidence.

## Design and change guardrails

Keep measured run/sensor/history state on the device and presentation/derived metrics in the browser. Preserve separate connected/disconnected, ACTIVE/IDLE, fresh/stale and valid/faulted meanings; a socket alone does not make temperature valid. The graph must show faults and gaps without inventing values, and physical witness cones remain the authority for heatwork. Prefer readable temperature and graph on iPad landscape and iPhone, large touch targets, visible keyboard focus, restrained technical styling, and no hover-only interactions. Do not imply burner/relay/PID control, add decorative controls without function, or introduce CDN/runtime network dependencies.

Future UI changes should retain the offline single-asset workflow: edit `web/src`, run the frontend checks, rebuild (`npm run build && npm run check:bundle`), then build both firmware targets and exercise the changed path in a browser. Device-specific changes need physical verification on the target actually changed. The concept image is inspiration, not a requirement to copy or a source of behavior claims.
