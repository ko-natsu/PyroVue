# PyroVue telemetry and archive protocol v3

All firmware targets and the embedded browser use protocol **3**, firmware `run-logging-1`. There is no older-wire parser or command alias. Archive schema 1 and PVR1/PVE1 file formats are independent: an existing run may truthfully retain `protocolVersion:2` in its immutable originating metadata.

## Transport and discovery

- `/ws`: hello, transactional RAM history, live samples and state notifications. Commands are HTTP-only; unsolicited WebSocket text receives `{type:"error",protocol:3,code:"HTTP_COMMAND_REQUIRED",message}`.
- `GET /api/info`: authoritative current device hello/profile. Discovery requires no device header.
- `GET /api/now`: latest physical sample, or state before the first reading.
- `POST /api/cmd` and every `/api/runs*` request require `X-PyroVue-Device-Id` equal to the current chip's 12-character lowercase hex `deviceId`. Mismatch is 409 `DEVICE_ID_MISMATCH`, with no mutation. This prevents cross-device mistakes; it is not authentication.
- HTTP responses use `Cache-Control: no-store`. Numeric run IDs are positive uint32; command Start's `afterRunId` may be zero.

Hello/info retains `type`, `protocol`, `firmware`, `sampleHz`, `runId`, `runActive`, `runStartMs`, `recentWindowMs`, `coarseResolutionMs`, `snapshotThroughSeq`, `preset` and `clients`. It adds:

| Field | Meaning |
|---|---|
| `deviceId`, `bootId` | Stable chip identity; random 16-lowercase-hex identity for this boot |
| `capabilities.runArchive` | True on N16R8 even if its storage is unavailable; false on monitor targets |
| `acquireIntervalMs`, `logIntervalMs`, `maxRunMs` | N16R8: 1000, 2000, 86400000; monitors report their acquisition interval and zero archive intervals/limit |
| `logging` | `state` (`idle`, `recording`, `error`, `unavailable`), `durableLogSeq`, `latestAcqSeq`, `bufferedRecords`, `acquisitionGaps`, `droppedAcquired`, `queueHighWater`, nullable `errorCode` |
| `storage` | `mounted`, `totalBytes`, `usedBytes`, `freeBytes`, `reservedRemainingBytes`, `startRequiredBytes` |

N16R8 Start requires `startRequiredBytes:2416640`. Monitor profiles use zero storage/counters, mounted false, logging unavailable with `ARCHIVE_UNAVAILABLE`; their archive routes return 404. Feather's separate CSV logger is not the multi-run archive.

N16R8 also reports `hardware:{flashBytes,psramBytes,psramHeapBytes,psramLargestBlock,filesystemLabel}`. `psramBytes` is detected physical capacity; usable allocator `psramHeapBytes` is slightly smaller. Serial reconnect or `i` prints hardware and Start readiness without delaying headless startup.

## Telemetry and bounded RAM replay

A connection receives `hello`, `history.begin`, coarse/raw `history` chunks, then `history.end`. Live samples remain unsuppressed and may interleave with this transaction. Clients buffer and merge them while a snapshot is open. Replay captures run ID, raw upper sequence and coarse upper time once; each loop copies at most 12 raw or 8 coarse points under a short RAM guard, then releases the guard before serialization/network sends. Overwritten prefixes are skipped, not fabricated. Disconnect cancels replay.

```json
{"type":"history.begin","protocol":3,"snapshotId":41,"runId":17,"throughSeq":120}
{"type":"history","protocol":3,"snapshotId":41,"runId":17,"kind":"raw","resolutionMs":1000,"chunk":0,"final":false,"samples":[{"seq":1,"ms":133,"temp":23.4,"fault":0}]}
{"type":"history.end","protocol":3,"snapshotId":41,"runId":17,"throughSeq":120}
{"type":"sample","protocol":3,"runId":17,"seq":121,"ms":120133,"temp":24.1,"fault":0}
```

Raw points merge by `(runId,seq)` and take precedence over coarse points, which merge by `(runId,ms,resolutionMs)`. Coarse entries contain `ms`, `avg`, `min`, `max`, `fault`. Replay is bounded graph history, not a substitute for archive samples.

Idle readings use exactly `(runId,seq,ms)=(0,0,0)`, updating only the physical readout. `runId` in hello/state is always the latest allocated device ID, even inactive; `runActive` is authoritative. Reboot never resumes a run. Failed preparation may consume an ID, and deletion never rolls it back.

`fault` bits: 1 open thermocouple, 2 short to ground, 4 short to VCC, 8 non-finite. Non-finite temperatures serialize as null. Missing sensor hardware is not itself a reliable open-probe reading.

