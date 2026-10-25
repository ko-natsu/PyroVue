'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');

require('../src/js/protocol.js');
require('../src/js/csv.js');

const {
  parseCsvStatus,
  csvActionBlocks,
  formatNotesCsv,
  notesNeedFormulaWarning,
  CsvHttpClient,
} = globalThis.PyroVue;

const BOM = String.fromCharCode(0xfeff);

function statusFixture(overrides = {}) {
  return {
    supported: true,
    mounted: true,
    hasRun: false,
    recording: false,
    runId: 0,
    sampleCount: 0,
    bytes: 0,
    freeBytes: 8388608,
    state: 'idle',
    error: null,
    logIntervalMs: 3000,
    maxDurationMs: 21600000,
    startRequiredBytes: 524288,
    ...overrides,
  };
}

function jsonResponse(status, body) {
  return {
    ok: status >= 200 && status < 300,
    status,
    text: async () => (body === undefined ? '' : JSON.stringify(body)),
  };
}


// ---- parseCsvStatus (GET /api/csv) ----


test('invalid storage responses cannot be used as authoritative status', () => {
  const invalid = [
    null,
    statusFixture({ state: 'paused' }),
    statusFixture({ runId: -1 }),
    statusFixture({ runId: 1.5 }),
    statusFixture({ freeBytes: 'lots' }),
    statusFixture({ error: '' }),
    statusFixture({ error: 5 }),
    statusFixture({ logIntervalMs: 0 }),
    statusFixture({ recording: 'yes' }),
  ];
  const missing = statusFixture();
  delete missing.startRequiredBytes;
  invalid.push(missing);
  for (const value of invalid) {
    assert.throws(() => parseCsvStatus(value), globalThis.PyroVue.ProtocolError);
  }
});

// ---- csvActionBlocks (Start gating, download/erase/format availability) ----

test('healthy idle storage allows start and blocks download/erase/format', () => {
  const blocks = csvActionBlocks(parseCsvStatus(statusFixture()));
  assert.equal(blocks.start, null);
  assert.notEqual(blocks.download, null);
  assert.notEqual(blocks.erase, null);
  assert.notEqual(blocks.format, null);
});

test('a saved run blocks Start until explicitly erased and stays downloadable', () => {
  const blocks = csvActionBlocks(parseCsvStatus(statusFixture({
    hasRun: true, runId: 9, bytes: 445000, state: 'complete',
  })));
  assert.notEqual(blocks.start, null);
  assert.equal(blocks.download, null);
  assert.equal(blocks.erase, null);
});

test('an unknown-id corrupt saved file is erasable but never downloadable', () => {
  const blocks = csvActionBlocks(parseCsvStatus(statusFixture({
    hasRun: true, runId: 0, bytes: 1234, state: 'interrupted',
  })));
  assert.notEqual(blocks.start, null);
  assert.notEqual(blocks.download, null);
  assert.equal(blocks.erase, null);
});

test('an empty saved file cannot be downloaded but can be erased', () => {
  const blocks = csvActionBlocks(parseCsvStatus(statusFixture({
    hasRun: true, runId: 4, bytes: 0, state: 'interrupted',
  })));
  assert.notEqual(blocks.download, null);
  assert.equal(blocks.erase, null);
});

test('recording blocks start, download and erase while Stop stays a session concern', () => {
  const blocks = csvActionBlocks(parseCsvStatus(statusFixture({
    recording: true, hasRun: true, runId: 9, bytes: 2048, state: 'recording',
  })));
  assert.notEqual(blocks.start, null);
  assert.notEqual(blocks.download, null);
  assert.notEqual(blocks.erase, null);
});

test('unmounted storage blocks Start and enables the full partition format action', () => {
  const blocks = csvActionBlocks(parseCsvStatus(statusFixture({ mounted: false })));
  assert.notEqual(blocks.start, null);
  assert.equal(blocks.format, null);
});

test('insufficient free storage blocks Start only', () => {
  const blocks = csvActionBlocks(parseCsvStatus(statusFixture({ freeBytes: 4096 })));
  assert.notEqual(blocks.start, null);
  assert.notEqual(blocks.download, null);
});

test('an acquisition or storage error blocks another Start even without a saved file', () => {
  const blocks = csvActionBlocks(parseCsvStatus(statusFixture({
    state: 'error', error: 'invalid_sample',
  })));
  assert.notEqual(blocks.start, null);
  assert.notEqual(blocks.download, null);
});

// ---- notes CSV serializer ----


