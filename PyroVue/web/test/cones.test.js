'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');

require('../src/js/metrics.js');
require('../src/js/cones.js');

const { CONE_SEQUENCE, SELF_SUPPORTING_EQUIVALENT_C, estimateConeProgress } = globalThis.PyroVue;

function raw(ms, tempC, seq, overrides = {}) {
  return { kind: 'raw', ms, tempC, seq, fault: 0, ...overrides };
}

function coarse(ms, averageC, overrides = {}) {
  return { kind: 'coarse', ms, averageC, resolutionMs: 30000, fault: 0, ...overrides };
}

/** A direct 100°C rise at the given rate, with optional flat peak hold. */
function measured(rate, peakC, holdMs = 0) {
  const riseMs = 100 * 3600000 / rate;
  return [
    raw(0, peakC - 100, 1),
    raw(riseMs, peakC, 2),
    ...(holdMs ? [raw(riseMs + holdMs, peakC, 3), raw(riseMs + holdMs * 2, peakC, 4)] : []),
  ];
}

test('publishes immutable, ordered self-supporting chart values from Orton 2016', () => {
  assert.deepEqual(CONE_SEQUENCE, [
    '022', '021', '020', '019', '018', '017', '016', '015', '014', '013', '012', '011', '010', '09', '08', '07', '06',
    '05½', '05', '04', '03', '02', '01', '1', '2', '3', '4', '5', '5½', '6', '7', '8', '9', '10', '11', '12', '13', '14',
  ]);
  assert.deepEqual(SELF_SUPPORTING_EQUIVALENT_C['022'], [null, 586, 590]);
  assert.deepEqual(SELF_SUPPORTING_EQUIVALENT_C['021'], [null, 600, 617]);
  assert.deepEqual(SELF_SUPPORTING_EQUIVALENT_C['6'], [1185, 1222, 1243]);
  assert.deepEqual(SELF_SUPPORTING_EQUIVALENT_C['14'], [1351, 1365, 1384]);
  assert.equal(Object.isFrozen(CONE_SEQUENCE), true);
  assert.equal(Object.isFrozen(SELF_SUPPORTING_EQUIVALENT_C), true);
  assert.equal(Object.isFrozen(SELF_SUPPORTING_EQUIVALENT_C['6']), true);
  assert.equal(CONE_SEQUENCE.length, 38);
  assert.equal(Object.keys(SELF_SUPPORTING_EQUIVALENT_C).length, 38);
});

test('uses nearest chart rate, with exact ties going to the faster column', () => {
  for (const [rate, expectedColumn] of [[37.49, 15], [37.5, 60], [37.51, 60], [104.99, 60], [105, 150], [105.01, 150]]) {
    const model = estimateConeProgress(measured(rate, 1222));
    assert.equal(model.rateColumnCPerHour, expectedColumn, `column at ${rate}°C/hr`);
    assert.equal(model.quality, 'measured-100c');
    assert.ok(Math.abs(model.rateCPerHour - rate) < 1e-6);
  }
  // Cone 022 has no 15°C/hr cell, so its nearest available cell is 60°C/hr.
  const low = estimateConeProgress(measured(15, 580));
  assert.equal(low.cone, '022');
  assert.equal(low.rateColumnCPerHour, 60);
});

test('interpolates progress and retains exact endpoints until temperature exceeds them', () => {
  const exact = estimateConeProgress(measured(60, 1222));
  assert.deepEqual({ label: exact.label, cone: exact.cone, previous: exact.previousCone, next: exact.nextCone, progress: exact.progress },
    { label: 'Cone 6', cone: '6', previous: '5½', next: '7', progress: 1 });
  const between = estimateConeProgress(measured(60, (1222 + 1239) / 2));
  assert.equal(between.label, 'Cone 7');
  assert.equal(between.cone, '7');
  assert.equal(between.previousCone, '6');
  assert.equal(between.nextCone, '8');
  assert.equal(between.progress, 0.5);
  assert.equal(estimateConeProgress(measured(60, 1222.01)).cone, '7');
  const below = estimateConeProgress(measured(60, 585));
  assert.deepEqual({ label: below.label, cone: below.cone, progress: below.progress, previous: below.previousCone },
    { label: 'Below cone 022', cone: '022', progress: 0, previous: null });
  const above = estimateConeProgress(measured(60, 1366));
  assert.deepEqual({ label: above.label, cone: above.cone, progress: above.progress, next: above.nextCone },
    { label: 'Cone 14+', cone: '14', progress: 1, next: null });
  assert.equal(Object.isFrozen(exact), true);
});

test('measures the final 100°C rise at first peak arrival, unaffected by peak holds or cooling', () => {
  const ramp = measured(60, 1222);
  const held = measured(60, 1222, 3600000);
  const a = estimateConeProgress(ramp);
  const b = estimateConeProgress(held);
  assert.equal(a.rateCPerHour, 60);
  assert.equal(b.rateCPerHour, 60);
  assert.equal(b.quality, 'measured-100c');
  const cooling = [...ramp, raw(ramp[1].ms + 10000, 1200, 3)];
  assert.equal(estimateConeProgress(cooling).rateCPerHour, 60);
});

