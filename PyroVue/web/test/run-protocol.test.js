'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');

require('../src/js/run-protocol.js');

const {
  ProtocolError,
  parseRunMeta,
  parseRunList,
  parseSnapshot,
  parseSamplesPage,
  parseEventsPage,
  parseMutationResult,
  normalizeNote,
} = globalThis.PyroVue;

const DEVICE_ID = 'a1b2c3d4e5f6';
const BOOT_ID = '0123456789abcdef';

const NUL = String.fromCharCode(0);
const TAB = String.fromCharCode(9);
const NEWLINE = String.fromCharCode(10);
const NBSP = String.fromCharCode(0x00a0);
const BOM = String.fromCharCode(0xfeff);
const LINE_SEP = String.fromCharCode(0x2028);
const IDEOGRAPHIC_SPACE = String.fromCharCode(0x3000);
const HIGH_SURROGATE = String.fromCharCode(0xd800);
const LOW_SURROGATE = String.fromCharCode(0xdc00);
const FIRE = String.fromCodePoint(0x1f525);

// ---------------------------------------------------------------------------
// Inline fixture factories (no fixture framework).
// ---------------------------------------------------------------------------

function healthyMeta(overrides = {}) {
  return {
    schemaVersion: 1,
    deviceId: DEVICE_ID,
    bootId: BOOT_ID,
    runId: 7,
    firmware: 'run-logging-1',
    protocolVersion: 2,
    status: 'complete',
    rawComplete: true,
    acquireIntervalMs: 1000,
    logIntervalMs: 2000,
    graphBucketMs: 5000,
    maxRunMs: 86400000,
    startUptimeMs: 12345,
    endElapsedMs: 9000,
    wallStartUtcMs: null,
    wallTimeSource: 'unavailable',
    sampleCount: 3,
    durableLogSeq: 3,
    latestAcqSeq: 7,
    eventRevision: 3,
    annotationCount: 1,
    acquisitionGaps: 0,
    droppedAcquired: 0,
    sizeBytes: 4096,
    recoveredTailBytes: 0,
    errorCode: null,
    ...overrides,
  };
}

function corruptMeta(overrides = {}) {
  return {
    schemaVersion: 1,
    deviceId: DEVICE_ID,
    runId: 9,
    status: 'corrupt',
    path: '/runs/9',
    sizeBytes: 128,
    recoveredTailBytes: 24,
    rawComplete: false,
    errorCode: 'CORRUPT_RUN',
    ...overrides,
  };
}

// Cross-language smoke samples published by the main agent for the
// native/Node contract bridge.
function smokeSamples() {
  return [
    { logSeq: 1, acqSeq: 1, ms: 1000, temp: -0, fault: 0, intervalFault: 0, flags: 0 },
    { logSeq: 2, acqSeq: 3, ms: 3000, temp: null, fault: 9, intervalFault: 9, flags: 0 },
    { logSeq: 3, acqSeq: 7, ms: 7000, temp: 1187.125, fault: 0, intervalFault: 1, flags: 3 },
  ];
}

function samplesPage(overrides = {}) {
  return {
    schemaVersion: 1,
    deviceId: DEVICE_ID,
    runBootId: BOOT_ID,
    runId: 7,
    afterLogSeq: 0,
    throughLogSeq: 3,
    samples: smokeSamples(),
    nextAfterLogSeq: 3,
    done: true,
    ...overrides,
  };
}

function samplesRequest(overrides = {}) {
  return {
    deviceId: DEVICE_ID,
    runBootId: BOOT_ID,
    runId: 7,
    afterLogSeq: 0,
    throughLogSeq: 3,
    ...overrides,
  };
}

const FIRE_NOTE = 'He said "hi"' + NEWLINE + 'second line ' + FIRE;

function startedEvent(overrides = {}) {
  return {
    revision: 1,
    type: 'run.started',
    requestId: 'req-start',
    ms: 0,
    status: 'active',
    errorCode: null,
    droppedAcquired: 0,
    ...overrides,
  };
}

function endedEvent(overrides = {}) {
  return {
    revision: 3,
    type: 'run.ended',
    requestId: 'req-stop',
    ms: 9000,
    status: 'complete',
    errorCode: null,
    droppedAcquired: 0,
    ...overrides,
  };
}

function createdEvent(overrides = {}) {
  return {
    revision: 2,
    type: 'marker.created',
    operationId: 'op-1',
    id: 'note-1',
    baseRevision: 0,
    ms: 5000,
    note: FIRE_NOTE,
    timeBasis: 'estimated',
    ...overrides,
  };
}

function deletedEvent(overrides = {}) {
  return {
    revision: 2,
    type: 'marker.deleted',
    operationId: 'op-1',
    id: 'note-1',
    baseRevision: 1,
    ...overrides,
  };
}

function smokeEvents() {
  return [startedEvent(), createdEvent(), endedEvent()];
}

function eventsPage(overrides = {}) {
  return {
    schemaVersion: 1,
    deviceId: DEVICE_ID,
    runBootId: BOOT_ID,
    runId: 7,
    afterRevision: 0,
    throughRevision: 3,
    events: smokeEvents(),
    nextAfterRevision: 3,
    done: true,
    ...overrides,
  };
}

