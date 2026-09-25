'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');

require('../src/js/protocol.js');
require('../src/js/state.js');

const { AppStore } = globalThis.PyroVue;

function hello(overrides = {}) {
  return {
    type: 'hello',
    protocol: 2,
    firmware: 'rework-2',
    sampleHz: 2,
    runId: 17,
    runActive: true,
    runStartMs: 100,
    recentWindowMs: 600000,
    coarseResolutionMs: 30000,
    snapshotThroughSeq: 2,
    ...overrides,
  };
}

function marker(type, overrides = {}) {
  return {
    type,
    protocol: 2,
    snapshotId: 41,
    runId: 17,
    throughSeq: 2,
    ...overrides,
  };
}

test('assembles replay transaction, buffers catch-up samples, and applies raw precedence', () => {
  let monotonicNow = 1000;
  let wallNow = 1_700_000_000_000;
  const store = new AppStore({ now: () => monotonicNow, wallNow: () => wallNow });
  store.setConnection('websocket', 'awaiting-state');
  store.handle(hello());
  store.handle(marker('history.begin'));

  store.handle({
    type: 'history',
    protocol: 2,
    snapshotId: 41,
    runId: 17,
    kind: 'coarse',
    resolutionMs: 30000,
    chunk: 0,
    final: false,
    samples: [
      { ms: 0, avg: 100, min: 90, max: 110, fault: 0 },
      { ms: 30000, avg: 200, min: 180, max: 220, fault: 0 },
    ],
  });
  store.handle({
    type: 'history',
    protocol: 2,
    snapshotId: 999,
    runId: 17,
    kind: 'raw',
    resolutionMs: 500,
    chunk: 1,
    final: false,
    samples: [{ seq: 99, ms: 500, temp: 999, fault: 0 }],
  });
  store.handle({
    type: 'history',
    protocol: 2,
    snapshotId: 41,
    runId: 17,
    kind: 'raw',
    resolutionMs: 500,
    chunk: 2,
    final: false,
    samples: [{ seq: 1, ms: 31000, temp: 205, fault: 0 }],
  });

  monotonicNow = 1100;
  wallNow += 100;
  store.handle({ type: 'sample', protocol: 2, runId: 17, seq: 2, ms: 31500, temp: 207, fault: 0 });
  assert.equal(store.telemetry.getChartData().length, 0, 'transaction is not published before history.end');
  assert.equal(store.currentReading.sample.seq, 2, 'catch-up sample updates current state immediately');

  store.handle(marker('history.end', { snapshotId: 100 }));
  assert.equal(store.snapshot.active, true, 'mismatched end marker cannot close transaction');
  store.handle(marker('history.end', { throughSeq: 2 }));

  const chart = store.telemetry.getChartData();
  assert.deepEqual(chart.map((point) => [point.kind, point.ms]), [
    ['coarse', 0],
    ['raw', 31000],
    ['raw', 31500],
  ]);
  assert.equal(chart[0].minimumC, 90);
  assert.equal(chart[0].maximumC, 110);
  assert.equal(store.connection.phase, 'live-active');
  assert.equal(store.run.startedAt, wallNow - 31500);
});

test('deduplicates raw samples and never merges different run IDs', () => {
  const store = new AppStore({ now: () => 0, wallNow: () => 100000 });
  store.handle(hello());
  const sample = { type: 'sample', protocol: 2, runId: 17, seq: 1, ms: 500, temp: 25, fault: 0 };
  store.handle({ ...sample, runId: 16 });
  assert.equal(store.currentReading, null, 'mismatched nonzero sample cannot establish a run');
  assert.equal(store.run.id, 17);
  store.handle(sample);
  store.handle(sample);
  assert.equal(store.telemetry.raw.size, 1);

  store.handle(hello({ runId: 18, snapshotThroughSeq: 0 }));
  assert.equal(store.telemetry.raw.size, 0);
  assert.equal(store.telemetry.coarse.size, 0);
  assert.equal(store.run.id, 18);
});

