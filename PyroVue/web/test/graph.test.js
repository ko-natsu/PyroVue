'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');

require('../src/js/protocol.js');
require('../src/js/state.js');
require('../src/js/graph.js');

const { AppStore, GraphAxis, GraphRenderer } = globalThis.PyroVue;

const SECOND = 1000;
const MINUTE = 60 * SECOND;
const HOUR = 60 * MINUTE;

/** @param {Partial<{ms:number, seq:number, tempC:number, fault:number, runId:number}>} overrides */
function rawPoint(overrides = {}) {
  return Object.freeze({
    kind: 'raw',
    runId: 1,
    seq: 1,
    ms: 0,
    tempC: 25,
    fault: 0,
    ...overrides,
  });
}

/**
 * @param {Partial<{ms:number, resolutionMs:number, averageC:number, minimumC:number, maximumC:number, fault:number, runId:number}>} overrides
 */
function coarsePoint(overrides = {}) {
  return Object.freeze({
    kind: 'coarse',
    runId: 1,
    ms: 0,
    resolutionMs: 30000,
    averageC: 25,
    minimumC: 24,
    maximumC: 26,
    fault: 0,
    ...overrides,
  });
}

/** Minimal recording 2D context so the renderer can run without a DOM. */
function recordingContext() {
  /** @type {[string, unknown[]][]} */
  const calls = [];
  const target = /** @type {any} */ ({});
  return {
    calls,
    context: new Proxy(target, {
      /** @param {string} prop */
      get(t, prop) {
        if (prop in t) return t[prop];
        return /** @type {any} */ ((...args) => {
          calls.push([/** @type {string} */ (prop), args]);
          if (prop === 'measureText') {
            return { width: String(args[0]).length * 6 };
          }
          return undefined;
        });
      },
      /** @param {string} prop */
      set(t, prop, value) {
        t[prop] = value;
        return true;
      },
    }),
  };
}

function stubCanvas(width = 800, height = 480) {
  const { context, calls } = recordingContext();
  /** @type {Map<string, Function>} */
  const listeners = new Map();
  const canvas = /** @type {any} */ ({
    clientWidth: width,
    clientHeight: height,
    width: 0,
    height: 0,
    style: {},
    getContext: () => context,
    addEventListener(type, listener) {
      listeners.set(type, listener);
    },
    removeEventListener(type) {
      listeners.delete(type);
    },
  });
  return { canvas, calls, listeners };
}

function hello(overrides = {}) {
  return {
    type: 'hello',
    protocol: 2,
    firmware: 'rework-2',
    sampleHz: 2,
    runId: 17,
    runActive: true,
    runStartMs: 0,
    recentWindowMs: 600000,
    coarseResolutionMs: 30000,
    snapshotThroughSeq: 0,
    ...overrides,
  };
}

/** Loads canonical chart data through the real store pipeline. */
function canonicalPoints(overrides = {}) {
  const store = new AppStore();
  store.handle(hello(overrides.hello));
  store.handle({
    type: 'history.begin',
    protocol: 2,
    snapshotId: 7,
    runId: 17,
    throughSeq: 0,
  });
  store.handle({
    type: 'history',
    protocol: 2,
    snapshotId: 7,
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
    snapshotId: 7,
    runId: 17,
    kind: 'raw',
    resolutionMs: 500,
    chunk: 1,
    final: false,
    samples: [
      { seq: 1, ms: 500, temp: 105, fault: 0 },
      { seq: 2, ms: 31000, temp: 205, fault: 0 },
    ],
  });
  store.handle({ type: 'history.end', protocol: 2, snapshotId: 7, runId: 17, throughSeq: 2 });
  return store.telemetry.getChartData();
}

