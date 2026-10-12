// @ts-check
// Schema-1 run archive response validators and shared note rules.
// Pure parsing only, no network. Wire contract: docs/n16r8-migration-plan.md
// ("HTTP/wire contract" and "Client module and export contract").
(function registerRunProtocol(root) {
  'use strict';

  const SCHEMA_VERSION = 1;
  const UINT32_MAX = 4294967295;
  const MAX_RUN_MS = 86400000;
  const GRAPH_BUCKET_MS = 5000;
  const MAX_FIRMWARE_CHARS = 63;
  const MAX_PATH_CHARS = 95;
  const MAX_NOTE_SCALARS = 500;
  const MAX_NOTE_BYTES = 2000;
  const MAX_RUN_LIST = 16;
  const MAX_SAMPLE_PAGE = 64;
  const MAX_EVENT_PAGE = 8;

  const RUN_STATUSES = new Set([
    'active',
    'complete',
    'interrupted',
    'duration_limit',
    'storage_error',
    'corrupt',
  ]);
  const WALL_TIME_SOURCES = new Set(['client-estimated', 'unavailable']);
  const TIME_BASES = new Set(['estimated', 'manual', 'untimed']);
  const ERROR_CODES = new Set([
    'INVALID_REQUEST',
    'RUN_NOT_FOUND',
    'MARKER_NOT_FOUND',
    'ARCHIVE_UNAVAILABLE',
    'RUN_ACTIVE',
    'RUN_ID_CONFLICT',
    'REVISION_CONFLICT',
    'OPERATION_ID_CONFLICT',
    'EVENT_TIME_OUT_OF_RANGE',
    'CORRUPT_RUN',
    'STALE_COMMAND',
    'DEVICE_ID_MISMATCH',
    'BODY_TOO_LARGE',
    'INVALID_CURSOR',
    'BUSY',
    'STORAGE_UNAVAILABLE',
    'WRITE_FAILED',
    'INSUFFICIENT_STORAGE',
    'EVENT_BUDGET_EXHAUSTED',
    'RUN_ID_SAVE_FAILED',
    'RUN_ID_EXHAUSTED',
  ]);

  const DEVICE_ID_RE = /^[0-9a-f]{12}$/;
  const BOOT_ID_RE = /^[0-9a-f]{16}$/;
  const ID_RE = /^[A-Za-z0-9_-]{1,80}$/;

  /**
   * @typedef {'active' | 'complete' | 'interrupted' | 'duration_limit' | 'storage_error' | 'corrupt'} RunStatusLiteral
   * @typedef {'active' | 'complete' | 'interrupted' | 'duration_limit' | 'storage_error'} HealthyRunStatusLiteral
   * @typedef {'complete' | 'interrupted' | 'duration_limit' | 'storage_error' | 'corrupt'} EndedRunStatusLiteral
   * @typedef {'client-estimated' | 'unavailable'} WallTimeSourceLiteral
   * @typedef {'estimated' | 'manual' | 'untimed'} TimeBasisLiteral
   */

  /**
   * @typedef {object} RunMetaHealthy
   * @property {number} schemaVersion
   * @property {string} deviceId
   * @property {string} bootId
   * @property {number} runId
   * @property {string} firmware
   * @property {number} protocolVersion
   * @property {HealthyRunStatusLiteral} status
   * @property {boolean} rawComplete
   * @property {number} acquireIntervalMs
   * @property {number} logIntervalMs
   * @property {number} graphBucketMs
   * @property {number} maxRunMs
   * @property {number} startUptimeMs
   * @property {number | null} endElapsedMs
   * @property {number | null} wallStartUtcMs
   * @property {WallTimeSourceLiteral} wallTimeSource
   * @property {number} sampleCount
   * @property {number} durableLogSeq
   * @property {number} latestAcqSeq
   * @property {number} eventRevision
   * @property {number} annotationCount
   * @property {number} acquisitionGaps
   * @property {number} droppedAcquired
   * @property {number} sizeBytes
   * @property {number} recoveredTailBytes
   * @property {string | null} errorCode
   */

  /**
   * Corrupt catalog variant, exactly: schema/device/run/status/path/size/
   * recoveredTailBytes/rawComplete:false/errorCode:'CORRUPT_RUN'.
   * @typedef {object} RunMetaCorrupt
   * @property {number} schemaVersion
   * @property {string} deviceId
   * @property {number} runId
   * @property {'corrupt'} status
   * @property {string} path
   * @property {number} sizeBytes
   * @property {number} recoveredTailBytes
   * @property {false} rawComplete
   * @property {'CORRUPT_RUN'} errorCode
   */

  /** @typedef {RunMetaHealthy | RunMetaCorrupt} RunMetaRecord */

  /**
   * @typedef {object} RunSample
   * @property {number} logSeq
   * @property {number} acqSeq
   * @property {number} ms
   * @property {number | null} temp
   * @property {number} fault
   * @property {number} intervalFault
   * @property {number} flags
   */

  /**
   * @typedef {object} RunStartedEvent
   * @property {number} revision
   * @property {'run.started'} type
   * @property {string} requestId
   * @property {number} ms
   * @property {'active'} status
   * @property {null} errorCode
   * @property {number} droppedAcquired
   */

  /**
   * @typedef {object} RunEndedEvent
   * @property {number} revision
   * @property {'run.ended'} type
   * @property {string} requestId
   * @property {number} ms
   * @property {EndedRunStatusLiteral} status
   * @property {string | null} errorCode
   * @property {number} droppedAcquired
   */

  /**
   * @typedef {object} MarkerEvent
   * @property {number} revision
   * @property {'marker.created' | 'marker.updated'} type
   * @property {string} operationId
   * @property {string} id
   * @property {number} baseRevision
   * @property {number | null} ms
   * @property {string} note
   * @property {TimeBasisLiteral} timeBasis
   */

  /**
   * Tombstones carry no timestamp or note.
   * @typedef {object} MarkerDeletedEvent
   * @property {number} revision
   * @property {'marker.deleted'} type
   * @property {string} operationId
   * @property {string} id
   * @property {number} baseRevision
   */

  /** @typedef {RunStartedEvent | RunEndedEvent | MarkerEvent | MarkerDeletedEvent} RunEvent */

  /**
   * @typedef {object} RunListResult
   * @property {number} schemaVersion
   * @property {RunMetaRecord[]} runs
   * @property {number | null} nextBeforeRunId
   */

  /**
   * @typedef {object} RunSnapshotResult
   * @property {number} schemaVersion
   * @property {RunMetaHealthy} meta
   * @property {number} throughLogSeq
   * @property {number} throughEventRevision
   */

  /**
   * @typedef {object} SamplesPageResult
   * @property {number} schemaVersion
   * @property {string} deviceId
   * @property {string} runBootId
   * @property {number} runId
   * @property {number} afterLogSeq
   * @property {number} throughLogSeq
   * @property {RunSample[]} samples
   * @property {number} nextAfterLogSeq
   * @property {boolean} done
   */

  /**
   * @typedef {object} EventsPageResult
   * @property {number} schemaVersion
   * @property {string} deviceId
   * @property {string} runBootId
   * @property {number} runId
   * @property {number} afterRevision
   * @property {number} throughRevision
   * @property {RunEvent[]} events
   * @property {number} nextAfterRevision
   * @property {boolean} done
   */

  /**
   * @typedef {object} MutationResultRecord
   * @property {number} schemaVersion
   * @property {number} runId
   * @property {string} operationId
   * @property {string} id
   * @property {number} revision
   * @property {boolean} persisted
   * @property {boolean} deleted
   */

  /**
   * @typedef {object} SamplesPageRequest
   * @property {string} deviceId
   * @property {string} runBootId
   * @property {number} runId
   * @property {number} afterLogSeq
   * @property {number} throughLogSeq
   * @property {number} [limit]
   */

  /**
   * @typedef {object} EventsPageRequest
   * @property {string} deviceId
   * @property {string} runBootId
   * @property {number} runId
   * @property {number} afterRevision
   * @property {number} throughRevision
   * @property {number} [limit]
   */

  /**
   * @typedef {object} SnapshotRequest
   * @property {string} [deviceId]
   * @property {number} [runId]
   * @property {string} [bootId]
   */

  /**
   * @typedef {object} MutationResultRequest
   * @property {number} [runId]
   * @property {string} [operationId]
   * @property {string} [id]
   */

  // Fields that must never appear on the corrupt wire variant (rawComplete and
  // recoveredTailBytes are part of both variants and validated per variant).
  const HEALTHY_ONLY_FIELDS = [
    'bootId',
    'firmware',
    'protocolVersion',
    'acquireIntervalMs',
    'logIntervalMs',
    'graphBucketMs',
    'maxRunMs',
    'startUptimeMs',
    'endElapsedMs',
    'wallStartUtcMs',
    'wallTimeSource',
    'sampleCount',
    'durableLogSeq',
    'latestAcqSeq',
    'eventRevision',
    'annotationCount',
    'acquisitionGaps',
    'droppedAcquired',
  ];

  // Reuse protocol.js's ProtocolError when it is already loaded and consistent;
  // otherwise define the same class so this module stays independently loadable.
  const ProtocolError = /** @type {any} */ (
    (root.PyroVue && root.PyroVue.ProtocolError) ||
    class ProtocolError extends Error {
      /** @param {string} message */
      constructor(message) {
        super(message);
        this.name = 'ProtocolError';
      }
    }
  );

  /**
   * @param {unknown} value
   * @param {string} name
   * @returns {Record<string, any>}
   */
  function requireObject(value, name) {
    if (!value || typeof value !== 'object' || Array.isArray(value)) {
      throw new ProtocolError(`${name} must be an object`);
    }
    return /** @type {Record<string, any>} */ (value);
  }

  /**
   * @param {unknown} value
   * @param {string} name
   * @param {number} min
   * @param {number} max
   * @returns {number}
   */
  function requireInt(value, name, min, max) {
    if (typeof value !== 'number' || !Number.isInteger(value) || value < min || value > max) {
      throw new ProtocolError(`${name} must be an integer between ${min} and ${max}`);
    }
    return value;
  }

  /**
   * @param {Record<string, any>} target
   * @param {string} field
   * @param {number} [min]
   * @param {number} [max]
   */
  function requireIntField(target, field, min = 0, max = UINT32_MAX) {
    requireInt(target[field], field, min, max);
  }

  /**
   * @param {Record<string, any>} target
   * @param {string} field
   */
  function requireBooleanField(target, field) {
    if (typeof target[field] !== 'boolean') {
      throw new ProtocolError(`${field} must be a boolean`);
    }
  }

  /** @param {Record<string, any>} target */
  function requireSchemaVersion(target) {
    if (target.schemaVersion !== SCHEMA_VERSION) {
      throw new ProtocolError(`schemaVersion must be ${SCHEMA_VERSION}`);
    }
  }

  /**
   * @param {unknown} value
   * @param {string} name
   * @param {number} min
   * @param {number} max
   * @returns {string}
   */
  function requireText(value, name, min, max) {
    if (typeof value !== 'string' || value.length < min || value.length > max) {
      throw new ProtocolError(`${name} must be a string of ${min}..${max} characters`);
    }
    return value;
  }

  /**
   * Marker and operation IDs: 1..80 ASCII characters from [A-Za-z0-9_-].
   * @param {unknown} value
   * @param {string} name
   * @returns {string}
   */
  function requireIdentifier(value, name) {
    if (typeof value !== 'string' || !ID_RE.test(value)) {
      throw new ProtocolError(`${name} must match [A-Za-z0-9_-] with 1..80 characters`);
    }
    return value;
  }

  /**
   * Stable lowercase 12-hex STA MAC.
   * @param {unknown} value
   * @returns {string}
   */
  function requireDeviceId(value) {
    if (typeof value !== 'string' || !DEVICE_ID_RE.test(value)) {
      throw new ProtocolError('deviceId must be 12 lowercase hex characters');
    }
    return value;
  }

  /**
   * Random lowercase 16-hex boot identity.
   * @param {unknown} value
   * @returns {string}
   */
  function requireBootId(value) {
    if (typeof value !== 'string' || !BOOT_ID_RE.test(value)) {
      throw new ProtocolError('bootId must be 16 lowercase hex characters');
    }
    return value;
  }

  /**
   * @param {unknown} value
   * @returns {RunStatusLiteral}
   */
  function requireRunStatus(value) {
    if (typeof value !== 'string' || !RUN_STATUSES.has(value)) {
      throw new ProtocolError(`status must be one of ${[...RUN_STATUSES].join(', ')}`);
    }
    return /** @type {RunStatusLiteral} */ (value);
  }

  /**
   * @param {unknown} value
   * @returns {WallTimeSourceLiteral}
   */
  function requireWallTimeSource(value) {
    if (typeof value !== 'string' || !WALL_TIME_SOURCES.has(value)) {
      throw new ProtocolError(`wallTimeSource must be one of ${[...WALL_TIME_SOURCES].join(', ')}`);
    }
    return /** @type {WallTimeSourceLiteral} */ (value);
  }

  /**
   * @param {unknown} value
   * @returns {TimeBasisLiteral}
   */
  function requireTimeBasis(value) {
    if (typeof value !== 'string' || !TIME_BASES.has(value)) {
      throw new ProtocolError(`timeBasis must be one of ${[...TIME_BASES].join(', ')}`);
    }
    return /** @type {TimeBasisLiteral} */ (value);
  }

  /**
   * ArchiveError wire names; NONE is serialized as null.
   * @param {unknown} value
   * @returns {string | null}
   */
  function requireErrorCode(value) {
    if (value === null) return null;
    if (typeof value !== 'string' || !ERROR_CODES.has(value)) {
      throw new ProtocolError('errorCode must be null or a known archive error code');
    }
    return value;
  }

  /**
   * Validates stored note text: no NUL, no lone surrogates, at most 500
   * Unicode scalar values and 2000 UTF-8 bytes.
   * @param {string} text
   * @param {string} name
   */
  function requireNoteText(text, name) {
    let scalars = 0;
    let bytes = 0;
    for (let i = 0; i < text.length; i += 1) {
      const code = text.charCodeAt(i);
      if (code === 0) {
        throw new ProtocolError(`${name} must not contain NUL`);
      }
      let point;
      if (code >= 0xd800 && code <= 0xdbff) {
        const next = i + 1 < text.length ? text.charCodeAt(i + 1) : 0;
        if (next < 0xdc00 || next > 0xdfff) {
          throw new ProtocolError(`${name} must not contain lone surrogates`);
        }
        point = 0x10000 + ((code - 0xd800) << 10) + (next - 0xdc00);
        i += 1;
      } else if (code >= 0xdc00 && code <= 0xdfff) {
        throw new ProtocolError(`${name} must not contain lone surrogates`);
      } else {
        point = code;
      }
      scalars += 1;
      bytes += point < 0x80 ? 1 : point < 0x800 ? 2 : point < 0x10000 ? 3 : 4;
      if (scalars > MAX_NOTE_SCALARS) {
        throw new ProtocolError(`${name} must be at most ${MAX_NOTE_SCALARS} Unicode scalar values`);
      }
      if (bytes > MAX_NOTE_BYTES) {
        throw new ProtocolError(`${name} must be at most ${MAX_NOTE_BYTES} UTF-8 bytes`);
      }
    }
  }

  /**
   * Marker notes from user input: ECMAScript String#trim whitespace, then the
   * stored-note rules. Blank notes remain valid markers.
   * @param {unknown} note
   * @returns {string}
   */
  function normalizeNote(note) {
    if (typeof note !== 'string') {
      throw new ProtocolError('note must be a string');
    }
    const trimmed = note.trim();
    requireNoteText(trimmed, 'note');
    return trimmed;
  }

  /**
   * @param {Record<string, any>} meta
   * @returns {RunMetaHealthy}
   */
  function parseHealthyRunMeta(meta) {
    requireSchemaVersion(meta);
    const deviceId = requireDeviceId(meta.deviceId);
    const bootId = requireBootId(meta.bootId);
    const runId = requireInt(meta.runId, 'runId', 1, UINT32_MAX);
    const firmware = requireText(meta.firmware, 'firmware', 1, MAX_FIRMWARE_CHARS);
    // Originating wire provenance is only required to be positive: historical
    // archives legitimately keep protocol 2 while current runs report 3.
    const protocolVersion = requireInt(meta.protocolVersion, 'protocolVersion', 1, UINT32_MAX);
    const status = requireRunStatus(meta.status);
    if (status === 'corrupt') {
      throw new ProtocolError('corrupt run metadata must use the corrupt wire variant');
    }
    requireBooleanField(meta, 'rawComplete');
    requireIntField(meta, 'acquireIntervalMs', 1000, 1000);
    requireIntField(meta, 'logIntervalMs', 2000, 2000);
    if (meta.graphBucketMs !== GRAPH_BUCKET_MS) {
      throw new ProtocolError(`graphBucketMs must be ${GRAPH_BUCKET_MS}`);
    }
    const maxRunMs = requireInt(meta.maxRunMs, 'maxRunMs', MAX_RUN_MS, MAX_RUN_MS);
    requireIntField(meta, 'startUptimeMs', 0, UINT32_MAX);
    const endElapsedMs = meta.endElapsedMs === null
      ? null
      : requireInt(meta.endElapsedMs, 'endElapsedMs', 0, maxRunMs);
    const wallStartUtcMs = meta.wallStartUtcMs === null
      ? null
      : requireInt(meta.wallStartUtcMs, 'wallStartUtcMs', 0, Number.MAX_SAFE_INTEGER);
    const wallTimeSource = requireWallTimeSource(meta.wallTimeSource);
    requireIntField(meta, 'sampleCount', 0, UINT32_MAX);
    requireIntField(meta, 'durableLogSeq', 0, UINT32_MAX);
    requireIntField(meta, 'latestAcqSeq', 0, UINT32_MAX);
    requireIntField(meta, 'eventRevision', 1, UINT32_MAX);
    requireIntField(meta, 'annotationCount', 0, UINT32_MAX);
    requireIntField(meta, 'acquisitionGaps', 0, UINT32_MAX);
    requireIntField(meta, 'droppedAcquired', 0, UINT32_MAX);
    requireIntField(meta, 'sizeBytes', 0, UINT32_MAX);
    requireIntField(meta, 'recoveredTailBytes', 0, UINT32_MAX);
    const errorCode = requireErrorCode(meta.errorCode);

    if (meta.sampleCount !== meta.durableLogSeq) {
      throw new ProtocolError('sampleCount must equal durableLogSeq for consecutive logSeq');
    }
    if (meta.rawComplete
      && (status !== 'complete' || meta.acquisitionGaps !== 0 || meta.droppedAcquired !== 0)) {
      throw new ProtocolError('rawComplete requires a clean complete run without gaps or drops');
    }
    if (status === 'active' && endElapsedMs !== null) {
      throw new ProtocolError('an active run must not report endElapsedMs');
    }
    if (status !== 'active' && status !== 'interrupted' && endElapsedMs === null) {
      throw new ProtocolError('an ended run must report endElapsedMs');
    }
    if ((wallTimeSource === 'client-estimated') !== (wallStartUtcMs !== null)) {
      throw new ProtocolError('wallStartUtcMs presence must match wallTimeSource');
    }
    if ('corrupt' in meta && meta.corrupt !== false) {
      throw new ProtocolError('healthy run metadata must not declare corrupt');
    }

    return {
      schemaVersion: SCHEMA_VERSION,
      deviceId,
      bootId,
      runId,
      firmware,
      protocolVersion,
      status,
      rawComplete: meta.rawComplete,
      acquireIntervalMs: meta.acquireIntervalMs,
      logIntervalMs: meta.logIntervalMs,
      graphBucketMs: GRAPH_BUCKET_MS,
      maxRunMs,
      startUptimeMs: meta.startUptimeMs,
      endElapsedMs,
      wallStartUtcMs,
      wallTimeSource,
      sampleCount: meta.sampleCount,
      durableLogSeq: meta.durableLogSeq,
      latestAcqSeq: meta.latestAcqSeq,
      eventRevision: meta.eventRevision,
      annotationCount: meta.annotationCount,
      acquisitionGaps: meta.acquisitionGaps,
      droppedAcquired: meta.droppedAcquired,
      sizeBytes: meta.sizeBytes,
      recoveredTailBytes: meta.recoveredTailBytes,
      errorCode,
    };
  }

  /**
   * @param {Record<string, any>} meta
   * @returns {RunMetaCorrupt}
   */
  function parseCorruptRunMeta(meta) {
    requireSchemaVersion(meta);
    if (meta.status !== 'corrupt') {
      throw new ProtocolError("corrupt run metadata must declare status 'corrupt'");
    }
    if ('corrupt' in meta && meta.corrupt !== true) {
      throw new ProtocolError('corrupt run metadata discriminator must be true');
    }
    for (const field of HEALTHY_ONLY_FIELDS) {
      if (field in meta) {
        throw new ProtocolError(`corrupt run metadata must not include ${field}`);
      }
    }
    const deviceId = requireDeviceId(meta.deviceId);
    const runId = requireInt(meta.runId, 'runId', 1, UINT32_MAX);
    const path = requireText(meta.path, 'path', 1, MAX_PATH_CHARS);
    const sizeBytes = requireInt(meta.sizeBytes, 'sizeBytes', 0, UINT32_MAX);
    const recoveredTailBytes = requireInt(meta.recoveredTailBytes, 'recoveredTailBytes', 0, UINT32_MAX);
    if (meta.rawComplete !== false) {
      throw new ProtocolError('corrupt run rawComplete must be false');
    }
    if (meta.errorCode !== 'CORRUPT_RUN') {
      throw new ProtocolError('corrupt run errorCode must be CORRUPT_RUN');
    }
    return {
      schemaVersion: SCHEMA_VERSION,
      deviceId,
      runId,
      status: 'corrupt',
      path,
      sizeBytes,
      recoveredTailBytes,
      rawComplete: false,
      errorCode: 'CORRUPT_RUN',
    };
  }

  /**
   * Validates one run catalog entry (healthy or corrupt wire variant).
   * @param {unknown} value
   * @returns {RunMetaRecord}
   */
  function parseRunMeta(value) {
    const meta = requireObject(value, 'run meta');
    const isCorrupt = meta.status === 'corrupt' || meta.corrupt === true;
    return isCorrupt ? parseCorruptRunMeta(meta) : parseHealthyRunMeta(meta);
  }

  /**
   * Newest-first run catalog page.
   * @param {unknown} value
   * @returns {RunListResult}
   */
  function parseRunList(value) {
    const list = requireObject(value, 'run list');
    requireSchemaVersion(list);
    if (!Array.isArray(list.runs)) {
      throw new ProtocolError('runs must be an array');
    }
    const rawRuns = list.runs;
    if (rawRuns.length > MAX_RUN_LIST) {
      throw new ProtocolError(`runs must contain at most ${MAX_RUN_LIST} entries`);
    }
    /** @type {RunMetaRecord[]} */
    const runs = [];
    for (let i = 0; i < rawRuns.length; i += 1) {
      const meta = parseRunMeta(rawRuns[i]);
      if (i > 0 && meta.runId >= runs[i - 1].runId) {
        throw new ProtocolError(`runs[${i}].runId must be smaller than the previous entry`);
      }
      runs.push(meta);
    }
    const rawNext = list.nextBeforeRunId;
    if (rawNext === undefined) {
      throw new ProtocolError('nextBeforeRunId must be null or a run id');
    }
    /** @type {number | null} */
    let nextBeforeRunId = null;
    if (rawNext !== null) {
      const next = requireInt(rawNext, 'nextBeforeRunId', 1, UINT32_MAX);
      if (runs.length === 0 || next !== runs[runs.length - 1].runId) {
        throw new ProtocolError('nextBeforeRunId must equal the oldest listed runId');
      }
      nextBeforeRunId = next;
    }
    return {
      schemaVersion: SCHEMA_VERSION,
      runs,
      nextBeforeRunId,
    };
  }

  /**
   * Immutable snapshot plus export watermark.
   * @param {unknown} value
   * @param {SnapshotRequest} [expect]
   * @returns {RunSnapshotResult}
   */
  function parseSnapshot(value, expect) {
    const snapshot = requireObject(value, 'run snapshot');
    requireSchemaVersion(snapshot);
    const meta = parseRunMeta(snapshot.meta);
    if (meta.status === 'corrupt') {
      throw new ProtocolError('corrupt catalog entries have no readable history snapshot');
    }
    const throughLogSeq = requireInt(snapshot.throughLogSeq, 'throughLogSeq', 0, UINT32_MAX);
    const throughEventRevision = requireInt(
      snapshot.throughEventRevision, 'throughEventRevision', 0, UINT32_MAX,
    );
    if (throughLogSeq !== meta.durableLogSeq || throughLogSeq !== meta.sampleCount) {
      throw new ProtocolError('throughLogSeq must equal the meta sample count and durable watermark');
    }
    if (throughEventRevision !== meta.eventRevision) {
      throw new ProtocolError('throughEventRevision must equal the meta event revision');
    }
    if (expect !== undefined) {
      const request = requireObject(expect, 'expect');
      if (request.deviceId !== undefined
        && requireDeviceId(request.deviceId) !== meta.deviceId) {
        throw new ProtocolError('snapshot deviceId does not match the captured device');
      }
      if (request.runId !== undefined
        && requireInt(request.runId, 'expect.runId', 1, UINT32_MAX) !== meta.runId) {
        throw new ProtocolError('snapshot runId does not match the requested run');
      }
      if (request.bootId !== undefined) {
        const expectedBoot = requireBootId(request.bootId);
        if (meta.bootId !== expectedBoot) {
          throw new ProtocolError('snapshot runBootId does not match the captured run boot id');
        }
      }
    }
    return {
      schemaVersion: SCHEMA_VERSION,
      meta,
      throughLogSeq,
      throughEventRevision,
    };
  }

  /**
   * @param {Record<string, any>} page
   * @param {Record<string, any>} expect
   * @param {string} label
   */
  function requireIdentityEcho(page, expect, label) {
    const deviceId = requireDeviceId(page.deviceId);
    if (deviceId !== requireDeviceId(expect.deviceId)) {
      throw new ProtocolError(`${label} deviceId does not match the captured device`);
    }
    const runBootId = requireBootId(page.runBootId);
    if (runBootId !== requireBootId(expect.runBootId)) {
      throw new ProtocolError(`${label} runBootId does not match the captured run boot id`);
    }
    const runId = requireInt(page.runId, 'runId', 1, UINT32_MAX);
    if (runId !== requireInt(expect.runId, 'expect.runId', 1, UINT32_MAX)) {
      throw new ProtocolError(`${label} runId does not match the requested run`);
    }
  }

  /**
   * @param {Record<string, any>} sample
   * @param {number} index
   * @returns {number | null}
   */
  function parseSampleTemp(sample, index) {
    const temp = sample.temp;
    if (temp === null) {
      if (sample.fault === 0) {
        throw new ProtocolError(`samples[${index}].temp may be null only when fault is nonzero`);
      }
      return null;
    }
    if (typeof temp !== 'number' || !Number.isFinite(temp)) {
      throw new ProtocolError(`samples[${index}].temp must be a finite number or null`);
    }
    const float32 = Math.fround(temp);
    if (!Number.isFinite(float32)) {
      throw new ProtocolError(`samples[${index}].temp must fit in a 32-bit float`);
    }
    return float32;
  }

  /**
   * @param {unknown} value
   * @param {number} index
   * @returns {RunSample}
   */
  function parseSample(value, index) {
    const sample = requireObject(value, `samples[${index}]`);
    const logSeq = requireInt(sample.logSeq, 'logSeq', 1, UINT32_MAX);
    const acqSeq = requireInt(sample.acqSeq, 'acqSeq', 1, UINT32_MAX);
    const ms = requireInt(sample.ms, 'ms', 0, MAX_RUN_MS);
    const fault = requireInt(sample.fault, 'fault', 0, 255);
    const intervalFault = requireInt(sample.intervalFault, 'intervalFault', 0, 255);
    const flags = requireInt(sample.flags, 'flags', 0, 3);
    const temp = parseSampleTemp(sample, index);
    return { logSeq, acqSeq, ms, temp, fault, intervalFault, flags };
  }

  /**
   * Bounded recorded-sample page. The request context pins the captured
   * identity and the requested cursors so echoed envelopes are verifiable.
   * @param {unknown} value
   * @param {SamplesPageRequest} expect
   * @returns {SamplesPageResult}
   */
  function parseSamplesPage(value, expect) {
    const page = requireObject(value, 'samples page');
    requireSchemaVersion(page);
    const request = requireObject(expect, 'expect');
    requireIdentityEcho(page, request, 'samples page');
    const after = requireInt(page.afterLogSeq, 'afterLogSeq', 0, UINT32_MAX);
    if (after !== requireInt(request.afterLogSeq, 'expect.afterLogSeq', 0, UINT32_MAX)) {
      throw new ProtocolError('afterLogSeq echo does not match the request');
    }
    const through = requireInt(page.throughLogSeq, 'throughLogSeq', 0, UINT32_MAX);
    if (through !== requireInt(request.throughLogSeq, 'expect.throughLogSeq', 0, UINT32_MAX)) {
      throw new ProtocolError('throughLogSeq echo does not match the request');
    }
    if (after > through) {
      throw new ProtocolError('afterLogSeq must not exceed throughLogSeq');
    }
    const limit = request.limit === undefined
      ? MAX_SAMPLE_PAGE
      : requireInt(request.limit, 'expect.limit', 1, MAX_SAMPLE_PAGE);
    if (!Array.isArray(page.samples)) {
      throw new ProtocolError('samples must be an array');
    }
    const rawSamples = page.samples;
    if (rawSamples.length > limit) {
      throw new ProtocolError(`samples must contain at most ${limit} records`);
    }
    /** @type {RunSample[]} */
    const samples = [];
    let terminalSeen = false;
    for (let i = 0; i < rawSamples.length; i += 1) {
      const sample = parseSample(rawSamples[i], i);
      if (sample.logSeq !== after + i + 1) {
        throw new ProtocolError(`samples[${i}].logSeq must be consecutive from afterLogSeq`);
      }
      if (i > 0) {
        const previous = samples[i - 1];
        if (sample.acqSeq <= previous.acqSeq) {
          throw new ProtocolError(`samples[${i}].acqSeq must increase`);
        }
        if (sample.ms <= previous.ms) {
          throw new ProtocolError(`samples[${i}].ms must increase`);
        }
      }
      if ((sample.flags & 1) !== 0) {
        if (i !== rawSamples.length - 1) {
          throw new ProtocolError(`samples[${i}] is terminal but records follow it`);
        }
        terminalSeen = true;
      }
      samples.push(sample);
    }
    const next = requireInt(page.nextAfterLogSeq, 'nextAfterLogSeq', 0, UINT32_MAX);
    if (next !== after + samples.length) {
      throw new ProtocolError('nextAfterLogSeq must equal afterLogSeq plus the delivered record count');
    }
    if (next > through) {
      throw new ProtocolError('nextAfterLogSeq must not exceed throughLogSeq');
    }
    if (samples.length === 0 && after !== through) {
      throw new ProtocolError('an empty samples page is only valid at throughLogSeq');
    }
    if (typeof page.done !== 'boolean') {
      throw new ProtocolError('done must be a boolean');
    }
    const done = page.done;
    if (done !== (next === through)) {
      throw new ProtocolError('done must be true exactly when the cursor reaches throughLogSeq');
    }
    if (terminalSeen && !done) {
      throw new ProtocolError('a terminal sample must end the recorded log');
    }
    return {
      schemaVersion: SCHEMA_VERSION,
      deviceId: page.deviceId,
      runBootId: page.runBootId,
      runId: page.runId,
      afterLogSeq: after,
      throughLogSeq: through,
      samples,
      nextAfterLogSeq: next,
      done,
    };
  }

  /**
   * @param {unknown} value
   * @param {number} index
   * @returns {RunEvent}
   */
  function parseEvent(value, index) {
    const event = requireObject(value, `events[${index}]`);
    const revision = requireInt(event.revision, 'revision', 1, UINT32_MAX);
    if (typeof event.type !== 'string') {
      throw new ProtocolError('type must be a string');
    }
    // The journal always begins with run.started at revision 1.
    if (revision === 1 && event.type !== 'run.started') {
      throw new ProtocolError('journal revision 1 must be run.started');
    }
    switch (event.type) {
      case 'run.started': {
        if (revision !== 1) {
          throw new ProtocolError('run.started must be journal revision 1');
        }
        const requestId = requireIdentifier(event.requestId, 'requestId');
        const ms = requireInt(event.ms, 'ms', 0, MAX_RUN_MS);
        if (event.status !== 'active') {
          throw new ProtocolError("run.started status must be 'active'");
        }
        if (event.errorCode !== null) {
          throw new ProtocolError('run.started errorCode must be null');
        }
        const droppedAcquired = requireInt(event.droppedAcquired, 'droppedAcquired', 0, UINT32_MAX);
        if (droppedAcquired !== 0) {
          throw new ProtocolError('run.started droppedAcquired must be 0');
        }
        return {
          revision,
          type: 'run.started',
          requestId,
          ms,
          status: 'active',
          errorCode: null,
          droppedAcquired: 0,
        };
      }
      case 'run.ended': {
        const requestId = requireIdentifier(event.requestId, 'requestId');
        const ms = requireInt(event.ms, 'ms', 0, MAX_RUN_MS);
        const status = requireRunStatus(event.status);
        if (status === 'active') {
          throw new ProtocolError("run.ended status must not be 'active'");
        }
        const errorCode = requireErrorCode(event.errorCode);
        const droppedAcquired = requireInt(event.droppedAcquired, 'droppedAcquired', 0, UINT32_MAX);
        return {
          revision,
          type: 'run.ended',
          requestId,
          ms,
          status,
          errorCode,
          droppedAcquired,
        };
      }
      case 'marker.created':
      case 'marker.updated': {
        const operationId = requireIdentifier(event.operationId, 'operationId');
        const id = requireIdentifier(event.id, 'id');
        const baseRevision = requireInt(event.baseRevision, 'baseRevision', 0, UINT32_MAX);
        if (event.type === 'marker.created' && baseRevision !== 0) {
          throw new ProtocolError('marker.created baseRevision must be 0');
        }
        if (event.type === 'marker.updated' && baseRevision < 1) {
          throw new ProtocolError('marker.updated baseRevision must be positive');
        }
        const timeBasis = requireTimeBasis(event.timeBasis);
        /** @type {number | null} */
        let ms;
        if (timeBasis === 'untimed') {
          if (event.ms !== null) {
            throw new ProtocolError('untimed marker events must carry ms:null');
          }
          ms = null;
        } else {
          ms = requireInt(event.ms, 'ms', 0, MAX_RUN_MS);
        }
        if (typeof event.note !== 'string') {
          throw new ProtocolError('note must be a string');
        }
        requireNoteText(event.note, 'note');
        return {
          revision,
          type: event.type === 'marker.created' ? 'marker.created' : 'marker.updated',
          operationId,
          id,
          baseRevision,
          ms,
          note: event.note,
          timeBasis,
        };
      }
      case 'marker.deleted': {
        // Tombstones carry no timestamp or note; none may be invented later.
        for (const field of ['ms', 'note', 'timeBasis']) {
          if (field in event) {
            throw new ProtocolError(`marker.deleted must not include ${field}`);
          }
        }
        const operationId = requireIdentifier(event.operationId, 'operationId');
        const id = requireIdentifier(event.id, 'id');
        const baseRevision = requireInt(event.baseRevision, 'baseRevision', 1, UINT32_MAX);
        return {
          revision,
          type: 'marker.deleted',
          operationId,
          id,
          baseRevision,
        };
      }
      default:
        throw new ProtocolError(`unknown event type ${event.type}`);
    }
  }

  /**
   * Bounded journal-event page with the same cursor/identity rules as samples.
   * @param {unknown} value
   * @param {EventsPageRequest} expect
   * @returns {EventsPageResult}
   */
  function parseEventsPage(value, expect) {
    const page = requireObject(value, 'events page');
    requireSchemaVersion(page);
    const request = requireObject(expect, 'expect');
    requireIdentityEcho(page, request, 'events page');
    const after = requireInt(page.afterRevision, 'afterRevision', 0, UINT32_MAX);
    if (after !== requireInt(request.afterRevision, 'expect.afterRevision', 0, UINT32_MAX)) {
      throw new ProtocolError('afterRevision echo does not match the request');
    }
    const through = requireInt(page.throughRevision, 'throughRevision', 0, UINT32_MAX);
    if (through !== requireInt(request.throughRevision, 'expect.throughRevision', 0, UINT32_MAX)) {
      throw new ProtocolError('throughRevision echo does not match the request');
    }
    if (after > through) {
      throw new ProtocolError('afterRevision must not exceed throughRevision');
    }
    const limit = request.limit === undefined
      ? MAX_EVENT_PAGE
      : requireInt(request.limit, 'expect.limit', 1, MAX_EVENT_PAGE);
    if (!Array.isArray(page.events)) {
      throw new ProtocolError('events must be an array');
    }
    const rawEvents = page.events;
    if (rawEvents.length > limit) {
      throw new ProtocolError(`events must contain at most ${limit} entries`);
    }
    /** @type {RunEvent[]} */
    const events = [];
    const seenOperations = new Set();
    for (let i = 0; i < rawEvents.length; i += 1) {
      const event = parseEvent(rawEvents[i], i);
      if (event.revision !== after + i + 1) {
        throw new ProtocolError(`events[${i}].revision must be consecutive from afterRevision`);
      }
      if (event.type !== 'run.started' && event.type !== 'run.ended') {
        if (seenOperations.has(event.operationId)) {
          throw new ProtocolError(`events[${i}].operationId repeats an operation in the page`);
        }
        seenOperations.add(event.operationId);
      }
      events.push(event);
    }
    const next = requireInt(page.nextAfterRevision, 'nextAfterRevision', 0, UINT32_MAX);
    if (next !== after + events.length) {
      throw new ProtocolError('nextAfterRevision must equal afterRevision plus the delivered entry count');
    }
    if (next > through) {
      throw new ProtocolError('nextAfterRevision must not exceed throughRevision');
    }
    if (events.length === 0 && after !== through) {
      throw new ProtocolError('an empty events page is only valid at throughRevision');
    }
    if (typeof page.done !== 'boolean') {
      throw new ProtocolError('done must be a boolean');
    }
    const done = page.done;
    if (done !== (next === through)) {
      throw new ProtocolError('done must be true exactly when the cursor reaches throughRevision');
    }
    return {
      schemaVersion: SCHEMA_VERSION,
      deviceId: page.deviceId,
      runBootId: page.runBootId,
      runId: page.runId,
      afterRevision: after,
      throughRevision: through,
      events,
      nextAfterRevision: next,
      done,
    };
  }

  /**
   * Marker mutation acknowledgment (only persisted results are successful).
   * @param {unknown} value
   * @param {MutationResultRequest} [expect]
   * @returns {MutationResultRecord}
   */
  function parseMutationResult(value, expect) {
    const result = requireObject(value, 'mutation result');
    requireSchemaVersion(result);
    const runId = requireInt(result.runId, 'runId', 1, UINT32_MAX);
    const operationId = requireIdentifier(result.operationId, 'operationId');
    const id = requireIdentifier(result.id, 'id');
    const revision = requireInt(result.revision, 'revision', 1, UINT32_MAX);
    requireBooleanField(result, 'persisted');
    if (result.persisted !== true) {
      throw new ProtocolError('persisted must be true');
    }
    requireBooleanField(result, 'deleted');
    if (expect !== undefined) {
      const request = requireObject(expect, 'expect');
      if (request.runId !== undefined
        && requireInt(request.runId, 'expect.runId', 1, UINT32_MAX) !== runId) {
        throw new ProtocolError('mutation result runId does not match the requested run');
      }
      if (request.operationId !== undefined
        && requireIdentifier(request.operationId, 'expect.operationId') !== operationId) {
        throw new ProtocolError('mutation result operationId does not match the request');
      }
      if (request.id !== undefined
        && requireIdentifier(request.id, 'expect.id') !== id) {
        throw new ProtocolError('mutation result id does not match the request');
      }
    }
    return {
      schemaVersion: SCHEMA_VERSION,
      runId,
      operationId,
      id,
      revision,
      persisted: true,
      deleted: result.deleted,
    };
  }

  root.PyroVue = Object.assign(root.PyroVue || {}, {
    ProtocolError,
    parseRunMeta,
    parseRunList,
    parseSnapshot,
    parseSamplesPage,
    parseEventsPage,
    parseMutationResult,
    normalizeNote,
  });
})(/** @type {any} */ (globalThis));
