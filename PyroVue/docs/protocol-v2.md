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

`runId` is a monotonically increasing persisted identifier for the current or last run; `runActive` is authoritative. Active samples have a positive `runId` and `seq`; their `ms` is elapsed time from the run origin and is valid only with that `runId`. Idle current readings use exactly `runId: 0`, `seq: 0`, and `ms: 0`. Clients must not infer active state from samples, reconnects, or timestamps.

Commands are JSON sent over WebSocket or `POST /api/cmd`:

```json
{"type":"run.start"}
{"type":"run.stop"}
{"type":"preset","value":"slow"}
```

After every recognized `run.start` or `run.stop`, including a repeated command, the device broadcasts authoritative state after command effects complete:

```json
{"type":"state","protocol":2,"runId":18,"runActive":true}
```

Clients keep session controls pending until `hello` or `state` confirms the requested state. A bounded `GET /api/info` reconciliation may be used if the pushed state is missed. Malformed and unknown commands are ignored; command transport acceptance alone is not confirmation.

## Fault flags

`fault` is a bit field: `1` open thermocouple, `2` short to ground, `4` short to VCC, and `8` non-finite temperature. Multiple flags may be set.