function eventsRequest(overrides = {}) {
  return {
    deviceId: DEVICE_ID,
    runBootId: BOOT_ID,
    runId: 7,
    afterRevision: 0,
    throughRevision: 3,
    ...overrides,
  };
}

function metaSnapshot(overrides = {}) {
  return {
    schemaVersion: 1,
    meta: healthyMeta(),
    throughLogSeq: 3,
    throughEventRevision: 3,
    ...overrides,
  };
}

// ---------------------------------------------------------------------------
// parseRunMeta
// ---------------------------------------------------------------------------

test('parses healthy run metadata including historical origin protocol 2', () => {
  const meta = parseRunMeta(healthyMeta());
  assert.deepEqual(meta, healthyMeta());
  assert.equal(meta.protocolVersion, 2);

  const current = parseRunMeta(healthyMeta({ protocolVersion: 3 }));
  assert.equal(current.protocolVersion, 3);
});

test('accepts the cross-language smoke metadata with a gap and rawComplete false', () => {
  const meta = parseRunMeta(healthyMeta({ rawComplete: false, acquisitionGaps: 1 }));
  assert.equal(meta.rawComplete, false);
  assert.equal(meta.acquisitionGaps, 1);
  assert.equal(meta.sampleCount, 3);
  assert.equal(meta.latestAcqSeq, 7);
});

test('rejects invalid schema versions and run statuses', () => {
  assert.throws(() => parseRunMeta(healthyMeta({ schemaVersion: 2 })), ProtocolError);
  assert.throws(() => parseRunMeta(healthyMeta({ schemaVersion: '1' })), ProtocolError);
  assert.throws(() => parseRunMeta(healthyMeta({ status: 'zombie' })), ProtocolError);
  assert.throws(() => parseRunMeta(healthyMeta({ status: '' })), ProtocolError);
  assert.throws(() => parseRunMeta({}), /schemaVersion must be 1/);
});

test('rejects identity fields that violate the wire formats', () => {
  assert.throws(() => parseRunMeta(healthyMeta({ deviceId: 'A1B2C3D4E5F6' })), ProtocolError);
  assert.throws(() => parseRunMeta(healthyMeta({ deviceId: 'a1b2c3d4e5' })), ProtocolError);
  assert.throws(() => parseRunMeta(healthyMeta({ deviceId: 'zzzzzzzzzzzz' })), ProtocolError);
  assert.throws(() => parseRunMeta(healthyMeta({ bootId: '0123456789abcde' })), ProtocolError);
  assert.throws(() => parseRunMeta(healthyMeta({ bootId: '0123456789ABCDEF' })), ProtocolError);
  assert.throws(() => parseRunMeta(healthyMeta({ runId: 0 })), ProtocolError);
  assert.throws(() => parseRunMeta(healthyMeta({ runId: 1.5 })), ProtocolError);
  assert.throws(() => parseRunMeta(healthyMeta({ runId: 2 ** 32 })), ProtocolError);
});

test('rejects non-positive or fractional protocol provenance', () => {
  assert.throws(() => parseRunMeta(healthyMeta({ protocolVersion: 0 })), ProtocolError);
  assert.throws(() => parseRunMeta(healthyMeta({ protocolVersion: -1 })), ProtocolError);
  assert.throws(() => parseRunMeta(healthyMeta({ protocolVersion: 2.5 })), ProtocolError);
  assert.throws(() => parseRunMeta(healthyMeta({ protocolVersion: '2' })), ProtocolError);
});

test('rejects incoherent counts and rawComplete claims', () => {
  assert.throws(() => parseRunMeta(healthyMeta({ sampleCount: 2 })), /sampleCount/);
  assert.throws(() => parseRunMeta(healthyMeta({ durableLogSeq: 4 })), /sampleCount/);
  assert.throws(
    () => parseRunMeta(healthyMeta({ rawComplete: true, acquisitionGaps: 1 })),
    /rawComplete/,
  );
  assert.throws(
    () => parseRunMeta(healthyMeta({ rawComplete: true, droppedAcquired: 1 })),
    /rawComplete/,
  );
  assert.throws(
    () => parseRunMeta(healthyMeta({ rawComplete: true, status: 'interrupted' })),
    /rawComplete/,
  );
});

test('enforces end time presence per status', () => {
  assert.throws(
    () => parseRunMeta(healthyMeta({ status: 'active', rawComplete: false, endElapsedMs: 5 })),
    /endElapsedMs/,
  );
  assert.throws(
    () => parseRunMeta(healthyMeta({ status: 'complete', endElapsedMs: null })),
    /endElapsedMs/,
  );
  assert.throws(() => parseRunMeta(healthyMeta({ endElapsedMs: 86400001 })), /endElapsedMs/);
});

test('enforces wall-time source coherence', () => {
  assert.throws(
    () => parseRunMeta(healthyMeta({ wallTimeSource: 'client-estimated', wallStartUtcMs: null })),
    /wallStartUtcMs/,
  );
  assert.throws(
    () => parseRunMeta(healthyMeta({ wallTimeSource: 'unavailable', wallStartUtcMs: 1700000000000 })),
    /wallStartUtcMs/,
  );
  assert.throws(() => parseRunMeta(healthyMeta({ wallTimeSource: 'trusted' })), ProtocolError);
});