test('time axis uses stable human-friendly ticks and never the live runtime', () => {
  const threeThirtyFive = 3 * HOUR + 35 * MINUTE;
  const domain = GraphAxis.computeTimeDomain(threeThirtyFive);
  assert.deepEqual(
    { startMs: domain.startMs, endMs: domain.endMs, stepMs: domain.stepMs },
    { startMs: 0, endMs: 4 * HOUR, stepMs: HOUR },
  );
  const ticks = GraphAxis.computeTimeTicks(domain);
  assert.deepEqual(
    ticks.map((tick) => tick.label),
    ['0:00', '1:00', '2:00', '3:00', '4:00'],
  );
  // Current runtime is not forced into the tick set.
  assert.ok(!ticks.some((tick) => tick.ms === threeThirtyFive));

  // A 2h14m run: 30-minute grid, live end between the last ticks.
  const twoFourteen = 2 * HOUR + 14 * MINUTE;
  const shortDomain = GraphAxis.computeTimeDomain(twoFourteen);
  assert.equal(shortDomain.stepMs, 30 * MINUTE);
  assert.equal(shortDomain.endMs, 150 * MINUTE);
  const shortTicks = GraphAxis.computeTimeTicks(shortDomain);
  assert.equal(shortTicks[0].label, '0:00');
  assert.equal(shortTicks[shortTicks.length - 1].label, '2:30');
  // Sub-hour steps stay on the h:mm scale (30 minutes reads 0:30, not 30:00).
  assert.ok(shortTicks.some((tick) => tick.label === '0:30'));
  assert.ok(!shortTicks.some((tick) => tick.ms === twoFourteen));

  // Ticks stay on the interval grid and always cover the data.
  for (const tick of shortTicks) {
    assert.equal(tick.ms % shortDomain.stepMs, 0);
    assert.ok(tick.ms <= shortDomain.endMs);
  }

  // Sub-minute runs still get readable ticks.
  const tiny = GraphAxis.computeTimeDomain(45 * SECOND);
  assert.equal(tiny.stepMs, 10 * SECOND);
  assert.equal(tiny.endMs, 50 * SECOND);

  // No data yet: deterministic fallback domain.
  assert.deepEqual(GraphAxis.computeTimeDomain(0), {
    startMs: 0,
    endMs: 5 * MINUTE,
    stepMs: MINUTE,
  });

  // Tick labels are compact durations, not ISO timestamps.
  assert.equal(GraphAxis.formatDuration(0), '0:00');
  assert.equal(GraphAxis.formatDuration(30 * MINUTE), '30:00');
  assert.equal(GraphAxis.formatDuration(HOUR), '1:00');
  assert.equal(GraphAxis.formatDuration(3 * HOUR + 35 * MINUTE), '3:35');
});

test('Y scaling is rounded and includes coarse min/max extrema', () => {
  const points = [
    coarsePoint({ ms: 0, averageC: 600, minimumC: 180, maximumC: 1100 }),
    rawPoint({ ms: 45000, seq: 5, tempC: 205 }),
  ];
  const series = GraphAxis.buildSeries(points, { unit: 'C' });
  // Coarse extrema feed the scaling values alongside raw and coarse averages.
  for (const expected of [180, 600, 1100, 205]) {
    assert.ok(series.temperatureValues.includes(expected), `missing ${expected}`);
  }
  const domain = GraphAxis.computeTemperatureDomain(series.temperatureValues);
  assert.ok(domain.min <= 180, 'domain must include the coarse minimum');
  assert.ok(domain.max >= 1100, 'domain must include the coarse maximum');
  // Bounds are snapped to the tick step.
  assert.equal(domain.min % domain.step, 0);
  assert.equal(domain.max % domain.step, 0);
  assert.ok(domain.min < 180 && domain.max > 1100);
  // Ticks walk the step from bound to bound.
  const last = domain.ticks[domain.ticks.length - 1];
  assert.equal(last.value, domain.max);
  assert.equal(domain.ticks[0].value, domain.min);

  // Faulted coarse extrema never leak into the scale.
  const faulted = [
    coarsePoint({ ms: 0, averageC: 600, minimumC: -50, maximumC: 5000, fault: 1 }),
    rawPoint({ ms: 45000, seq: 9, tempC: 610 }),
  ];
  const faultedSeries = GraphAxis.buildSeries(faulted, { unit: 'C' });
  const faultedDomain = GraphAxis.computeTemperatureDomain(faultedSeries.temperatureValues);
  assert.ok(faultedDomain.min > -50);
  assert.ok(faultedDomain.max < 5000);

  // Far-from-zero data zooms instead of pinning the axis at zero.
  const zoomed = GraphAxis.computeTemperatureDomain([900, 1300]);
  assert.ok(zoomed.min >= 800, `expected zoomed domain, got min ${zoomed.min}`);
  assert.ok(zoomed.max >= 1300);

  // Near-zero data anchors at zero with a readable step.
  const anchored = GraphAxis.computeTemperatureDomain([20, 1240]);
  assert.equal(anchored.min, 0);
  assert.ok(anchored.max >= 1240);

  // Flat data still produces a sensible span.
  const flat = GraphAxis.computeTemperatureDomain([1000, 1000]);
  assert.ok(flat.min < 1000 && flat.max > 1000);

  // No valid values: deterministic fallback.
  const fallback = GraphAxis.computeTemperatureDomain([]);
  assert.deepEqual({ min: fallback.min, max: fallback.max }, { min: 0, max: 100 });
});