## Commands

JSON bodies are UTF-8, at most **256 bytes**, and may span body callbacks. `requestId` is 1–80 ASCII `[A-Za-z0-9_-]`; `bootId` is the discovered current boot identity.

```json
{"type":"run.start","requestId":"start-123","bootId":"0123456789abcdef","afterRunId":1,"wallStartUtcMs":1792912000000}
{"type":"run.stop","requestId":"stop-123","bootId":"0123456789abcdef","runId":2}
{"type":"preset","requestId":"preset-123","bootId":"0123456789abcdef","value":"slow"}
```

`wallStartUtcMs` is optional client-estimated wall time, never device clock truth. Preset values are nonempty UTF-8 of at most 32 bytes.

Success is HTTP 200:

```json
{"type":"command.result","protocol":3,"requestId":"start-123","ok":true,"runId":2,"runActive":true,"targetRunId":2,"durable":true}
```

`runId/runActive` describe the **current** device. `targetRunId` identifies a retry's historical target (zero for presets). N16R8 Start/Stop acknowledge durable work only after persistence; monitor operations and presets report `durable:false`.

Start first resolves an existing `startRequestId` without reactivating it. A new Start requires current `bootId` and `afterRunId == latest allocated runId`, otherwise 409 `STALE_COMMAND`. Stop of a known ended target is harmless success; stopping an active run requires current boot and exact target. An old Stop cannot stop a newer run. Preset requires current boot. The browser captures device/header/boot/run context once, generates one operation ID, and never automatically retries an ambiguous command; reconcile `/api/info` instead.

After processing a command, including one with failed effects, the device broadcasts actual current `{type:"state",protocol:3,runId,runActive}`. Controls reconcile against authoritative state, not transport acceptance.

## Archive reads

All archive payloads carry `schemaVersion:1`. These routes exist on N16R8 only.

| Request | Response |
|---|---|
| `GET /api/runs?beforeRunId=N&limit=16` | Newest-first `{schemaVersion:1,runs:[meta],nextBeforeRunId:null\|number}`; cursor optional, limit 1–16 |
| `GET /api/runs/N` | Run metadata, or 404 |
| `GET /api/runs/N/snapshot` | `{schemaVersion:1,meta,throughLogSeq,throughEventRevision}`; syncs active pending records, never ends the run |
| `GET /api/runs/N/samples?afterLogSeq=A&throughLogSeq=T&limit=64` | `{schemaVersion:1,deviceId,runBootId,runId,afterLogSeq,throughLogSeq,samples,nextAfterLogSeq,done}` |
| `GET /api/runs/N/events?afterRevision=A&throughRevision=T&limit=8` | `{schemaVersion:1,deviceId,runBootId,runId,afterRevision,throughRevision,events,nextAfterRevision,done}` |

Cursors are exclusive below and inclusive above. Both after/through parameters are required on pages; sample limit is 1–64, event limit 1–8. `after == through` returns empty/done. `after > through` or a future watermark is 416 `INVALID_CURSOR`. Bind every page to the captured device ID, originating run boot ID, run ID and requested watermark. A later annotation cannot alter an earlier event watermark.

Samples contain `{logSeq,acqSeq,ms,temp,fault,intervalFault,flags}`. `logSeq` is consecutive recorded identity; `acqSeq` retains original acquisition identity. `intervalFault` ORs intervening faults. Flag 1 is terminal, flag 2 is acquisition gap. Selected samples are unfiltered actual readings, not graph aggregates. Finite float32 values use at least nine significant decimal digits, including `-0`; the browser normalizes with `Math.fround` and rejects overflow. Null represents non-finite temperature.

Healthy metadata fields: `schemaVersion,deviceId,bootId,runId,firmware,protocolVersion,status,rawComplete,acquireIntervalMs,logIntervalMs,graphBucketMs,maxRunMs,startUptimeMs,endElapsedMs,wallStartUtcMs,wallTimeSource,sampleCount,durableLogSeq,latestAcqSeq,eventRevision,annotationCount,acquisitionGaps,droppedAcquired,sizeBytes,recoveredTailBytes,errorCode`. Optional times and error are null, not zero sentinels. Status is `active`, `complete`, `interrupted`, `duration_limit` or `storage_error`. Origin boot/firmware/protocol never change on later firmware uploads. Durable counts describe synced records; live health may be newer.

Corrupt metadata is **only** `{schemaVersion,deviceId,runId,status:"corrupt",path,sizeBytes,recoveredTailBytes,rawComplete:false,errorCode:"CORRUPT_RUN"}`. It remains listable/deletable; snapshot, pages and notes reject with 409 `CORRUPT_RUN`.