test('rejects invalid declared semantics for rates, bucket and journal revision', () => {
  assert.throws(() => parseRunMeta(healthyMeta({ graphBucketMs: 4000 })), /graphBucketMs/);
  assert.throws(() => parseRunMeta(healthyMeta({ acquireIntervalMs: 0 })), ProtocolError);
  assert.throws(() => parseRunMeta(healthyMeta({ logIntervalMs: 0 })), ProtocolError);
  assert.throws(() => parseRunMeta(healthyMeta({ maxRunMs: 0 })), ProtocolError);
  assert.throws(() => parseRunMeta(healthyMeta({ maxRunMs: 86400001 })), ProtocolError);
  assert.throws(() => parseRunMeta(healthyMeta({ eventRevision: 0 })), ProtocolError);
  assert.throws(() => parseRunMeta(healthyMeta({ acquireIntervalMs: 500 })), /acquireIntervalMs/);
  assert.throws(() => parseRunMeta(healthyMeta({ logIntervalMs: 5000 })), /logIntervalMs/);
  assert.throws(() => parseRunMeta(healthyMeta({ maxRunMs: 1000 })), /maxRunMs/);
});

test('rejects unknown errorCode strings', () => {
  assert.throws(() => parseRunMeta(healthyMeta({ errorCode: 'BOOM' })), /errorCode/);
  assert.throws(() => parseRunMeta(healthyMeta({ errorCode: '' })), /errorCode/);
});


test('parses corrupt catalog entries with only their declared fields', () => {
  const meta = parseRunMeta(corruptMeta());
  assert.deepEqual(meta, {
    schemaVersion: 1,
    deviceId: DEVICE_ID,
    runId: 9,
    status: 'corrupt',
    path: '/runs/9',
    sizeBytes: 128,
    recoveredTailBytes: 24,
    rawComplete: false,
    errorCode: 'CORRUPT_RUN',
  });
});

test('rejects corrupt entries with healthy fields or missing declared fields', () => {
  assert.throws(() => parseRunMeta(corruptMeta({ sampleCount: 0 })), /must not include sampleCount/);
  assert.throws(() => parseRunMeta(corruptMeta({ bootId: BOOT_ID })), /must not include bootId/);
  assert.throws(() => parseRunMeta(corruptMeta({ corrupt: false })), ProtocolError);
  assert.throws(() => parseRunMeta(corruptMeta({ path: '' })), ProtocolError);
  assert.throws(() => parseRunMeta(corruptMeta({ errorCode: 'BOGUS' })), /errorCode/);
  assert.throws(() => parseRunMeta(corruptMeta({ errorCode: null })), /errorCode/);
  assert.throws(() => parseRunMeta(corruptMeta({ rawComplete: true })), /rawComplete/);
  assert.throws(() => parseRunMeta(corruptMeta({ recoveredTailBytes: undefined })), /recoveredTailBytes/);
  const withoutPath = { ...corruptMeta() };
  delete withoutPath.path;
  assert.throws(() => parseRunMeta(withoutPath), /path/);
});

// ---------------------------------------------------------------------------
// parseRunList
// ---------------------------------------------------------------------------

test('parses mixed healthy and corrupt catalogs newest first', () => {
  const result = parseRunList({
    schemaVersion: 1,
    runs: [healthyMeta({ runId: 8 }), corruptMeta({ runId: 7 }), healthyMeta({ runId: 6 })],
    nextBeforeRunId: 6,
  });
  assert.equal(result.runs.length, 3);
  assert.equal(result.runs[0].runId, 8);
  assert.equal(result.runs[1].status, 'corrupt');
  assert.equal(result.nextBeforeRunId, 6);
});

test('rejects non-descending or duplicate run order', () => {
  assert.throws(
    () => parseRunList({
      schemaVersion: 1,
      nextBeforeRunId: null,
      runs: [healthyMeta({ runId: 6 }), healthyMeta({ runId: 7 })],
    }),
    ProtocolError,
  );
  assert.throws(
    () => parseRunList({
      schemaVersion: 1,
      nextBeforeRunId: null,
      runs: [healthyMeta({ runId: 7 }), healthyMeta({ runId: 7 })],
    }),
    ProtocolError,
  );
});

test('preserves the empty catalog terminal cursor', () => {
  const empty = parseRunList({ schemaVersion: 1, runs: [], nextBeforeRunId: null });
  assert.deepEqual(empty.runs, []);
  assert.equal(empty.nextBeforeRunId, null);

});

