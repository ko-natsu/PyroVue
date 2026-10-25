# PyroVue telemetry protocol v2

The device exposes state and telemetry over WebSocket at `/ws`. JSON is used deliberately; the wire contract is independent of the browser layout.

## Message ordering

A WebSocket connection receives `hello`, then `history.begin`, zero or more coarse and raw `history` chunks, zero or more live-shaped catch-up `sample` messages, and `history.end`. Subsequent `sample` messages are live readings. Clients must buffer catch-up samples while a snapshot is open. Every history snapshot has a `snapshotId` and `throughSeq`. A client merges active raw samples by `(runId, seq)` and coarse points by `(runId, ms, resolutionMs)`. Raw points take precedence over coarse points for overlapping time.

## Messages

```json
{"type":"hello","protocol":2,"firmware":"rework-2","sampleHz":2,"runId":17,"runActive":true,"runStartMs":328400,"recentWindowMs":600000,"coarseResolutionMs":30000,"snapshotThroughSeq":8821}
```

```json
{"type":"sample","protocol":2,"runId":17,"seq":8822,"ms":194720,"temp":843.7,"fault":0}
```

While idle, current readings use a reserved non-historical identity:

```json
{"type":"sample","protocol":2,"runId":0,"seq":0,"ms":0,"temp":23.4,"fault":0}
```

An idle sample updates the current readout and sensor status only. It must not be inserted into run history or interpreted as a run transition.

History metadata is repeated in every chunk so a chunk can be validated independently:

```json
{"type":"history","protocol":2,"snapshotId":41,"runId":17,"kind":"coarse","resolutionMs":30000,"chunk":0,"final":false,"samples":[{"ms":180000,"avg":831.4,"min":827.1,"max":839.8,"fault":0}]}
```

`kind` is `coarse` or `raw`. Raw entries contain `seq`, `ms`, `temp`, and `fault`; coarse entries contain `ms`, `avg`, `min`, `max`, and `fault`. `history.begin` and `history.end` carry the snapshot watermark.

`GET /api/now` returns the latest physical `sample`, including the reserved idle sample when no run is active. It returns a `state` message only before the first sensor reading is available. `GET /api/info` returns the current hello/state fields plus `clients` and `preset`.

## Run state

`runId` is the monotonically increasing persisted latest allocated identifier; a failed durable Start can consume an ID while remaining inactive. `runActive` is authoritative. Active samples have a positive `runId` and `seq`; their `ms` is elapsed time from the run origin and is valid only with that `runId`. Idle current readings use exactly `runId: 0`, `seq: 0`, and `ms: 0`. Clients must not infer active state from samples, reconnects, or timestamps.

Commands are JSON sent over WebSocket or `POST /api/cmd`:

```json
{"type":"run.start"}
{"type":"run.stop"}
{"type":"preset","value":"slow"}
```

Commands are copied into bounded loop-owned jobs; NVS/filesystem work never runs in network callbacks. State HTTP reads are also loop-owned and serialize with the legacy replay reader. After processing a command, including a repeated Start/Stop or a storage failure that ended the run, the device broadcasts actual authoritative state:

```json
{"type":"state","protocol":2,"runId":18,"runActive":true}
```

Clients keep session controls pending until `hello` or `state` confirms the requested state. A bounded `GET /api/info` reconciliation may be used if the pushed state is missed. HTTP success is `200 {"ok":true}` after effects complete; malformed/unknown commands return 400, busy queues 503, and real storage/identity failures return non-2xx JSON `{ok:false,code,message}`. WebSocket failures use the protocol-v2 `error` envelope. Transport acceptance alone is not confirmation. A saved Feather CSV blocks a new Start until explicitly erased; failed Start never advertises an active recording before its header is synced.

## Fault flags

`fault` is a bit field: `1` open thermocouple, `2` short to ground, `4` short to VCC, and `8` non-finite temperature. Multiple flags may be set.

## Feather single-run CSV extension

These routes do not change protocol version or implement the N16R8 archive. They exist only on `adafruit_feather_esp32s3_tft` (`PYROVUE_SINGLE_RUN_CSV`); other targets return 404. Responses use `Cache-Control: no-store`.

| Request | Contract |
|---|---|
| `GET /api/csv` | `{supported:true,mounted,hasRun,recording,runId,sampleCount,bytes,freeBytes,state,error,logIntervalMs:3000,maxDurationMs:21600000,startRequiredBytes:524288}`. Counts/bytes/IDs are uint32; booleans are actual state. `runId=0` means no saved identity or a corrupt/unknown-ID saved file; `hasRun` distinguishes them. `bytes` is the exact validated downloadable prefix, not a claimed physical free-space measurement. |
| `GET /api/run.csv?runId=N` | N must be a positive uint32 matching the saved file. Bounded UTF-8 CSV attachment `run-N-temperatures.csv`; active recording/download conflicts return 409, stale ID 409, missing file 404, unavailable/read failure 503. Valid completed, limited, interrupted or failed-write prefixes can be downloaded; invalid tails are never appended to the response. The fixed Content-Length prevents a read failure from masquerading as a complete download. |
| `DELETE /api/run.csv?runId=N` | Explicit erase, matching the saved ID, with no active writer/reader; returns `{ok:true}` only after removal. N=0 is permitted only for the existing unknown/corrupt-ID file. Does not decrement persisted identity or erase browser notes. |
| `POST /api/csv/format` | Explicit body `{"confirm":"ERASE_FFAT"}` (at most 64 bytes). Erases the entire `ffat` partition and remounts; refuses saved runs, recording and downloads (409). Missing/wrong confirmation is 400; format failure 503. Never invoked automatically. |

`state` is `idle`, `recording`, `complete`, `interrupted`, `duration_limit` or `error`. `error` is null or one of `mount_failed`, `read_failed`, `create_failed`, `write_failed`, `sync_failed`, `storage_full`, `remove_failed`, `format_failed`, `corrupt_saved_run`, `invalid_sample`. Body limits retain the v2 single-fragment rule (commands at most 256 bytes); fragmented requests are rejected without mutation.

The existing 960 KiB FAT region stores `/ffat/pyrovue-run.csv`; no repartition or uploadfs is part of this bridge. Temperature rows use `run_id,log_seq,acq_seq,elapsed_ms,temp_c,fault,interval_fault,flags`, with declared 500 ms acquisition / 3,000 ms selected logging and a six-hour limit. `flags` bit 0 is terminal and bit 1 acquisition gap. PC readers skip the `#` metadata/footer lines.

Notes export is a separate browser-only file with `run_id,marker_id,elapsed_ms,elapsed_hms,time_basis,note`. Its original captured elapsed timestamps share the run origin, but are estimated from live telemetry; edits do not retimestamp notes. It is not a device notes API, shared outbox, ZIP export or archived browser graph loader. Deployment and manual file-pairing limits are in the [README](../README.md#feather-single-run-csv-bridge).

