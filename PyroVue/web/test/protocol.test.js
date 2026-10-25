'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');

require('../src/js/protocol.js');

const {
  PROTOCOL_VERSION,
  ProtocolError,
  parseMessage,
  parseError,
  parseCommandResult,
  decodeFaults,
  createOperationId,
  createCommand,
} = globalThis.PyroVue;

const DEVICE_ID = 'a1b2c3d4e5f6';
const BOOT_ID = '0123456789abcdef';

function hello(overrides = {}) {
  return {
    type: 'hello',
    protocol: 3,
    firmware: 'run-logging-1',
    sampleHz: 2,
    runId: 17,
    runActive: true,
    deviceId: DEVICE_ID,
    bootId: BOOT_ID,
    capabilities: { runArchive: true },
    acquireIntervalMs: 500,
    logIntervalMs: 2000,
    maxRunMs: 86400000,
    logging: {
      state: 'recording',
      durableLogSeq: 12,
      latestAcqSeq: 13,
      bufferedRecords: 1,
      acquisitionGaps: 0,
      droppedAcquired: 0,
      queueHighWater: 4,
      errorCode: null,
    },
    storage: {
      mounted: true,
      totalBytes: 8192000,
      usedBytes: 4096,
      freeBytes: 8187904,
      reservedRemainingBytes: 32768,
      startRequiredBytes: 2416640,
    },
    runStartMs: 100,
    recentWindowMs: 600000,
    coarseResolutionMs: 30000,
    snapshotThroughSeq: 42,
    preset: 'slow',
    ...overrides,
  };
}

function commandResult(overrides = {}) {
  return {
    type: 'command.result',
    protocol: 3,
    requestId: 'req-start_1',
    ok: true,
    runId: 18,
    runActive: true,
    targetRunId: 18,
    durable: true,
    ...overrides,
  };
}

test('parses the full protocol-3 hello profile from websocket and /api/info', () => {
  const full = parseMessage(JSON.stringify(hello()));
  assert.equal(full.protocol, PROTOCOL_VERSION);
  assert.equal(full.deviceId, DEVICE_ID);
  assert.equal(full.bootId, BOOT_ID);
  assert.equal(full.capabilities.runArchive, true);
  assert.equal(full.logging.state, 'recording');
  assert.equal(full.storage.mounted, true);

  const info = parseMessage(hello({ runActive: false, clients: 0 }));
  assert.equal(info.runActive, false);
  assert.equal(info.runId, 17);

  const monitor = parseMessage(hello({
    capabilities: { runArchive: false },
    logIntervalMs: 0,
    maxRunMs: 0,
    logging: {
      state: 'unavailable',
      durableLogSeq: 0,
      latestAcqSeq: 0,
      bufferedRecords: 0,
      acquisitionGaps: 0,
      droppedAcquired: 0,
      queueHighWater: 0,
      errorCode: 'ARCHIVE_UNAVAILABLE',
    },
    storage: {
      mounted: false,
      totalBytes: 0,
      usedBytes: 0,
      freeBytes: 0,
      reservedRemainingBytes: 0,
      startRequiredBytes: 0,
    },
  }));
  assert.equal(monitor.capabilities.runArchive, false);
});

test('requires hello identity, capabilities, intervals, logging, and storage fields', () => {
  for (const field of [
    'firmware', 'sampleHz', 'runId', 'runActive', 'deviceId', 'bootId',
    'capabilities', 'acquireIntervalMs', 'logIntervalMs', 'maxRunMs',
    'logging', 'storage',
  ]) {
    const value = hello();
    delete value[field];
    assert.throws(() => parseMessage(value), ProtocolError, `${field} is required`);
  }

  assert.throws(() => parseMessage(hello({ deviceId: 'A1B2C3D4E5F6' })), /deviceId/);
  assert.throws(() => parseMessage(hello({ deviceId: 'a1b2c3d4e5' })), /deviceId/);
  assert.throws(() => parseMessage(hello({ bootId: '0123456789ABCDEF' })), /bootId/);
  assert.throws(() => parseMessage(hello({ acquireIntervalMs: 0 })), /acquireIntervalMs/);
  assert.throws(() => parseMessage(hello({ logIntervalMs: -1 })), /logIntervalMs/);
  assert.throws(() => parseMessage(hello({ maxRunMs: 1.5 })), /maxRunMs/);

  const noCapability = hello();
  delete noCapability.capabilities.runArchive;
  assert.throws(() => parseMessage(noCapability), /runArchive/);

  assert.throws(() => parseMessage(hello({
    logging: { ...hello().logging, state: 'paused' },
  })), /logging\.state/);
  assert.throws(() => parseMessage(hello({
    logging: { ...hello().logging, durableLogSeq: -1 },
  })), /durableLogSeq/);

  const noMounted = hello();
  delete noMounted.storage.mounted;
  assert.throws(() => parseMessage(noMounted), /mounted/);
  assert.throws(() => parseMessage(hello({
    storage: { ...hello().storage, usedBytes: -1 },
  })), /usedBytes/);
});

test('rejects protocol 2 and accepts null faulted temperatures unchanged', () => {
  assert.throws(() => parseMessage(hello({ protocol: 2 })), /unsupported protocol 2/);
  const sample = parseMessage({
    type: 'sample',
    protocol: 3,
    runId: 4,
    seq: 1,
    ms: 500,
    temp: null,
    fault: 9,
  });
  assert.equal(sample.temp, null);
  assert.equal(sample.fault, 9);
});