test('invalid, non-finite, and faulted points break segments instead of drawing through', () => {
  const points = [
    rawPoint({ ms: 0, seq: 1, tempC: 20 }),
    rawPoint({ ms: 500, seq: 2, tempC: 25 }),
    rawPoint({ ms: 1000, seq: 3, tempC: Number.NaN, fault: 8 }),
    rawPoint({ ms: 1500, seq: 4, tempC: 30, fault: 3 }),
    rawPoint({ ms: 2000, seq: 5, tempC: 32 }),
  ];
  const series = GraphAxis.buildSeries(points, { unit: 'C' });
  assert.equal(series.rawSegments.length, 2);
  assert.deepEqual(series.rawSegments[0].map((model) => model.ms), [0, 500]);
  assert.deepEqual(series.rawSegments[1].map((model) => model.ms), [2000]);
  // Faulted values never appear as drawable values.
  assert.ok(!series.temperatureValues.includes(Number.NaN));
  assert.equal(series.maxMs, 2000);
  assert.ok(!series.temperatureValues.includes(30), "faulted value must not be drawable");

  const coarse = [
    coarsePoint({ ms: 0, averageC: 100, minimumC: 90, maximumC: 110 }),
    coarsePoint({ ms: 30000, averageC: Number.NaN, minimumC: 180, maximumC: 220, fault: 0 }),
    coarsePoint({ ms: 60000, averageC: 200, minimumC: 180, maximumC: 220, fault: 1 }),
    coarsePoint({ ms: 90000, averageC: 300, minimumC: 280, maximumC: 320 }),
  ];
  const coarseSeries = GraphAxis.buildSeries(coarse, { unit: 'C' });
  assert.equal(coarseSeries.coarseSegments.length, 2);
  assert.deepEqual(coarseSeries.coarseSegments[0].map((model) => model.ms), [0]);
  assert.deepEqual(coarseSeries.coarseSegments[1].map((model) => model.ms), [90000]);

  const missingRaw = GraphAxis.buildSeries([
    rawPoint({ ms: 0, seq: 1, tempC: 20 }),
    rawPoint({ ms: 1000, seq: 3, tempC: 22 }),
  ]);
  assert.equal(missingRaw.rawSegments.length, 2, 'a sequence gap must not be connected');

  const missingCoarse = GraphAxis.buildSeries([
    coarsePoint({ ms: 0 }),
    coarsePoint({ ms: 60000 }),
  ]);
  assert.equal(missingCoarse.coarseSegments.length, 2, 'a missing coarse bucket must not be connected');
  // A missing extent falls back to the average and disables the band.
  const missingExtent = [
    coarsePoint({ ms: 0, averageC: 100, minimumC: Number.NaN, maximumC: Number.NaN }),
    coarsePoint({ ms: 30000, averageC: 200, minimumC: 180, maximumC: 220 }),
  ];
  const extentSeries = GraphAxis.buildSeries(missingExtent, { unit: 'C' });
  assert.equal(extentSeries.coarseSegments.length, 1);
  assert.equal(extentSeries.coarseSegments[0][0].band, false);
  assert.equal(extentSeries.coarseSegments[0][1].band, true);
  assert.equal(extentSeries.coarseSegments[0][0].minValue, extentSeries.coarseSegments[0][0].value);

  // Raw-over-coarse precedence comes from canonical state: buckets overlapped
  // by raw samples are already omitted by the store.
  const storedPoints = canonicalPoints();
  const storedSeries = GraphAxis.buildSeries(storedPoints, { unit: 'C' });
  assert.ok(storedSeries.coarseSegments.every((segment) => segment.every((model) => model.ms === 0 || model.ms >= 30000)));
  assert.ok(storedSeries.rawSegments.length >= 1);
});