test('rejects incoherent nextBeforeRunId cursors', () => {
  assert.throws(
    () => parseRunList({ schemaVersion: 1, runs: [healthyMeta({ runId: 7 })], nextBeforeRunId: 6 }),
    /nextBeforeRunId/,
  );
  assert.throws(
    () => parseRunList({ schemaVersion: 1, runs: [healthyMeta({ runId: 7 })], nextBeforeRunId: 8 }),
    /nextBeforeRunId/,
  );
  assert.throws(
    () => parseRunList({ schemaVersion: 1, runs: [], nextBeforeRunId: 6 }),
    /nextBeforeRunId/,
  );
  assert.throws(
    () => parseRunList({ schemaVersion: 1, runs: [], nextBeforeRunId: 1.5 }),
    ProtocolError,
  );
  assert.throws(() => parseRunList({ schemaVersion: 1, runs: [] }), /nextBeforeRunId/);
  assert.throws(() => parseRunList({ schemaVersion: 1, runs: [], nextBeforeRunId: 0 }), /nextBeforeRunId/);
});

test('rejects oversized catalogs and bad schema versions', () => {
  const runs = [];
  for (let i = 0; i < 17; i += 1) runs.push(healthyMeta({ runId: 100 - i }));
  assert.throws(
    () => parseRunList({ schemaVersion: 1, runs, nextBeforeRunId: 84 }),
    /at most 16/,
  );
  assert.throws(
    () => parseRunList({ schemaVersion: 2, runs: [], nextBeforeRunId: null }),
    ProtocolError,
  );
});

// ---------------------------------------------------------------------------
// parseSnapshot
// ---------------------------------------------------------------------------

test('parses snapshots whose watermark equals the meta counts and revision', () => {
  const snapshot = parseSnapshot(metaSnapshot());
  assert.equal(snapshot.throughLogSeq, 3);
  assert.equal(snapshot.throughEventRevision, 3);
  assert.equal(snapshot.meta.runId, 7);
  assert.equal(snapshot.meta.protocolVersion, 2);
});

test('rejects watermark mismatches against meta counts and revisions', () => {
  assert.throws(() => parseSnapshot(metaSnapshot({ throughLogSeq: 4 })), /throughLogSeq/);
  assert.throws(() => parseSnapshot(metaSnapshot({ throughLogSeq: 2 })), /throughLogSeq/);
  assert.throws(() => parseSnapshot(metaSnapshot({ throughEventRevision: 2 })), /throughEventRevision/);
  assert.throws(() => parseSnapshot(metaSnapshot({ throughEventRevision: 0 })), /throughEventRevision/);
});

test('verifies captured snapshot identity when requested', () => {
  assert.throws(
    () => parseSnapshot(metaSnapshot(), { deviceId: 'ffffffffffff' }),
    /deviceId/,
  );
  assert.throws(() => parseSnapshot(metaSnapshot(), { runId: 8 }), /runId/);
  assert.throws(
    () => parseSnapshot(metaSnapshot(), { bootId: 'ffffffffffffffff' }),
    /runBootId/,
  );
});

test('refuses history snapshots for corrupt catalog entries', () => {
  assert.throws(() => parseSnapshot({
    schemaVersion: 1, meta: corruptMeta(), throughLogSeq: 0, throughEventRevision: 0,
  }), /corrupt/);
});

// ---------------------------------------------------------------------------
// parseSamplesPage
// ---------------------------------------------------------------------------

test('parses the cross-language sample fixture preserving float32 values', () => {
  const page = parseSamplesPage(samplesPage(), samplesRequest());
  assert.equal(page.samples.length, 3);
  assert.ok(Object.is(page.samples[0].temp, -0));
  assert.equal(page.samples[1].temp, null);
  assert.equal(page.samples[1].fault, 9);
  assert.equal(page.samples[1].intervalFault, 9);
  assert.equal(page.samples[2].temp, 1187.125);
  assert.equal(page.samples[2].flags, 3);
  assert.equal(page.nextAfterLogSeq, 3);
  assert.equal(page.done, true);
});

test('preserves negative zero temperatures parsed from JSON text', () => {
  const wire = JSON.parse(`{
    "schemaVersion": 1,
    "deviceId": "${DEVICE_ID}",
    "runBootId": "${BOOT_ID}",
    "runId": 7,
    "afterLogSeq": 0,
    "throughLogSeq": 1,
    "samples": [
      { "logSeq": 1, "acqSeq": 1, "ms": 1000, "temp": -0, "fault": 0, "intervalFault": 0, "flags": 0 }
    ],
    "nextAfterLogSeq": 1,
    "done": true
  }`);
  const page = parseSamplesPage(wire, samplesRequest({ throughLogSeq: 1 }));
  assert.ok(Object.is(page.samples[0].temp, -0));
});

test('normalizes temperatures with Math.fround and rejects non-finite or float32 overflow', () => {
  const rounded = parseSamplesPage(samplesPage({
    samples: [{ logSeq: 1, acqSeq: 1, ms: 1000, temp: 0.1, fault: 0, intervalFault: 0, flags: 0 }],
    nextAfterLogSeq: 1,
    done: false,
  }), samplesRequest());
  assert.ok(Object.is(rounded.samples[0].temp, Math.fround(0.1)));

  const bad = [1e308, -3.5e38, Infinity, -Infinity, NaN, '20', undefined];
  for (const temp of bad) {
    assert.throws(
      () => parseSamplesPage(samplesPage({
        samples: [{ logSeq: 1, acqSeq: 1, ms: 1000, temp, fault: 0, intervalFault: 0, flags: 0 }],
        nextAfterLogSeq: 1,
        done: false,
      }), samplesRequest()),
      ProtocolError,
    );
  }
});