test('notes CSV preserves notes byte-exact including formula-like, multiline and quoted text', () => {
  const notes = [
    '=1+1+SUM(A1)',
    '-3 degrees at ramp',
    '@import',
    'plain, with comma',
    'he said "hi"',
    'first line\nsecond line',
    'crlf line\r\nnext',
    '  leading and trailing  ',
    'unicode \u00b0C \u2014 ok',
  ];
  const markers = notes.map((note, index) => ({ id: `m${index}`, runId: 5, ms: index * 1000, note }));
  const csv = formatNotesCsv(5, markers);
  // Whole-file equality pins the exact bytes: CRLF-separated rows, each note
  // quoted only where RFC 4180 requires it and never rewritten.
  assert.equal(csv, BOM + [
    'run_id,marker_id,elapsed_ms,elapsed_hms,time_basis,note',
    '5,m0,0,00:00:00,estimated,=1+1+SUM(A1)',
    '5,m1,1000,00:00:01,estimated,-3 degrees at ramp',
    '5,m2,2000,00:00:02,estimated,@import',
    '5,m3,3000,00:00:03,estimated,"plain, with comma"',
    '5,m4,4000,00:00:04,estimated,"he said ""hi"""',
    '5,m5,5000,00:00:05,estimated,"first line\nsecond line"',
    '5,m6,6000,00:00:06,estimated,"crlf line\r\nnext"',
    '5,m7,7000,00:00:07,estimated,  leading and trailing  ',
    '5,m8,8000,00:00:08,estimated,unicode °C — ok',
  ].join('\r\n') + '\r\n');
  assert.deepEqual(markers.map((marker) => marker.note), notes);
});

test('notes CSV sorts by captured elapsed time then id and excludes other runs', () => {
  const csv = formatNotesCsv(2, [
    { id: 'b', runId: 2, ms: 500, note: 'second' },
    { id: 'a', runId: 2, ms: 500, note: 'first' },
    { id: 'c', runId: 2, ms: 100, note: 'earliest' },
    { id: 'd', runId: 1, ms: 50, note: 'other run' },
    { id: 'e', runId: 2, ms: 1234.5, note: 'fractional capture' },
  ]);
  const body = csv.slice(BOM.length);
  const rows = body.split('\r\n').slice(1).filter((line) => line.length > 0);
  assert.equal(rows.length, 4);
  assert.match(rows[0], /^2,c,100,00:00:00,estimated,earliest$/);
  assert.match(rows[1], /^2,a,500,00:00:00,estimated,first$/);
  assert.match(rows[2], /^2,b,500,00:00:00,estimated,second$/);
  assert.match(rows[3], /^2,e,1234\.5,00:00:01,estimated,fractional capture$/);
  assert.doesNotMatch(csv, /other run/);
});

test('notes CSV rejects malformed input instead of inventing rows', () => {
  assert.throws(() => formatNotesCsv(0, []), RangeError);
  assert.throws(() => formatNotesCsv(1.5, []), RangeError);
  assert.throws(() => formatNotesCsv(1, 'nope'), TypeError);
  assert.throws(() => formatNotesCsv(1, [{ id: '', runId: 1, ms: 0, note: '' }]), TypeError);
  assert.throws(() => formatNotesCsv(1, [{ id: 'a', runId: 0, ms: 0, note: '' }]), TypeError);
  assert.throws(() => formatNotesCsv(1, [{ id: 'a', runId: 1, ms: -1, note: '' }]), RangeError);
  assert.throws(() => formatNotesCsv(1, [{ id: 'a', runId: 1, ms: 0, note: 7 }]), TypeError);
});


test('formula-like note warning fires only on leading sigils', () => {
  assert.equal(notesNeedFormulaWarning([{ id: 'a', runId: 1, ms: 0, note: 'ok' }]), false);
  assert.equal(notesNeedFormulaWarning([{ id: 'a', runId: 1, ms: 0, note: 'a = b' }]), false);
  assert.equal(notesNeedFormulaWarning([{ id: 'a', runId: 1, ms: 0, note: '=1+1' }]), true);
  assert.equal(notesNeedFormulaWarning([{ id: 'a', runId: 1, ms: 0, note: '-3' }]), true);
  assert.equal(notesNeedFormulaWarning([{ id: 'a', runId: 1, ms: 0, note: '@x' }]), true);
  assert.equal(notesNeedFormulaWarning([{ id: 'a', runId: 1, ms: 0, note: '+x' }]), true);
  assert.equal(notesNeedFormulaWarning([]), false);
});

test('legacy targets remain unsupported rather than offering storage actions', async () => {
  const legacy = new CsvHttpClient(async () => jsonResponse(404, null));
  assert.deepEqual(await legacy.getStatus(), { supported: false });
});

test('storage mutations reject missing acknowledgments and device refusals', async () => {
  for (const [status, body] of [[200, {}], [200, { ok: false }], [409, { code: 'RUN_ACTIVE' }]]) {
    const client = new CsvHttpClient(async () => jsonResponse(status, body));
    await assert.rejects(client.eraseRun(1), Error);
    await assert.rejects(client.formatStorage(), Error);
  }
});

