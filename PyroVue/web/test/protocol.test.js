'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');

require('../src/js/protocol.js');

const { PROTOCOL_VERSION, ProtocolError, parseMessage, decodeFaults, createCommand } = globalThis.PyroVue;

test('parses full websocket hello and reduced HTTP info hello', () => {
  const full = parseMessage(JSON.stringify({
    type: 'hello',
    protocol: 2,
    firmware: 'rework-2',
    sampleHz: 2,
    runId: 17,
    runActive: true,
    runStartMs: 100,
    recentWindowMs: 600000,
    coarseResolutionMs: 30000,
    snapshotThroughSeq: 42,
  }));
  assert.equal(full.protocol, PROTOCOL_VERSION);
  assert.equal(full.runId, 17);

  const reduced = parseMessage({
    type: 'hello',
    protocol: 2,
    firmware: 'rework-2',
    sampleHz: 2,
    runId: 17,
    runActive: false,
    clients: 0,
    preset: 'slow',
  });
  assert.equal(reduced.runActive, false);
});

test('accepts null faulted temperatures without converting them to zero', () => {
  const sample = parseMessage({
    type: 'sample',
    protocol: 2,
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
    type: 'sample', protocol: 2, runId: 0, seq: 0, ms: 0, temp: null, fault: 1,
  }), {
    type: 'sample', protocol: 2, runId: 0, seq: 0, ms: 0, temp: null, fault: 1,
  });
  for (const identity of [
    { runId: 0, seq: 0, ms: 1 },
    { runId: 0, seq: 1, ms: 0 },
    { runId: 1, seq: 0, ms: 0 },
  ]) {
    assert.throws(() => parseMessage({
      type: 'sample', protocol: 2, ...identity, temp: 20, fault: 0,
    }), ProtocolError);
  }
});

test('rejects malformed, unsupported, and unknown messages', () => {
  assert.throws(() => parseMessage('{'), ProtocolError);
  assert.throws(() => parseMessage({ type: 'sample', protocol: 1 }), /unsupported protocol 1/);
  assert.throws(() => parseMessage({ type: 'mystery', protocol: 2 }), /unknown message type/);
  assert.throws(() => parseMessage({
    type: 'sample', protocol: 2, runId: 1, seq: 1, ms: 0, temp: 20, fault: 999,
  }), /fault must fit in one byte/);
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

test('creates only supported command payloads', () => {
  assert.deepEqual(createCommand('run.start'), { type: 'run.start' });
  assert.deepEqual(createCommand('run.stop'), { type: 'run.stop' });
  assert.deepEqual(createCommand('preset', 'slow'), { type: 'preset', value: 'slow' });
  assert.throws(() => createCommand('preset'), /invalid command/);
});
