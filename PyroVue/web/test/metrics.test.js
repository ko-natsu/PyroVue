'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');

require('../src/js/metrics.js');

const { calculateRateOfRise, segmentRateRegions } = globalThis.PyroVue;

/** Raw canonical point with a healthy fault mask. */
function raw(ms, tempC, overrides = {}) {
  return { kind: 'raw', runId: 1, seq: overrides.seq ?? ms / 1000, ms, tempC, fault: 0, ...overrides };
}

/** Coarse canonical bucket with a healthy fault mask. */
function coarse(ms, averageC, overrides = {}) {
  return {
    kind: 'coarse', runId: 1, ms, resolutionMs: 30000,
    averageC, minimumC: averageC - 5, maximumC: averageC + 5, fault: 0, ...overrides,
  };
}

/** Uniform ramp: temp = base + rateCPerSec * (ms / 1000). */
function ramp(count, stepMs, rateCPerSec, base = 20) {
  const points = [];
  for (let i = 0; i < count; i += 1) {
    points.push(raw(i * stepMs, base + rateCPerSec * ((i * stepMs) / 1000), { seq: i }));
  }
  return points;
}

/**
 * Independent least-squares slope, written differently from the
 * implementation, used as a cross-check for irregular sampling.
 */
function referenceSlopeCPerHour(samples) {
  const xBar = samples.reduce((acc, s) => acc + s.ms, 0) / samples.length;
  const yBar = samples.reduce((acc, s) => acc + s.tempC, 0) / samples.length;
  let num = 0;
  let den = 0;
  for (const s of samples) {
    num += (s.ms - xBar) * (s.tempC - yBar);
    den += (s.ms - xBar) ** 2;
  }
  return (num / den) * 3600000;
}

function close(actual, expected, message, epsilon = 1e-6) {
  assert.ok(
    Math.abs(actual - expected) <= epsilon * Math.max(1, Math.abs(expected)),
    `${message}: expected ~${expected}, got ${actual}`,
  );
}

test('positive ramp yields the exact regression rate', () => {
  // 1 °C every 10 s => 360 °C/hr.
  const result = calculateRateOfRise(ramp(13, 10000, 0.1));
  close(result.rateCPerHour, 360, 'rate of rise');
  assert.equal(result.quality, 'good');
  assert.equal(result.sampleCount, 13);
  assert.equal(result.startMs, 0);
  assert.equal(result.endMs, 120000);
  assert.equal(result.windowMs, 120000);
});

test('flat temperature yields zero rate', () => {
  const result = calculateRateOfRise(ramp(13, 10000, 0));
  assert.equal(result.rateCPerHour, 0);
  assert.equal(result.quality, 'good');
  assert.equal(result.sampleCount, 13);
});

test('negative ramp yields a negative rate', () => {
  const result = calculateRateOfRise(ramp(13, 10000, -0.1, 200));
  close(result.rateCPerHour, -360, 'cooling rate');
  assert.equal(result.quality, 'good');
});

test('irregular sampling matches an independent least-squares solution', () => {
  const steps = [0, 5000, 12000, 30000, 47000, 71000, 90000];
  const points = steps.map((ms, i) => raw(ms, 100 + 0.05 * (ms / 1000), { seq: i }));
  const result = calculateRateOfRise(points);
  // 0.05 °C/s => 180 °C/hr.
  close(result.rateCPerHour, referenceSlopeCPerHour(points), 'irregular regression');
  close(result.rateCPerHour, 180, 'expected slope');
  // Span 90 s of a 120 s window: fair coverage, not good.
  assert.equal(result.quality, 'fair');
  assert.equal(result.sampleCount, 7);
});

test('windowed regression resists an outlier that defeats adjacent deltas', () => {
  const points = ramp(13, 10000, 0.1);
  // Single +200 °C spike exactly at the window's temporal center: an
  // outlier at the mean x contributes zero to the least-squares slope.
  points[6] = raw(60000, points[6].tempC + 200, { seq: 6 });

  const result = calculateRateOfRise(points);
  close(result.rateCPerHour, 360, 'rate unchanged by centered outlier');

  // Adjacent-sample deltas across the spike explode instead.
  let maxDelta = 0;
  for (let i = 1; i < points.length; i += 1) {
    const delta = ((points[i].tempC - points[i - 1].tempC) / (points[i].ms - points[i - 1].ms)) * 3600000;
    maxDelta = Math.max(maxDelta, Math.abs(delta));
  }
  assert.ok(maxDelta > 36000, `naive delta should explode, got ${maxDelta}`);
  assert.ok(Math.abs(result.rateCPerHour - 360) < Math.abs(maxDelta - 360) / 100);
});