test('publishes replayed raw history as the latest reading when no catch-up arrives', () => {
  const store = new AppStore({ now: () => 1000, wallNow: () => 50000 });
  store.handle(hello());
  store.handle(marker('history.begin'));
  store.handle({
    type: 'history', protocol: 2, snapshotId: 41, runId: 17, kind: 'raw', resolutionMs: 500,
    chunk: 0, final: false, samples: [{ seq: 1, ms: 10000, temp: 400, fault: 0 }],
  });
  store.handle(marker('history.end', { throughSeq: 1 }));
  assert.equal(store.currentReading.sample.tempC, 400);
  assert.equal(store.currentReading.source, 'history');
  assert.equal(store.getFreshness().telemetry, 'stale');
  assert.equal(store.run.elapsedMs, 10000);
});

test('reports connection, freshness, and sensor validity independently', () => {
  let now = 0;
  const store = new AppStore({ now: () => now, wallNow: () => 100000 + now });
  store.setConnection('websocket', 'live-active');
  store.handle(hello());
  store.handle({ type: 'sample', protocol: 2, runId: 17, seq: 1, ms: 500, temp: null, fault: 9 });

  assert.deepEqual(store.getFreshness(), {
    connection: 'live-active',
    telemetry: 'fresh',
    sensor: 'faulted',
    ageMs: 0,
    faults: {
      mask: 9,
      openThermocouple: true,
      shortToGround: false,
      shortToVcc: false,
      nonFinite: true,
      unknownMask: 0,
    },
  });

  now = 4001;
  assert.equal(store.getFreshness().telemetry, 'stale');
  store.handle({ type: 'state', protocol: 2, runId: 17, runActive: false });
  assert.equal(store.getFreshness().telemetry, 'stale', 'aged live reading stays stale after stop');
  store.setConnection('none', 'disconnected');
  assert.equal(store.getFreshness().connection, 'disconnected');
});

test('idle live readings refresh independently without polluting retained run telemetry', () => {
  let now = 10;
  const store = new AppStore({ now: () => now, wallNow: () => 100000 + now });
  store.handle(hello());
  store.handle({ type: 'sample', protocol: 2, runId: 17, seq: 1, ms: 500, temp: 25, fault: 0 });
  const chartBeforeStop = store.telemetry.getChartData();
  const startedAt = store.run.startedAt;
  const elapsedMs = store.run.elapsedMs;
  store.handle({ type: 'state', protocol: 2, runId: 17, runActive: false });
  now = 20;
  store.handle({ type: 'sample', protocol: 2, runId: 0, seq: 0, ms: 0, temp: 26, fault: 0 });
  now = 30;
  store.handle({ type: 'sample', protocol: 2, runId: 0, seq: 0, ms: 0, temp: null, fault: 1 });

  assert.equal(store.currentReading.sample.tempC, Number.NaN);
  assert.equal(store.currentReading.sample.fault, 1);
  assert.equal(store.currentReading.receivedAt, 30);
  assert.equal(store.currentReading.source, 'live');
  assert.equal(Object.isFrozen(store.currentReading), true);
  assert.equal(store.getFreshness().telemetry, 'fresh');
  assert.equal(store.getFreshness().sensor, 'faulted');
  assert.equal(store.run.id, 17);
  assert.equal(store.run.elapsedMs, elapsedMs);
  assert.equal(store.run.startedAt, startedAt);
  assert.deepEqual(store.telemetry.getChartData(), chartBeforeStop);
  assert.equal(store.telemetry.raw.size, 1);

  store.handle({ type: 'state', protocol: 2, runId: 18, runActive: true });
  assert.equal(store.run.id, 18);
  assert.equal(store.currentReading, null);
  assert.equal(store.telemetry.getChartData().length, 0);
  store.handle({ type: 'sample', protocol: 2, runId: 18, seq: 1, ms: 100, temp: 30, fault: 0 });
  assert.equal(store.currentReading.source, 'live');
  assert.equal(store.currentReading.sample.runId, 18);
  assert.equal(store.run.elapsedMs, 100);
  assert.equal(store.telemetry.raw.size, 1);
});