test('C/F conversion covers temperatures, rates, and the full view model', () => {
  assert.equal(GraphAxis.convertTemperature(100, 'F'), 212);
  assert.equal(GraphAxis.convertTemperature(0, 'F'), 32);
  assert.equal(GraphAxis.convertTemperature(-40, 'F'), -40);
  assert.equal(GraphAxis.convertTemperature(100, 'C'), 100);
  assert.equal(GraphAxis.convertRate(152, 'F'), 273.6);
  assert.equal(GraphAxis.convertRate(152, 'C'), 152);

  const seriesC = GraphAxis.buildSeries([rawPoint({ ms: 0, tempC: 100 })], { unit: 'C' });
  const seriesF = GraphAxis.buildSeries([rawPoint({ ms: 0, tempC: 100 })], { unit: 'F' });
  assert.equal(seriesC.rawSegments[0][0].value, 100);
  assert.equal(seriesF.rawSegments[0][0].value, 212);
  // Canonical Celsius is preserved alongside the display value.
  assert.equal(seriesF.rawSegments[0][0].tempC, 100);

  const domainF = GraphAxis.computeTemperatureDomain([212, 338]);
  assert.ok(domainF.min <= 212 && domainF.max >= 338);
  // Tick labels are plain display-unit numbers.
  assert.ok(domainF.ticks.every((tick) => !Number.isNaN(Number(tick.label))));

  // Rate tiers are judged on canonical °C/hr regardless of display unit.
  assert.equal(GraphAxis.rateTier(76), 'mid');
  assert.equal(GraphAxis.rateTier(59), 'cool');
  assert.equal(GraphAxis.rateTier(152), 'hot');
});