test('accepts only the exact idle zero-sequence sample identity', () => {
  assert.deepEqual(parseMessage({
    type: 'sample', protocol: 3, runId: 0, seq: 0, ms: 0, temp: null, fault: 1,
  }), {
    type: 'sample', protocol: 3, runId: 0, seq: 0, ms: 0, temp: null, fault: 1,
  });
  for (const identity of [
    { runId: 0, seq: 0, ms: 1 },
    { runId: 0, seq: 1, ms: 0 },
    { runId: 1, seq: 0, ms: 0 },
  ]) {
    assert.throws(() => parseMessage({
      type: 'sample', protocol: 3, ...identity, temp: 20, fault: 0,
    }), ProtocolError);
  }
});

test('rejects malformed, unsupported, and unknown messages', () => {
  assert.throws(() => parseMessage('{'), ProtocolError);
  assert.throws(() => parseMessage({ type: 'sample', protocol: 1 }), /unsupported protocol 1/);
  assert.throws(() => parseMessage({ type: 'mystery', protocol: 3 }), /unknown message type/);
  assert.throws(() => parseMessage({
    type: 'error', protocol: 3, code: 'BUSY',
  }), /message/);
  assert.throws(() => parseMessage({
    type: 'sample', protocol: 3, runId: 1, seq: 1, ms: 0, temp: 20, fault: 999,
  }), /fault must fit in one byte/);
  assert.throws(() => parseMessage({
    type: 'command.result', protocol: 3, requestId: 'x', ok: true,
    runId: 1, runActive: true, targetRunId: 1, durable: true,
  }), /unknown message type/);
});

test('validates strict error and command.result bodies', () => {
  assert.deepEqual(parseError({
    type: 'error', protocol: 3, code: 'STALE_COMMAND', message: 'stale',
    requestId: 'req_1',
  }, 'req_1'), {
    type: 'error', protocol: 3, code: 'STALE_COMMAND', message: 'stale',
    requestId: 'req_1',
  });
  assert.throws(() => parseError({
    type: 'error', protocol: 3, code: 'STALE_COMMAND', message: 'stale',
    requestId: 'req_2',
  }, 'req_1'), /does not match/);
  assert.throws(() => parseError({
    type: 'error', protocol: 2, code: 'STALE_COMMAND', message: 'stale',
  }), /unsupported protocol/);

  const result = parseCommandResult(commandResult(), { requestId: 'req-start_1', targetRunId: 18 });
  assert.equal(result.runId, 18);
  assert.equal(result.durable, true);
  assert.throws(() => parseCommandResult(commandResult(), { requestId: 'other' }), /does not match/);
  assert.throws(() => parseCommandResult(commandResult(), {
    requestId: 'req-start_1', targetRunId: 17,
  }), /targetRunId/);
  assert.throws(() => parseCommandResult(commandResult({ durable: undefined })), /durable/);
  assert.throws(() => parseCommandResult(commandResult({ runActive: undefined })), /runActive/);
  assert.throws(() => parseCommandResult(commandResult({ protocol: 2 })), /unsupported protocol/);
  assert.throws(() => parseCommandResult(commandResult({
    runId: 18, targetRunId: 18, runActive: true,
  }), { requestId: 'req-start_1', targetRunId: 18, inactiveTarget: true }), /contradicts/);
  assert.deepEqual(parseCommandResult({
    type: 'command.result', protocol: 3, requestId: 'req_1', ok: false, errorCode: 'BUSY',
  }, { requestId: 'req_1' }).errorCode, 'BUSY');
});

test('decodes individual, combined, and unknown fault bits', () => {
  assert.deepEqual(decodeFaults(1), {
    mask: 1,
    openThermocouple: true,
    shortToGround: false,
    shortToVcc: false,
    nonFinite: false,
    unknownMask: 0,
  });
  const combined = decodeFaults(15);
  assert.equal(combined.openThermocouple, true);
  assert.equal(combined.shortToGround, true);
  assert.equal(combined.shortToVcc, true);
  assert.equal(combined.nonFinite, true);
  assert.equal(decodeFaults(0x20).unknownMask, 0x20);
});

test('creates exact command bodies from the authoritative context', () => {
  const context = {
    requestId: 'req_start-1',
    bootId: BOOT_ID,
    runId: 17,
    wallStartUtcMs: 1700000000000,
  };
  assert.deepEqual(createCommand('run.start', context), {
    type: 'run.start',
    requestId: 'req_start-1',
    bootId: BOOT_ID,
    afterRunId: 17,
    wallStartUtcMs: 1700000000000,
  });
  assert.deepEqual(createCommand('run.stop', context), {
    type: 'run.stop',
    requestId: 'req_start-1',
    bootId: BOOT_ID,
    runId: 17,
  });
  assert.deepEqual(createCommand('preset', context, 'slow'), {
    type: 'preset',
    requestId: 'req_start-1',
    bootId: BOOT_ID,
    value: 'slow',
  });

  assert.throws(() => createCommand('preset', context), /invalid command preset/);
  assert.throws(() => createCommand('run.pause', context), /invalid command/);
  assert.throws(() => createCommand('run.start', undefined), /command context/);
  assert.throws(() => createCommand('run.start', { ...context, requestId: '' }), /requestId/);
  assert.throws(() => createCommand('run.start', { ...context, requestId: 'bad id' }), /requestId/);
  assert.throws(() => createCommand('run.start', { ...context, bootId: 'not-a-boot' }), /bootId/);
  assert.throws(() => createCommand('run.start', { ...context, runId: -1 }), /runId/);
});

test('createOperationId emits supported request identities', () => {
  const ids = new Set();
  for (let index = 0; index < 32; index += 1) {
    const id = createOperationId();
    assert.match(id, /^[A-Za-z0-9_-]{1,80}$/);
    ids.add(id);
  }
  assert.equal(ids.size, 32);
});