test('faulted and non-finite samples are rejected', () => {
  const points = ramp(13, 10000, 0.1);
  points[3] = raw(30000, 999, { fault: 1 }); // open thermocouple bit
  points[7] = raw(70000, Number.NaN, { fault: 0 }); // non-finite reading
  points[9] = raw(90000, 29, { fault: 8 }); // any non-zero mask is faulted
  points[11] = { ...raw(110000, 31), fault: null, tempC: Number.NaN };

  const result = calculateRateOfRise(points);
  assert.equal(result.sampleCount, 9); // 13 minus the four unusable points
  assert.equal(result.endMs, 120000); // last usable sample, not the faulted one
  close(result.rateCPerHour, 360, 'rate from usable points only');
});

test('insufficient data returns an unavailable result', () => {
  assert.deepEqual({ ...calculateRateOfRise([]) }, {
    rateCPerHour: null, quality: 'unavailable', sampleCount: 0, startMs: null, endMs: null, windowMs: 120000,
  });

  const single = calculateRateOfRise([raw(0, 20)]);
  assert.equal(single.rateCPerHour, null);
  assert.equal(single.quality, 'unavailable');
  assert.equal(single.sampleCount, 1);

  // Two points only 5 s apart: enough samples, not enough duration.
  const short = calculateRateOfRise([raw(0, 20), raw(5000, 21)]);
  assert.equal(short.rateCPerHour, null);
  assert.equal(short.quality, 'unavailable');
  assert.equal(short.sampleCount, 2);
  assert.equal(short.startMs, 0);
  assert.equal(short.endMs, 5000);
});

test('all-invalid input reports nothing usable', () => {
  const result = calculateRateOfRise([
    raw(0, 20, { fault: 2 }),
    raw(10000, Number.NaN),
    { kind: 'coarse', runId: 1, ms: 20000, resolutionMs: 30000, averageC: Number.NaN, minimumC: 0, maximumC: 0, fault: 0 },
  ]);
  assert.equal(result.rateCPerHour, null);
  assert.equal(result.quality, 'unavailable');
  assert.equal(result.sampleCount, 0);
  assert.equal(result.startMs, null);
  assert.equal(result.endMs, null);
});

test('custom window restricts the regression to recent samples', () => {
  const points = ramp(31, 10000, 0.1); // 0..300 s
  const result = calculateRateOfRise(points, { windowMs: 60000 });
  assert.equal(result.windowMs, 60000);
  assert.equal(result.sampleCount, 7); // 240 s .. 300 s inclusive
  assert.equal(result.startMs, 240000);
  assert.equal(result.endMs, 300000);
  close(result.rateCPerHour, 360, 'rate inside the custom window');

  // A window longer than the data span simply uses everything available.
  const wide = calculateRateOfRise(points, { windowMs: 600000 });
  assert.equal(wide.sampleCount, 31);
  close(wide.rateCPerHour, 360, 'rate over the full span');
});

test('equal timestamps deduplicate deterministically', () => {
  // Raw overrides coarse at the same timestamp.
  const mixed = calculateRateOfRise([coarse(60000, 90), raw(60000, 100)]);
  assert.equal(mixed.sampleCount, 1);
  // slope from one point is unavailable; only dedup outcome matters below.
  assert.equal(mixed.quality, 'unavailable');

  // Prove raw won by pairing it with earlier raw points.
  const decided = calculateRateOfRise([
    raw(0, 90),
    raw(30000, 95),
    coarse(60000, 500),
    raw(60000, 100),
  ]);
  // 90 -> 100 over 60 s would be 600 °C/hr; the coarse 500 would give 24600.
  close(decided.rateCPerHour, 600, 'raw sample won the duplicate timestamp');
  assert.equal(decided.sampleCount, 3);

  // Newer raw sequence wins over an older raw sequence at the same ms.
  const seqDecided = calculateRateOfRise([
    raw(0, 90),
    raw(30000, 95),
    raw(60000, 500, { seq: 5 }),
    raw(60000, 100, { seq: 6 }),
  ]);
  close(seqDecided.rateCPerHour, 600, 'newer seq won the duplicate timestamp');

  // Finer coarse resolution wins between coarse buckets at the same ms.
  const resDecided = calculateRateOfRise([
    raw(0, 90),
    raw(30000, 95),
    coarse(60000, 500, { resolutionMs: 60000 }),
    coarse(60000, 100, { resolutionMs: 30000 }),
  ]);
  close(resDecided.rateCPerHour, 600, 'finer coarse resolution won');
});

test('regions are contiguous and carry per-region rates', () => {
  const points = ramp(25, 10000, 0.1); // 0..240 s
  const regions = segmentRateRegions(points);
  assert.equal(regions.length, 4);
  assert.equal(regions[0].startMs, 0);
  assert.equal(regions[3].endMs, 240000);
  for (let i = 1; i < regions.length; i += 1) {
    assert.equal(regions[i].startMs, regions[i - 1].endMs, 'contiguous boundary');
  }
  // Final region is closed, so it also owns the point at exactly 240 s.
  assert.deepEqual(regions.map((r) => r.sampleCount), [6, 6, 6, 7]);
  // Interior regions cover 50 s of their 60 s span (fair); the closed final
  // region covers its full 60 s (good).
  assert.deepEqual(regions.map((r) => r.quality), ['fair', 'fair', 'fair', 'good']);
  for (const region of regions) {
    close(region.rateCPerHour, 360, 'per-region rate');
    assert.deepEqual(Object.keys(region), ['startMs', 'endMs', 'rateCPerHour', 'quality', 'sampleCount']);
  }
});

