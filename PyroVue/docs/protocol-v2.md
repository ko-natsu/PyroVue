# PyroVue telemetry protocol v2

The device exposes state and telemetry over WebSocket at `/ws`. JSON is used deliberately; the wire contract is independent of the browser layout.

## Message ordering

A WebSocket connection receives `hello`, then `history.begin`, zero or more coarse and raw `history` chunks, `history.end`, and finally live `sample` messages. Every history snapshot has a `snapshotId` and `throughSeq`. A client merges raw samples by `(runId, seq)` and coarse points by `(runId, ms, resolutionMs)`. Raw points take precedence over coarse points for overlapping time.

## Messages

```json
{"type":"hello","protocol":2,"firmware":"rework-2","sampleHz":2,"runId":17,"runActive":true,"runStartMs":328400,"recentWindowMs":600000,"coarseResolutionMs":30000,"snapshotId":41,"snapshotThroughSeq":8821}
```

```json
{"type":"sample","protocol":2,"runId":17,"seq":8822,"ms":194720,"temp":843.7,"fault":0}
```

History metadata is repeated in every chunk so a chunk can be validated independently:

```json
{"type":"history","protocol":2,"snapshotId":41,"runId":17,"kind":"coarse","resolutionMs":30000,"chunk":0,"final":false,"samples":[{"ms":180000,"avg":831.4,"min":827.1,"max":839.8,"fault":0}]}
```

`kind` is `coarse` or `raw`. Raw entries contain `seq`, `ms`, `temp`, and `fault`; coarse entries contain `ms`, `avg`, `min`, `max`, and `fault`. `history.begin` and `history.end` carry the snapshot watermark.
`GET /api/now` returns a `sample` with the same fields when a raw sample exists, otherwise a `state` message. `GET /api/info` returns the current hello/state fields plus `clients` and `preset`.

## Run state

`runId` is a monotonically increasing persisted identifier for the current or last run; `runActive` is authoritative. Samples use `runId: 0` and `ms: 0` while no run is active. For active samples, `ms` is elapsed time from the run origin and is valid only with its `runId`. Clients must not infer active state from reconnects or timestamps.

Commands are JSON sent over WebSocket or `POST /api/cmd`:

```json
{"type":"run.start"}
{"type":"run.stop"}
{"type":"preset","value":"slow"}
```

Responses use `{"type":"error","code":"..."}` for malformed or invalid commands.

## Fault flags

`fault` is a bit field: `1` open thermocouple, `2` short to ground, `4` short to VCC, and `8` non-finite temperature. Multiple flags may be set.