## Device annotations and deletion

| Request | Body |
|---|---|
| `POST /api/runs/N/markers` | `{operationId,id,baseRevision:0,ms:null\|uint,note,timeBasis}` |
| `PUT /api/runs/N/markers/ID` | Same fields, current marker `baseRevision`; body ID matches path |
| `DELETE /api/runs/N/markers/ID` | `{operationId,baseRevision}` |

Bodies are at most **4096 UTF-8 bytes** and support multichunk input. IDs follow the request-ID ASCII rule. Notes are trimmed as in the editor, allow blank/comma/quote/newline/Unicode text, and are limited to 500 Unicode scalars/2000 UTF-8 bytes. NUL and invalid Unicode/UTF-8 are rejected. `timeBasis` is `estimated`, `manual` or `untimed`; untimed requires null `ms`, others require an elapsed value within the known run boundary. Observation time is never silently replaced with save time. Post-fire untimed notes are allowed.

Success after journal sync:

```json
{"schemaVersion":1,"runId":4,"operationId":"note-create-123","id":"note-123","revision":2,"persisted":true,"deleted":false}
```

An identical operation-ID/domain retry returns its original revision/result, including after later edits/deletion. Changed payload is 409 `OPERATION_ID_CONFLICT`; stale marker revision is 409 `REVISION_CONFLICT`. Deletes append tombstones. Journal recovery validates and syncs the actual prefix before replaying an unacknowledged result or appending again; invalid event suffixes are preserved before repair. Raw tails are never truncated to hide loss.

`DELETE /api/runs/N` rejects an active run with 409 `RUN_ACTIVE`. Others use `.deleting-N` staging before unlink. Response is `{schemaVersion:1,runId,deleted:true}` or `deleted:false` if absent. No ID rollback or automatic deletion of previous runs. A reader encountering a deleted run fails rather than completing a partial export.

The existing browser marker UI remains browser-local until M5; these device-owned annotation endpoints do not silently migrate localStorage notes.

## Errors and execution

Errors are `{type:"error",protocol:3,code,message,requestId?}` with the actual final HTTP status:

| Status | Codes |
|---|---|
| 400 | `INVALID_REQUEST` |
| 404 | `RUN_NOT_FOUND`, `MARKER_NOT_FOUND`, `ARCHIVE_UNAVAILABLE` |
| 409 | `RUN_ACTIVE`, `RUN_ID_CONFLICT`, `REVISION_CONFLICT`, `OPERATION_ID_CONFLICT`, `EVENT_TIME_OUT_OF_RANGE`, `CORRUPT_RUN`, `STALE_COMMAND`, `DEVICE_ID_MISMATCH`, `RUN_ID_EXHAUSTED` |
| 413 | `BODY_TOO_LARGE` |
| 416 | `INVALID_CURSOR` |
| 500 | `RUN_ID_SAVE_FAILED` |
| 503 | `BUSY`, `STORAGE_UNAVAILABLE`, `WRITE_FAILED` |
| 507 | `INSUFFICIENT_STORAGE`, `EVENT_BUDGET_EXHAUSTED` |

Network callbacks validate bounded bodies and enqueue owned typed jobs; no filesystem/run mutation occurs in AsyncTCP. The owner loop advances one job step between acquisition/sync opportunities. N16R8 has four 32-KiB PSRAM response leases/eight jobs; monitors have one 1-KiB command lease/job. Partial-body disconnects release assembling slots. Accepted mutations finish after disconnect; abandoned GETs are dropped. A response contains only completed RAM and releases its generation-stamped lease on destruction. There is no guessed success or filesystem-backed pending fill.

## Retained Feather single-run CSV extension

Feather alone retains `GET /api/csv`, `GET|DELETE /api/run.csv?runId=N` and explicit `POST /api/csv/format` with `{"confirm":"ERASE_FFAT"}` (at most 64 bytes). Format refuses a saved run/recording/download; erase requires the matching saved ID (zero only for an existing unknown/corrupt-ID file). Temperature download is the validated fixed-length `run-N-temperatures.csv` prefix. These CSV-only jobs remain loop-owned and use a bounded producer/consumer download buffer. They are not `/api/runs` or the N16R8 archive.

The 960-KiB FAT bridge retains its 500-ms acquisition / 3000-ms selection / six-hour limit and does not overwrite its saved run. A saved Feather run blocks a new Start until explicit erase. Browser Notes CSV remains a separate local-only file. See [README](../README.md#feather-single-run-csv-bridge).
