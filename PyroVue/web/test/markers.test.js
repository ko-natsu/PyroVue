'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');

require('../src/js/markers.js');
const { MarkerStore } = globalThis.PyroVue;

class MemoryStorage {
  constructor(value = null) {
    this.value = value;
    this.failRead = false;
    this.failWrite = false;
  }

  getItem(key) {
    if (this.failRead) throw new Error('read denied');
    assert.equal(key, 'pyrovue.markers.v1');
    return this.value;
  }

  setItem(key, value) {
    if (this.failWrite) throw new Error('quota exceeded');
    assert.equal(key, 'pyrovue.markers.v1');
    this.value = value;
  }
}

test('markers persist across store recreation and retain notes and timestamp', () => {
  const storage = new MemoryStorage();
  const first = new MarkerStore(storage);
  const marker = first.add(17, 1234.5);
  assert.deepEqual(first.update(17, marker.id, '  First peak  '), {
    ...marker, note: 'First peak',
  });

  const reloaded = new MarkerStore(storage);
  assert.deepEqual(reloaded.list(17), [{ ...marker, note: 'First peak' }]);
});

test('same-millisecond captures have distinct identities and are ordered', () => {
  const store = new MarkerStore(new MemoryStorage());
  const first = store.add(1, 500);
  const second = store.add(1, 500);
  assert.notEqual(first.id, second.id);
  assert.deepEqual(store.list(1).map((marker) => marker.ms), [500, 500]);
  assert.deepEqual(new Set(store.list(1).map((marker) => marker.id)).size, 2);
});

test('edits and deletion persist and cannot cross run boundaries', () => {
  const storage = new MemoryStorage();
  const store = new MarkerStore(storage);
  const one = store.add(1, 200);
  const otherRun = store.add(2, 200);

  assert.equal(store.update(2, one.id, 'wrong run'), null);
  assert.equal(store.remove(2, one.id), false);
  assert.equal(store.update(1, one.id, '  note  ').note, 'note');
  assert.equal(store.remove(1, one.id), true);
  assert.deepEqual(store.list(1), []);

  const reloaded = new MarkerStore(storage);
  assert.deepEqual(reloaded.list(1), []);
  assert.deepEqual(reloaded.list(2), [otherRun]);
});

test('malformed JSON and invalid saved records recover without exposing corrupt data', () => {
  const storage = new MemoryStorage('{not json');
  assert.deepEqual(new MarkerStore(storage).list(1), []);

  storage.value = JSON.stringify({ version: 1, markers: [
    { id: 'good', runId: 1, ms: 0, note: 'valid' },
    { id: 'bad-time', runId: 1, ms: Infinity, note: 'invalid' },
    { id: 'bad-run', runId: 0, ms: 1, note: 'invalid' },
    { id: 'good', runId: 2, ms: 2, note: 'duplicate id' },
    null,
  ] });
  const recovered = new MarkerStore(storage);
  assert.deepEqual(recovered.list(1), [{ id: 'good', runId: 1, ms: 0, note: 'valid' }]);
  assert.deepEqual(recovered.list(2), []);
});

test('storage access and persistence failures are surfaced without claiming a mutation', () => {
  const deniedRead = new MemoryStorage();
  deniedRead.failRead = true;
  assert.throws(() => new MarkerStore(deniedRead), /read denied/);

  const storage = new MemoryStorage();
  const store = new MarkerStore(storage);
  storage.failWrite = true;
  assert.throws(() => store.add(3, 40), /quota exceeded/);
  assert.deepEqual(store.list(3), []);

  storage.failWrite = false;
  const marker = store.add(3, 40);
  storage.failWrite = true;
  assert.throws(() => store.update(3, marker.id, 'not saved'), /quota exceeded/);
  assert.equal(store.list(3)[0].note, '');
  assert.throws(() => store.remove(3, marker.id), /quota exceeded/);
  assert.equal(store.list(3).length, 1);
});

test('run and timestamp constraints reject invalid captures', () => {
  const store = new MarkerStore(new MemoryStorage());
  assert.throws(() => store.add(0, 1), RangeError);
  assert.throws(() => store.add(1, -1), RangeError);
  assert.throws(() => store.add(1, Number.NaN), RangeError);
  assert.throws(() => store.update(1, 'missing', 'x'.repeat(501)), RangeError);
});