test('allows null temperature only with a declared fault', () => {
  assert.throws(
    () => parseSamplesPage(samplesPage({
      samples: [{ logSeq: 1, acqSeq: 1, ms: 1000, temp: null, fault: 0, intervalFault: 1, flags: 0 }],
      nextAfterLogSeq: 1,
      done: false,
    }), samplesRequest()),
    /fault/,
  );
});

test('validates sample page identity echo and requested cursors', () => {
  assert.throws(
    () => parseSamplesPage(samplesPage({ deviceId: 'ffffffffffff' }), samplesRequest()),
    /deviceId/,
  );
  assert.throws(
    () => parseSamplesPage(samplesPage({ runBootId: 'ffffffffffffffff' }), samplesRequest()),
    /runBootId/,
  );
  assert.throws(() => parseSamplesPage(samplesPage({ runId: 8 }), samplesRequest()), /runId/);
  assert.throws(
    () => parseSamplesPage(samplesPage({ afterLogSeq: 1 }), samplesRequest()),
    /afterLogSeq echo/,
  );
  assert.throws(
    () => parseSamplesPage(samplesPage({ throughLogSeq: 4 }), samplesRequest()),
    /throughLogSeq echo/,
  );
  assert.throws(
    () => parseSamplesPage(
      samplesPage({ afterLogSeq: 3, throughLogSeq: 2, samples: [], nextAfterLogSeq: 3, done: true }),
      samplesRequest({ afterLogSeq: 3, throughLogSeq: 2 }),
    ),
    /afterLogSeq must not exceed/,
  );
});

test('requires consecutive logSeq with increasing acqSeq and ms', () => {
  const base = { nextAfterLogSeq: 3, done: false };
  assert.throws(
    () => parseSamplesPage(samplesPage({
      ...base,
      samples: [smokeSamples()[0], smokeSamples()[1], { ...smokeSamples()[2], logSeq: 4 }],
    }), samplesRequest()),
    /logSeq/,
  );
  assert.throws(
    () => parseSamplesPage(samplesPage({
      ...base,
      samples: smokeSamples().map((sample, i) => ({ ...sample, logSeq: i + 2 })),
    }), samplesRequest()),
    /logSeq/,
  );
  assert.throws(
    () => parseSamplesPage(samplesPage({
      ...base,
      samples: [smokeSamples()[0], { ...smokeSamples()[1], acqSeq: 1 }, smokeSamples()[2]],
    }), samplesRequest()),
    /acqSeq/,
  );
  assert.throws(
    () => parseSamplesPage(samplesPage({
      ...base,
      samples: [smokeSamples()[0], { ...smokeSamples()[1], ms: 1000 }, smokeSamples()[2]],
    }), samplesRequest()),
    /ms/,
  );
  assert.throws(
    () => parseSamplesPage(samplesPage({
      ...base,
      samples: [{ ...smokeSamples()[0], logSeq: 0, acqSeq: 1 }],
      nextAfterLogSeq: 1,
    }), samplesRequest()),
    /logSeq/,
  );
  assert.throws(
    () => parseSamplesPage(samplesPage({
      ...base,
      samples: [{ ...smokeSamples()[0], ms: 86400001 }],
      nextAfterLogSeq: 1,
    }), samplesRequest()),
    /ms/,
  );
});

test('allows short pages that advance and rejects empty pages before the watermark', () => {
  const short = parseSamplesPage(samplesPage({
    samples: smokeSamples().slice(0, 2),
    nextAfterLogSeq: 2,
    done: false,
  }), samplesRequest({ limit: 3 }));
  assert.equal(short.nextAfterLogSeq, 2);
  assert.equal(short.done, false);

  assert.throws(() => parseSamplesPage(samplesPage({
    samples: [],
    nextAfterLogSeq: 0,
    done: false,
  }), samplesRequest()), /empty samples page/);

  const atWatermark = parseSamplesPage(samplesPage({
    afterLogSeq: 3,
    throughLogSeq: 3,
    samples: [],
    nextAfterLogSeq: 3,
    done: true,
  }), samplesRequest({ afterLogSeq: 3, throughLogSeq: 3 }));
  assert.equal(atWatermark.samples.length, 0);
  assert.equal(atWatermark.done, true);
});

test('enforces sample cursor advance, count coherence and done', () => {
  assert.throws(() => parseSamplesPage(samplesPage({
    nextAfterLogSeq: 2,
    done: false,
  }), samplesRequest()), /nextAfterLogSeq/);
  assert.throws(() => parseSamplesPage(samplesPage({
    done: false,
  }), samplesRequest()), /done/);
  assert.throws(() => parseSamplesPage(samplesPage({
    throughLogSeq: 2,
    nextAfterLogSeq: 3,
    done: true,
  }), samplesRequest({ throughLogSeq: 2 })), /nextAfterLogSeq/);
  assert.throws(() => parseSamplesPage(samplesPage({
    done: 'yes',
  }), samplesRequest()), /done/);
});

