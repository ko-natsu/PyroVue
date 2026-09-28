'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');

require('../src/js/theme.js');

const { deriveHeatTheme } = globalThis.PyroVue;
const active = (tempC, overrides = {}) => deriveHeatTheme({
  active: true, fresh: true, valid: true, tempC, ...overrides,
});

test('heat stages use inclusive thresholds and preserve values immediately below them', () => {
  const boundaries = [
    [499.999, 'neutral'], [500, 'dull-red'], [699.999, 'dull-red'],
    [700, 'red'], [899.999, 'red'], [900, 'orange'], [1099.999, 'orange'],
    [1100, 'yellow'], [1249.999, 'yellow'], [1250, 'white'],
  ];
  for (const [tempC, stage] of boundaries) assert.equal(active(tempC).stage, stage, `${tempC}°C`);
});

test('intensity is globally linear and clamped independently of stage', () => {
  assert.equal(active(400).intensity, 0);
  assert.equal(active(850).intensity, 0.5);
  assert.equal(active(1300).intensity, 1);
  assert.equal(active(-100).intensity, 0);
  assert.equal(active(5000).intensity, 1);
});

test('idle, stale, invalid, and non-finite readings are neutral', () => {
  const validReading = { active: true, fresh: true, valid: true, tempC: 1000 };
  for (const overrides of [
    { active: false }, { fresh: false }, { valid: false },
    { tempC: NaN }, { tempC: Infinity }, { tempC: -Infinity },
    { tempC: '1000' }, { tempC: null },
  ]) {
    assert.deepEqual(deriveHeatTheme({ ...validReading, ...overrides }), { stage: 'neutral', intensity: 0 });
  }
  assert.deepEqual(deriveHeatTheme(null), { stage: 'neutral', intensity: 0 });
});

test('each result is frozen and independent across calls', () => {
  const first = active(700);
  const second = active(1250);
  assert.equal(Object.isFrozen(first), true);
  assert.equal(Object.isFrozen(second), true);
  assert.deepEqual(first, { stage: 'red', intensity: 1 / 3 });
  assert.deepEqual(second, { stage: 'white', intensity: 17 / 18 });
  assert.notEqual(first, second);
});