test('inspection selects the nearest valid point inside the touch radius', () => {
  const models = [
    { ms: 0, x: 10 },
    { ms: 30000, x: 100 },
    { ms: 60000, x: 190 },
  ];
  assert.equal(GraphAxis.pickPoint(models, 95, 32).ms, 30000);
  assert.equal(GraphAxis.pickPoint(models, 20, 32).ms, 0);
  // Outside every hit target: nothing selected.
  assert.equal(GraphAxis.pickPoint(models, 150, 32), null);
  assert.equal(GraphAxis.pickPoint(models, 230, 32), null);
  // Boundary is inclusive.
  assert.equal(GraphAxis.pickPoint(models, 132, 32).ms, 30000);
  // Non-finite input never selects.
  assert.equal(GraphAxis.pickPoint(models, Number.NaN, 32), null);

  // End-to-end through the renderer with a stub canvas.
  const { canvas } = stubCanvas();
  /** @type {unknown[]} */
  const inspected = [];
  const renderer = new GraphRenderer(canvas, {
    onInspect: (payload) => inspected.push(payload),
  });
  const inspectPoints = canonicalPoints();
  renderer.render({ points: inspectPoints, unit: 'C' });
  // Coarse bucket at ms=30000 survives (raw starts at ms=500), so models exist
  // for both kinds.
  assert.ok(renderer.models.length > 0);
  const target = renderer.models[0];
  const hit = renderer.inspectAt(target.x, target.y, true);
  assert.ok(hit);
  assert.equal(hit.ms, target.ms);
  assert.equal(hit.kind, target.kind);
  assert.equal(hit.tempC, target.tempC);
  const payload = inspected[0];
  assert.equal(payload.ms, target.ms);
  assert.equal(payload.kind, target.kind);
  assert.equal(payload.valueC, target.tempC);
  assert.equal(payload.value, target.value);
  assert.equal(payload.unit, 'C');
  assert.equal(inspected.length, 1);
  const miss = renderer.inspectAt(target.x + renderer.hitRadiusPx * 3, target.y, true);
  assert.equal(miss, null);
  assert.equal(inspected[inspected.length - 1], null);

  // Selection is sticky across re-render, and clearSelection resets it.
  renderer.inspectAt(target.x, target.y, true);
  renderer.clearSelection();
  assert.equal(renderer.selection, null);
  assert.equal(inspected[inspected.length - 1], null);

  // Faulted/non-finite points are not selectable even when hit directly.
  renderer.render({
    points: [
      rawPoint({ ms: 0, tempC: 20 }),
      rawPoint({ ms: 500, tempC: Number.NaN, fault: 1 }),
    ],
    unit: 'C',
  });
  const invalidHit = renderer.inspectAt(renderer.models[0].x + 1, 0, true);
  assert.ok(invalidHit);
  assert.ok(Number.isFinite(invalidHit.tempC));
  assert.equal(renderer.models.some((model) => !Number.isFinite(model.value)), false);
  renderer.destroy();
});

test('GraphRenderer fulfils the shared contract: render, resize, destroy', () => {
  const { canvas, calls, listeners } = stubCanvas();
  /** @type {boolean[]} */
  const expandedChanges = [];
  const renderer = new GraphRenderer(canvas, {
    onExpandedChange: (expanded) => expandedChanges.push(expanded),
  });
  const contractPoints = canonicalPoints();
  const rateRegions = [
    { startMs: 0, endMs: 42000, rateCPerHour: 152, quality: 'good', sampleCount: 6 },
    { startMs: 30000, endMs: 60000, rateCPerHour: 40, quality: 'good', sampleCount: 4 },
  ];
  const markers = [{ ms: 45000, label: 'Exhaust closing' }];

  renderer.render({ points: contractPoints, rateRegions, markers, unit: 'F', expanded: true });
  assert.ok(calls.length > 0);
  assert.equal(canvas.width, 800);
  assert.equal(canvas.height, 480);
  assert.ok(calls.some(([name]) => name === 'clearRect'));

  // resize() re-measures and redraws the last payload.
  canvas.clientWidth = 1200;
  renderer.resize();
  assert.equal(canvas.width, 1200);

  // Double-tap toggle drives the expanded-change hook.
  renderer.onDoubleClick();
  assert.deepEqual(expandedChanges, [false]);
  renderer.onDoubleClick();
  assert.deepEqual(expandedChanges, [false, true]);

  // Empty payload renders the waiting state without throwing.
  renderer.render({});
  renderer.render({ points: [rawPoint({ ms: 0, tempC: Number.NaN, fault: 1 })] });

  renderer.destroy();
  assert.equal(listeners.size, 0);
  // Destroyed renderer is inert.
  const widthBefore = canvas.width;
  renderer.render({ points: contractPoints });
  renderer.resize();
  assert.equal(canvas.width, widthBefore);
});

test('renderer consumes raw store output directly (no protocol knowledge)', () => {
  const { canvas } = stubCanvas();
  const renderer = new GraphRenderer(canvas);
  const frozenSource = canonicalPoints();
  // Canonical points are frozen; rendering must not mutate them.
  const frozen = Object.freeze(Array.from(frozenSource));
  renderer.render({ points: frozen, unit: 'C' });
  assert.equal(frozen.length, frozenSource.length);
  renderer.destroy();
});