test('chooses the latest reheat peak and its last upward 100°C crossing', () => {
  const points = [
    raw(0, 390, 1), raw(3600000, 500, 2),
    raw(7200000, 390, 3), raw(10800000, 500, 4),
  ];
  const model = estimateConeProgress(points);
  assert.equal(model.quality, 'measured-100c');
  assert.ok(Math.abs(model.rateCPerHour - 110) < 1e-9);
  assert.equal(model.peakC, 500);
});

test('rejects faults and raw sequence gaps rather than crossing them', () => {
  const faulted = [raw(0, 500, 1), raw(1000000, 550, 2, { fault: 1 }), raw(2000000, 600, 3)];
  assert.equal(estimateConeProgress(faulted), null);
  const sequenceGap = [raw(0, 500, 1), raw(3600000, 550, 2), raw(7200000, 600, 4)];
  assert.equal(estimateConeProgress(sequenceGap), null);
  const unknownTimeFault = [raw(0, 1122, 1), raw(NaN, 0, 999), raw(3600000, 1222, 2)];
  assert.equal(estimateConeProgress(unknownTimeFault), null, 'unknown-time invalid event separates input chunks');
  const faultBeforeValidPrefix = [
    raw(0, 200, 1, { fault: 1 }),
    raw(60000, 500, 2), raw(1800000, 530, 3), raw(3600000, 560, 4), raw(5400000, 590, 5),
  ];
  const fallback = estimateConeProgress(faultBeforeValidPrefix);
  assert.equal(fallback.quality, 'estimated-regression');
  assert.ok(fallback.rateCPerHour > 0);
});

test('coarse continuity honors bucket ends and mixed boundaries honor coarse resolution', () => {
  const coarseAdjacent = [coarse(0, 1122), coarse(30000, 1155), coarse(60000, 1188), coarse(90000, 1222)];
  assert.equal(estimateConeProgress(coarseAdjacent).quality, 'measured-100c');
  const coarseGap = [coarse(0, 1122), coarse(30000, 1155), coarse(90000, 1188), coarse(120000, 1222)];
  assert.equal(estimateConeProgress(coarseGap), null);
  assert.equal(estimateConeProgress([coarse(0, 1122), raw(60000, 1222, 1)]), null);
  const mixedValid = [coarse(0, 1122, { resolutionMs: 3600000 }), raw(3600000, 1222, 1)];
  const validModel = estimateConeProgress(mixedValid);
  assert.equal(validModel.quality, 'measured-100c');
  assert.equal(validModel.rateCPerHour, 100);
});

test('uses only a fair or good positive regression for short-rise fallback', () => {
  const points = Array.from({ length: 7 }, (_, i) => raw(i * 600000, 700 + i * 5, i + 1));
  const model = estimateConeProgress(points);
  assert.equal(model.quality, 'estimated-regression');
  assert.ok(Math.abs(model.rateCPerHour - 30) < 1e-9);
  assert.equal(estimateConeProgress([raw(0, 700, 1), raw(300000, 705, 2)]), null);
  const cooling = Array.from({ length: 7 }, (_, i) => raw(i * 600000, 760 - i * 5, i + 1));
  assert.equal(estimateConeProgress(cooling), null);
});

test('deduplicates timestamps using raw, newest-sequence, and finer-resolution precedence', () => {
  const base = [raw(0, 1122, 1), raw(3600000, 1222, 2)];
  const coarseWinnerCandidate = [...base.slice(0, 1), coarse(3600000, 2000), base[1]];
  assert.equal(estimateConeProgress(coarseWinnerCandidate).peakC, 1222);
  const newestRaw = [raw(0, 1000, 1), raw(0, 1122, 2), raw(3600000, 1222, 3)];
  assert.equal(estimateConeProgress(newestRaw).peakC, 1222);
  const finerCoarse = [
    coarse(0, 1122, { resolutionMs: 3600000 }),
    coarse(3600000, 900, { resolutionMs: 60000 }),
    coarse(3600000, 1222, { resolutionMs: 30000 }),
  ];
  assert.equal(estimateConeProgress(finerCoarse).peakC, 1222);
});

test('supports fractional and zero-padded cone labels and rejects unavailable input', () => {
  assert.equal(estimateConeProgress(measured(60, 1200)).cone, '5½');
  assert.equal(estimateConeProgress(measured(60, 586)).cone, '022');
  assert.equal(estimateConeProgress(null), null);
  assert.equal(estimateConeProgress([]), null);
  assert.equal(estimateConeProgress([raw(0, NaN, 1)]), null);
  assert.equal(estimateConeProgress([raw(NaN, 500, 1)]), null);
});
