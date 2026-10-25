'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');

require('../src/js/protocol.js');
require('../src/js/state.js');
require('../src/js/transport.js');

const { AppStore, DeviceTransport } = globalThis.PyroVue;

const DEVICE_ID = 'a1b2c3d4e5f6';
const BOOT_ID = '0123456789abcdef';

class FakeWebSocket {
  static OPEN = 1;
  static instances = [];

  constructor(url) {
    this.url = url;
    this.readyState = 0;
    this.sent = [];
    FakeWebSocket.instances.push(this);
  }

  open() {
    this.readyState = FakeWebSocket.OPEN;
    this.onopen?.();
  }

  receive(message) {
    this.onmessage?.({ data: JSON.stringify(message) });
  }

  disconnect() {
    this.readyState = 3;
    this.onclose?.();
  }

  send(payload) {
    this.sent.push(payload);
  }

  close() {
    this.readyState = 3;
  }
}

function hello(overrides = {}) {
  return {
    type: 'hello',
    protocol: 3,
    firmware: 'run-logging-1',
    sampleHz: 2,
    runId: 9,
    runActive: true,
    deviceId: DEVICE_ID,
    bootId: BOOT_ID,
    capabilities: { runArchive: true },
    acquireIntervalMs: 500,
    logIntervalMs: 2000,
    maxRunMs: 86400000,
    logging: {
      state: 'recording',
      durableLogSeq: 4,
      latestAcqSeq: 4,
      bufferedRecords: 0,
      acquisitionGaps: 0,
      droppedAcquired: 0,
      queueHighWater: 1,
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
    runStartMs: 0,
    recentWindowMs: 600000,
    coarseResolutionMs: 30000,
    snapshotThroughSeq: 0,
    ...overrides,
  };
}

function jsonResponse(status, body) {
  return new Response(JSON.stringify(body), {
    status,
    headers: { 'Content-Type': 'application/json' },
  });
}

function commandResult(request, overrides = {}) {
  return {
    type: 'command.result',
    protocol: 3,
    requestId: request.requestId,
    ok: true,
    runId: request.type === 'run.stop' ? request.runId : 10,
    runActive: request.type !== 'run.stop',
    targetRunId: request.type === 'run.stop'
      ? request.runId
      : request.type === 'preset' ? 0 : 10,
    durable: true,
    ...overrides,
  };
}

test('reconnects with polling fallback after socket loss', async () => {
  FakeWebSocket.instances.length = 0;
  const timeouts = [];
  const intervals = [];
  const requests = [];
  const fetch = async (url) => {
    requests.push(url);
    if (url === '/api/info') {
      return jsonResponse(200, hello({ runActive: true, clients: 0, preset: 'slow' }));
    }
    return jsonResponse(200, {
      type: 'sample', protocol: 3, runId: 9, runActive: true,
      seq: 4, ms: 2000, temp: 100, fault: 0,
    });
  };
  const store = new AppStore({ now: () => 0, wallNow: () => 10000 });
  const transport = new DeviceTransport(store, {
    WebSocket: FakeWebSocket,
    fetch,
    location: { protocol: 'http:', host: 'kiln.local' },
    setTimeout: (callback, delay) => { timeouts.push({ callback, delay }); return timeouts.length; },
    clearTimeout: () => {},
    setInterval: (callback, delay) => { intervals.push({ callback, delay }); return intervals.length; },
    clearInterval: () => {},
    reconnectBaseMs: 250,
    pollMs: 500,
  });

  transport.start();
  assert.equal(FakeWebSocket.instances[0].url, 'ws://kiln.local/ws');
  FakeWebSocket.instances[0].open();
  FakeWebSocket.instances[0].receive(hello());
  assert.equal(store.run.id, 9);
  assert.equal(store.device.deviceId, DEVICE_ID);
  assert.equal(store.device.bootId, BOOT_ID);

  FakeWebSocket.instances[0].disconnect();
  await new Promise((resolve) => setImmediate(resolve));
  await new Promise((resolve) => setImmediate(resolve));
  assert.deepEqual(requests, ['/api/info', '/api/now']);
  assert.equal(store.connection.phase, 'polling-degraded');
  assert.equal(store.currentReading.sample.seq, 4);
  assert.equal(timeouts[0].delay, 250);
  assert.equal(intervals[0].delay, 500);

  timeouts[0].callback();
  assert.equal(FakeWebSocket.instances.length, 2);
  transport.stop();
});

test('commands always use HTTP even while the websocket is connected', async () => {
  FakeWebSocket.instances.length = 0;
  const posts = [];
  const fetch = async (url, options = {}) => {
    assert.equal(url, '/api/cmd');
    const body = JSON.parse(options.body);
    posts.push({ url, options, body });
    return jsonResponse(200, commandResult(body));
  };
  const store = new AppStore();
  const transport = new DeviceTransport(store, {
    WebSocket: FakeWebSocket,
    fetch,
    location: { protocol: 'http:', host: 'kiln.local' },
    setTimeout: () => 1,
    clearTimeout: () => {},
    setInterval: () => 1,
    clearInterval: () => {},
    wallNow: () => 1700000000000,
  });

  transport.start();
  const socket = FakeWebSocket.instances[0];
  socket.open();
  socket.receive(hello());

  const start = await transport.send({ type: 'run.start' });
  assert.equal(socket.sent.length, 0, 'websocket is never used for commands');
  assert.equal(posts[0].url, '/api/cmd');
  assert.equal(posts[0].options.method, 'POST');
  assert.equal(posts[0].options.headers['X-PyroVue-Device-Id'], DEVICE_ID);
  assert.equal(posts[0].options.headers['Content-Type'], 'application/json');
  assert.equal(posts[0].body.type, 'run.start');
  assert.equal(posts[0].body.bootId, BOOT_ID);
  assert.equal(posts[0].body.afterRunId, 9);
  assert.equal(posts[0].body.wallStartUtcMs, 1700000000000);
  assert.match(posts[0].body.requestId, /^[A-Za-z0-9_-]{1,80}$/);
  assert.deepEqual(start, commandResult(posts[0].body));

  const stop = await transport.send({ type: 'run.stop' });
  assert.equal(posts[1].body.type, 'run.stop');
  assert.equal(posts[1].body.runId, 9);
  assert.equal(posts[1].body.bootId, BOOT_ID);
  assert.equal(posts[1].options.headers['X-PyroVue-Device-Id'], DEVICE_ID);
  assert.equal(stop.runActive, false);
  assert.equal(stop.durable, true);
  assert.equal(socket.sent.length, 0);
  transport.stop();
});

test('unknown command context waits for authoritative /api/info identity', async () => {
  const calls = [];
  const fetch = async (url, options = {}) => {
    calls.push(url);
    if (url === '/api/info') {
      return jsonResponse(200, hello({
        deviceId: 'bbbbbbbbbbbb',
        bootId: 'fedcba9876543210',
        runId: 42,
        runActive: false,
      }));
    }
    assert.equal(url, '/api/cmd');
    const body = JSON.parse(options.body);
    assert.equal(options.headers['X-PyroVue-Device-Id'], 'bbbbbbbbbbbb');
    assert.equal(body.bootId, 'fedcba9876543210');
    assert.equal(body.afterRunId, 42);
    return jsonResponse(200, commandResult(body, { runId: 43, targetRunId: 43, runActive: true }));
  };
  const store = new AppStore();
  const transport = new DeviceTransport(store, { fetch, wallNow: () => 1234 });

  const result = await transport.send({ type: 'run.start' });
  assert.deepEqual(calls, ['/api/info', '/api/cmd']);
  assert.equal(result.runId, 43);
  assert.equal(store.device.deviceId, 'bbbbbbbbbbbb');
  assert.equal(store.run.id, 42);
});

test('captures device identity once and never rebinds after a device switch', async () => {
  const posts = [];
  let resolveResponse;
  const responsePromise = new Promise((resolve) => { resolveResponse = resolve; });
  const fetch = async (url, options = {}) => {
    assert.equal(url, '/api/cmd');
    const body = JSON.parse(options.body);
    posts.push({ url, options, body });
    return responsePromise;
  };
  const store = new AppStore();
  store.handle(hello());
  const transport = new DeviceTransport(store, { fetch, wallNow: () => 999 });

  const sending = transport.send({ type: 'run.start' });
  assert.equal(posts.length, 1, 'command is built before awaiting the response');
  const sent = posts[0];

  // A device switch while the HTTP request is in flight must not rewrite the
  // already-bound header, bootId, or afterRunId.
  store.handle(hello({
    deviceId: 'cccccccccccc',
    bootId: '1111222233334444',
    runId: 55,
  }));
  resolveResponse(jsonResponse(200, commandResult(sent.body, { runId: 10, targetRunId: 10 })));
  const result = await sending;

  assert.equal(sent.options.headers['X-PyroVue-Device-Id'], DEVICE_ID);
  assert.equal(sent.body.bootId, BOOT_ID);
  assert.equal(sent.body.afterRunId, 9);
  assert.equal(result.requestId, sent.body.requestId);
});

test('surfaces the actual HTTP command error without retrying', async () => {
  const posts = [];
  const fetch = async (url, options = {}) => {
    posts.push({ url, options, body: JSON.parse(options.body) });
    return jsonResponse(409, {
      type: 'error',
      protocol: 3,
      code: 'STALE_COMMAND',
      message: 'start afterRunId is not the latest allocated run',
    });
  };
  const store = new AppStore();
  store.handle(hello());
  const transport = new DeviceTransport(store, { fetch, wallNow: () => 1 });

  await assert.rejects(
    transport.send({ type: 'run.start' }),
    /HTTP 409.*STALE_COMMAND.*start afterRunId/,
  );
  assert.equal(posts.length, 1, 'no automatic ambiguous retry');
  assert.equal(posts[0].options.headers['X-PyroVue-Device-Id'], DEVICE_ID);
});

test('validates command responses even on non-2xx and mismatched request identities', async () => {
  const posts = [];
  let mode = 'text';
  const fetch = async (url, options = {}) => {
    const body = JSON.parse(options.body);
    posts.push({ url, options, body });
    if (mode === 'text') return new Response('device exploded', { status: 503 });
    if (mode === 'requestId') {
      return jsonResponse(200, commandResult({ requestId: 'some-other-request' }));
    }
    return jsonResponse(200, commandResult(body, { targetRunId: 99 }));
  };
  const store = new AppStore();
  store.handle(hello());
  const transport = new DeviceTransport(store, { fetch, wallNow: () => 1 });

  await assert.rejects(
    transport.send({ type: 'run.start' }),
    /HTTP 503.*not valid JSON/,
  );

  mode = 'requestId';
  await assert.rejects(
    transport.send({ type: 'run.start' }),
    /requestId .* does not match/,
  );

  mode = 'target';
  await assert.rejects(
    transport.send({ type: 'run.stop' }),
    /targetRunId/,
  );
  assert.equal(posts.length, 3);
});

test('enforces the UTF-8 byte limit rather than JavaScript code units', async () => {
  const posts = [];
  const fetch = async (url, options = {}) => {
    posts.push({ url, options });
    throw new Error('must not reach the device');
  };
  const store = new AppStore();
  store.handle(hello());
  const transport = new DeviceTransport(store, { fetch, wallNow: () => 1 });

  // 60 four-byte emoji are 120 JS code units but 240 bytes in the value alone,
  // which pushes the JSON body past the 256-byte device limit.
  await assert.rejects(
    transport.send({ type: 'preset', value: '😀'.repeat(60) }),
    /256-byte device limit/,
  );
  assert.equal(posts.length, 0);
});

test('syncState reconciles over HTTP while websocket stays connected', async () => {
  FakeWebSocket.instances.length = 0;
  const requests = [];
  const fetch = async (url) => {
    requests.push(url);
    if (url === '/api/info') {
      return jsonResponse(200, hello({ runId: 3, runActive: true, clients: 1, preset: 'slow' }));
    }
    return jsonResponse(200, {
      type: 'sample', protocol: 3, runId: 3, runActive: true,
      seq: 8, ms: 4000, temp: 125, fault: 0,
    });
  };
  const store = new AppStore({ now: () => 4000, wallNow: () => 8000 });
  const transport = new DeviceTransport(store, {
    WebSocket: FakeWebSocket,
    fetch,
    location: { protocol: 'http:', host: 'kiln.local' },
    setTimeout: () => 1,
    clearTimeout: () => {},
    setInterval: () => 1,
    clearInterval: () => {},
  });

  transport.start();
  const socket = FakeWebSocket.instances[0];
  socket.open();
  socket.receive(hello({ runId: 3, runActive: true }));
  const phase = store.connection.phase;
  const channel = store.connection.channel;
  assert.equal(transport.pollTimer, null);

  await transport.syncState();

  assert.deepEqual(requests, ['/api/info', '/api/now']);
  assert.equal(socket.readyState, FakeWebSocket.OPEN);
  assert.equal(transport.socket, socket);
  assert.equal(store.connection.phase, phase);
  assert.equal(store.connection.channel, channel);
  assert.equal(transport.pollTimer, null);
  assert.equal(store.currentReading.sample.seq, 8);
  transport.stop();
});