test('rejects byte-overflow fields and terminal records that do not end the log', () => {
  const one = (overrides) => [{ ...smokeSamples()[0], ...overrides }];
  const base = { nextAfterLogSeq: 1, done: false };
  for (const field of ['fault', 'intervalFault', 'flags']) {
    assert.throws(
      () => parseSamplesPage(samplesPage({ ...base, samples: one({ [field]: 256 }) }), samplesRequest()),
      ProtocolError,
    );
    assert.throws(
      () => parseSamplesPage(samplesPage({ ...base, samples: one({ [field]: -1 }) }), samplesRequest()),
      ProtocolError,
    );
  }
  assert.throws(() => parseSamplesPage(samplesPage({
    samples: [{ ...smokeSamples()[0], flags: 1 }, smokeSamples()[1], smokeSamples()[2]],
    nextAfterLogSeq: 3,
    done: true,
  }), samplesRequest()), /terminal/);
  assert.throws(() => parseSamplesPage(samplesPage({
    throughLogSeq: 4,
    samples: smokeSamples(),
    nextAfterLogSeq: 3,
    done: false,
  }), samplesRequest({ throughLogSeq: 4 })), /terminal/);
});

test('rejects sample pages larger than the requested or maximum limit', () => {
  assert.throws(() => parseSamplesPage(samplesPage(), samplesRequest({ limit: 2 })), /at most 2/);
  const many = [];
  for (let i = 0; i < 65; i += 1) {
    many.push({
      logSeq: i + 1, acqSeq: 2 * i + 1, ms: 1000 * (i + 1), temp: 20, fault: 0, intervalFault: 0, flags: 0,
    });
  }
  assert.throws(() => parseSamplesPage(samplesPage({
    samples: many,
    throughLogSeq: 65,
    nextAfterLogSeq: 65,
    done: true,
  }), samplesRequest({ throughLogSeq: 65 })), /at most 64/);
});

// ---------------------------------------------------------------------------
// parseEventsPage
// ---------------------------------------------------------------------------

test('parses lifecycle and marker event variants from the cross-language fixture', () => {
  const page = parseEventsPage(eventsPage(), eventsRequest());
  assert.equal(page.events.length, 3);
  assert.equal(page.events[0].type, 'run.started');
  assert.equal(page.events[0].status, 'active');
  assert.equal(page.events[1].type, 'marker.created');
  assert.equal(page.events[1].note, FIRE_NOTE);
  assert.equal(page.events[2].type, 'run.ended');
  assert.equal(page.events[2].status, 'complete');
  assert.equal(page.nextAfterRevision, 3);
  assert.equal(page.done, true);
});

test('preserves sparse untimed markers and deleted tombstones without invented fields', () => {
  const page = parseEventsPage(eventsPage({
    events: [
      startedEvent(),
      createdEvent({ revision: 2, operationId: 'op-1', ms: null, note: '', timeBasis: 'untimed' }),
      deletedEvent({ revision: 3, operationId: 'op-2', baseRevision: 2 }),
      endedEvent({ revision: 4 }),
    ],
    throughRevision: 4,
    nextAfterRevision: 4,
  }), eventsRequest({ throughRevision: 4 }));
  assert.equal(page.events[1].ms, null);
  assert.equal(page.events[1].note, '');
  assert.ok(!('ms' in page.events[2]));
  assert.ok(!('note' in page.events[2]));
  assert.ok(!('timeBasis' in page.events[2]));

  for (const field of ['ms', 'note', 'timeBasis']) {
    assert.throws(() => parseEventsPage(eventsPage({
      events: [startedEvent(), deletedEvent({ [field]: field === 'ms' ? 5 : 'x' }), endedEvent()],
      throughRevision: 3,
      nextAfterRevision: 3,
    }), eventsRequest()), /marker.deleted must not include/);
  }
});

test('enforces marker time basis and ms coherence', () => {
  const withMarker = (marker) => eventsPage({
    events: [startedEvent(), createdEvent(marker), endedEvent()],
    throughRevision: 3,
    nextAfterRevision: 3,
  });
  assert.throws(
    () => parseEventsPage(withMarker({ ms: 5, timeBasis: 'untimed' }), eventsRequest()),
    /untimed/,
  );
  assert.throws(
    () => parseEventsPage(withMarker({ ms: null, timeBasis: 'estimated' }), eventsRequest()),
    ProtocolError,
  );
  assert.throws(
    () => parseEventsPage(withMarker({ ms: null, timeBasis: 'manual' }), eventsRequest()),
    ProtocolError,
  );
  assert.throws(
    () => parseEventsPage(withMarker({ ms: 86400001 }), eventsRequest()),
    ProtocolError,
  );
  assert.throws(
    () => parseEventsPage(withMarker({ timeBasis: 'measured' }), eventsRequest()),
    /timeBasis/,
  );
});