test('gaps are represented honestly, not interpolated', () => {
  const points = [
    ...ramp(7, 10000, 0.1), // 0..60 s
    ...ramp(7, 10000, 0.1, 26).map((p) => ({ ...p, ms: p.ms + 180000, seq: p.seq + 20 })), // 180..240 s
  ];
  const regions = segmentRateRegions(points);
  assert.equal(regions.length, 4);
  const gapRegions = regions.filter((r) => r.quality === 'unavailable');
  assert.equal(gapRegions.length, 2);
  for (const region of gapRegions) {
    assert.equal(region.rateCPerHour, null);
  }
  assert.equal(regions[0].sampleCount, 6); // point at 60 s falls in the next region
  assert.equal(regions[1].sampleCount, 1); // lone boundary point cannot trend
  assert.equal(regions[2].sampleCount, 0); // full gap
  assert.equal(regions[3].sampleCount, 7);
  close(regions[0].rateCPerHour, 360, 'leading region still regresses');
  close(regions[3].rateCPerHour, 360, 'trailing region still regresses');
  for (let i = 1; i < regions.length; i += 1) {
    assert.equal(regions[i].startMs, regions[i - 1].endMs, 'contiguity across the gap');
  }
});

test('custom segment size and trailing-sliver merge stay contiguous', () => {
  const points = ramp(14, 10000, 0.1); // 0..130 s
  const regions = segmentRateRegions(points, { segmentMs: 30000 });
  // Bounds 0-30, 30-60, 60-90, 90-120, then a 10 s sliver merged into 90-120.
  assert.equal(regions.length, 4);
  assert.equal(regions[3].startMs, 90000);
  assert.equal(regions[3].endMs, 130000);
  assert.equal(regions[3].sampleCount, 5);
  for (let i = 1; i < regions.length; i += 1) {
    assert.equal(regions[i].startMs, regions[i - 1].endMs);
  }
  for (const region of regions) {
    close(region.rateCPerHour, 360, 'custom segment rate');
  }
});

test('coarse points regress on their bucket averages', () => {
  const points = [];
  for (let i = 0; i < 6; i += 1) {
    points.push(coarse(i * 30000, 100 + i * 15)); // 0.5 °C/s => 1800 °C/hr
  }
  const result = calculateRateOfRise(points);
  close(result.rateCPerHour, 1800, 'coarse average drives the rate');
  assert.equal(result.sampleCount, 5); // default 120 s window trims the 0 s bucket

  const regions = segmentRateRegions(points, { segmentMs: 60000 });
  assert.equal(regions.length, 3); // 0-60, 60-120, trailing 30 s kept (>= half segment)
  close(regions[0].rateCPerHour, 1800, 'coarse region rate');
  assert.equal(regions[2].startMs, 120000);
  assert.equal(regions[2].endMs, 150000);
  assert.equal(regions[2].sampleCount, 2);
});

test('metrics work directly on canonical store chart data', () => {
  require('../src/js/protocol.js');
  require('../src/js/state.js');
  const { TelemetryStore } = globalThis.PyroVue;

  const store = new TelemetryStore();
  store.selectRun(7);
  for (let i = 0; i < 13; i += 1) {
    store.mergeRaw({ seq: i, ms: i * 10000, temp: 20 + i, fault: 0 }, 7);
  }
  store.mergeRaw({ seq: 99, ms: 60000, temp: null, fault: 4 }, 7); // faulted duplicate

  const chart = store.getChartData();
  const result = calculateRateOfRise(chart);
  assert.equal(result.sampleCount, 13); // faulted duplicate dropped, timestamps intact
  assert.equal(result.endMs, 120000);
  close(result.rateCPerHour, 360, 'store-fed rate of rise');
});

test('regression remains stable at large run-relative timestamps', () => {
  const offset = 4_000_000_000;
  const expectedRate = 120;
  const samples = Array.from({ length: 6 }, (_, index) => {
    const elapsed = index * 30000;
    return raw(offset + elapsed, 500 + expectedRate * elapsed / 3600000, { seq: index + 1 });
  });
  const result = calculateRateOfRise(samples, {
    windowMs: 180000,
    minSamples: 3,
    minDurationMs: 60000,
  });
  close(result.rateCPerHour, expectedRate, 'large timestamp slope');
});

test('invalid options are rejected', () => {
  assert.throws(() => calculateRateOfRise([], { windowMs: 0 }), TypeError);
  assert.throws(() => calculateRateOfRise([], { windowMs: Number.NaN }), TypeError);
  assert.throws(() => calculateRateOfRise([], { minSamples: 1 }), TypeError);
  assert.throws(() => segmentRateRegions([], { segmentMs: -1 }), TypeError);
});