test('validates marker identifiers, base revisions and note budgets', () => {
  const withMarker = (marker) => eventsPage({
    events: [startedEvent(), createdEvent(marker), endedEvent()],
    throughRevision: 3,
    nextAfterRevision: 3,
  });
  assert.throws(() => parseEventsPage(withMarker({ operationId: 'bad id' }), eventsRequest()), /operationId/);
  assert.throws(() => parseEventsPage(withMarker({ id: '' }), eventsRequest()), /id/);
  assert.throws(() => parseEventsPage(withMarker({ id: 'a'.repeat(81) }), eventsRequest()), /id/);
  assert.throws(() => parseEventsPage(withMarker({ baseRevision: 1 }), eventsRequest()), /baseRevision/);
  assert.throws(() => parseEventsPage(withMarker({ note: 'a'.repeat(501) }), eventsRequest()), /scalar values/);
  assert.throws(() => parseEventsPage(withMarker({ note: 'a' + NUL + 'b' }), eventsRequest()), /NUL/);
  assert.throws(
    () => parseEventsPage(withMarker({ note: 'a' + HIGH_SURROGATE + 'b' }), eventsRequest()),
    /surrogates/,
  );

  assert.throws(() => parseEventsPage(eventsPage({
    events: [
      startedEvent(),
      createdEvent({ revision: 2 }),
      deletedEvent({ revision: 3, baseRevision: 0 }),
      endedEvent({ revision: 4 }),
    ],
    throughRevision: 4,
    nextAfterRevision: 4,
  }), eventsRequest({ throughRevision: 4 })), /baseRevision/);

  assert.throws(() => parseEventsPage(eventsPage({
    events: [
      startedEvent(),
      createdEvent({ revision: 2, type: 'marker.updated', baseRevision: 0 }),
      endedEvent(),
    ],
    throughRevision: 3,
    nextAfterRevision: 3,
  }), eventsRequest()), /baseRevision/);
});

test('validates lifecycle event declarations', () => {
  const pageWith = (started, ended) => eventsPage({
    events: [startedEvent(started), endedEvent({ revision: 2, ...ended })],
    throughRevision: 2,
    nextAfterRevision: 2,
  });
  const request = () => eventsRequest({ throughRevision: 2 });
  assert.throws(() => parseEventsPage(pageWith({ status: 'complete' }), request()), /run.started status/);
  assert.throws(() => parseEventsPage(pageWith({ errorCode: 'WRITE_FAILED' }), request()), /errorCode/);
  assert.throws(() => parseEventsPage(pageWith({ droppedAcquired: 1 }), request()), /droppedAcquired/);
  assert.throws(() => parseEventsPage(pageWith({}, { status: 'active' }), request()), /run.ended status/);
  assert.throws(() => parseEventsPage(pageWith({}, { errorCode: 'BOGUS' }), request()), /errorCode/);
  assert.throws(() => parseEventsPage(pageWith({}, { ms: 86400001 }), request()), ProtocolError);
  assert.throws(() => parseEventsPage(pageWith({ requestId: 'bad!' }), request()), /requestId/);
});

test('enforces journal revision semantics and operation uniqueness', () => {
  assert.throws(() => parseEventsPage(eventsPage({
    events: [startedEvent(), createdEvent({ revision: 1 }), endedEvent()],
    throughRevision: 3,
    nextAfterRevision: 3,
  }), eventsRequest()), /journal revision 1/);
  assert.throws(() => parseEventsPage(eventsPage({
    events: [createdEvent({ revision: 1 }), endedEvent({ revision: 2 })],
    throughRevision: 2,
    nextAfterRevision: 2,
  }), eventsRequest({ throughRevision: 2 })), /journal revision 1/);
  assert.throws(() => parseEventsPage(eventsPage({
    events: [startedEvent(), endedEvent({ revision: 3 })],
    throughRevision: 3,
    nextAfterRevision: 3,
  }), eventsRequest({ throughRevision: 3 })), /revision/);
  assert.throws(() => parseEventsPage(eventsPage({
    events: [
      startedEvent(),
      createdEvent({ revision: 2, operationId: 'op-1', baseRevision: 0 }),
      createdEvent({ revision: 3, operationId: 'op-1', id: 'note-2', baseRevision: 0 }),
      endedEvent({ revision: 4 }),
    ],
    throughRevision: 4,
    nextAfterRevision: 4,
  }), eventsRequest({ throughRevision: 4 })), /operationId/);
});

test('validates event page envelope identity, cursors, done and limits', () => {
  assert.throws(() => parseEventsPage(eventsPage({ deviceId: 'ffffffffffff' }), eventsRequest()), /deviceId/);
  assert.throws(() => parseEventsPage(eventsPage({ runBootId: 'ffffffffffffffff' }), eventsRequest()), /runBootId/);
  assert.throws(() => parseEventsPage(eventsPage({ runId: 8 }), eventsRequest()), /runId/);
  assert.throws(() => parseEventsPage(eventsPage({ afterRevision: 1 }), eventsRequest()), /afterRevision echo/);
  assert.throws(() => parseEventsPage(eventsPage({ throughRevision: 4 }), eventsRequest()), /throughRevision echo/);
  assert.throws(() => parseEventsPage(eventsPage({
    nextAfterRevision: 2,
    done: false,
  }), eventsRequest()), /nextAfterRevision/);
  assert.throws(() => parseEventsPage(eventsPage({
    events: [],
    nextAfterRevision: 0,
    done: false,
  }), eventsRequest()), /empty events page/);
  assert.throws(() => parseEventsPage(eventsPage(), eventsRequest({ limit: 2 })), /at most 2/);

  const short = parseEventsPage(eventsPage({
    events: [startedEvent(), endedEvent({ revision: 2 })],
    throughRevision: 3,
    nextAfterRevision: 2,
    done: false,
  }), eventsRequest({ throughRevision: 3, limit: 3 }));
  assert.equal(short.nextAfterRevision, 2);
  assert.equal(short.done, false);

  const atWatermark = parseEventsPage(eventsPage({
    afterRevision: 3,
    throughRevision: 3,
    events: [],
    nextAfterRevision: 3,
    done: true,
  }), eventsRequest({ afterRevision: 3, throughRevision: 3 }));
  assert.equal(atWatermark.done, true);
});

// ---------------------------------------------------------------------------
// parseMutationResult
// ---------------------------------------------------------------------------

test('parses persisted marker mutation results', () => {
  const result = parseMutationResult({
    schemaVersion: 1,
    runId: 7,
    operationId: 'op-1',
    id: 'note-1',
    revision: 4,
    persisted: true,
    deleted: false,
  }, { runId: 7, operationId: 'op-1', id: 'note-1' });
  assert.deepEqual(result, {
    schemaVersion: 1,
    runId: 7,
    operationId: 'op-1',
    id: 'note-1',
    revision: 4,
    persisted: true,
    deleted: false,
  });
  assert.equal(parseMutationResult({
    schemaVersion: 1,
    runId: 7,
    operationId: 'op-1',
    id: 'note-1',
    revision: 5,
    persisted: true,
    deleted: true,
  }).deleted, true);
});

test('rejects unpersisted results, bad identities and mismatched echoes', () => {
  const good = {
    schemaVersion: 1,
    runId: 7,
    operationId: 'op-1',
    id: 'note-1',
    revision: 4,
    persisted: true,
    deleted: false,
  };
  assert.throws(() => parseMutationResult({ ...good, persisted: false }), /persisted/);
  assert.throws(() => parseMutationResult({ ...good, deleted: undefined }), ProtocolError);
  assert.throws(() => parseMutationResult({ ...good, revision: 0 }), ProtocolError);
  assert.throws(() => parseMutationResult({ ...good, id: 'bad id' }), /id/);
  assert.throws(() => parseMutationResult({ ...good, schemaVersion: 2 }), ProtocolError);
  assert.throws(() => parseMutationResult(good, { operationId: 'op-2' }), /operationId/);
  assert.throws(() => parseMutationResult(good, { runId: 8 }), /runId/);
  assert.throws(() => parseMutationResult(good, { id: 'note-2' }), /id/);
});

// ---------------------------------------------------------------------------
// normalizeNote
// ---------------------------------------------------------------------------

test('trims ECMAScript whitespace and keeps blank notes valid', () => {
  assert.equal(normalizeNote(' ' + NBSP + 'x' + BOM + LINE_SEP), 'x');
  assert.equal(normalizeNote(TAB + 'hello' + NEWLINE), 'hello');
  assert.equal(normalizeNote('   '), '');
  assert.equal(normalizeNote(IDEOGRAPHIC_SPACE + ' a ' + IDEOGRAPHIC_SPACE), 'a');
});

test('accepts notes at the scalar and UTF-8 byte limits', () => {
  assert.equal(normalizeNote('a'.repeat(500)).length, 500);
  assert.equal(normalizeNote(FIRE.repeat(500)).length, 1000);
  assert.equal(normalizeNote(FIRE.repeat(500)), FIRE.repeat(500));
  assert.equal(normalizeNote(FIRE.repeat(499) + 'a').length, 999);
});

test('rejects notes beyond the scalar limits', () => {
  assert.throws(() => normalizeNote('a'.repeat(501)), /scalar values/);
  assert.throws(() => normalizeNote(FIRE.repeat(501)), /scalar values/);
  assert.throws(() => normalizeNote('  ' + 'a'.repeat(501)), /scalar values/);
  assert.equal(normalizeNote('  ' + 'a'.repeat(500)).length, 500);
});

test('rejects NUL, lone surrogates and non-string notes', () => {
  assert.throws(() => normalizeNote('a' + NUL + 'b'), /NUL/);
  assert.throws(() => normalizeNote(NUL), /NUL/);
  assert.throws(() => normalizeNote(HIGH_SURROGATE), /surrogates/);
  assert.throws(() => normalizeNote('x' + LOW_SURROGATE + 'y'), /surrogates/);
  assert.throws(() => normalizeNote(42), /string/);
  assert.throws(() => normalizeNote(null), /string/);
  assert.throws(() => normalizeNote(undefined), /string/);
});

test('rejects undefined raw flag bits before exposing recorded analysis', () => {
  assert.throws(() => parseSamplesPage(samplesPage({
    samples: [{ ...smokeSamples()[0], flags: 4 }], nextAfterLogSeq: 1, done: false,
  }), samplesRequest()), /flags/);
});
