'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');

require('../src/js/protocol.js');
require('../src/js/state.js');
require('../src/js/transport.js');

const { AppStore, DeviceTransport, createCommand } = globalThis.PyroVue;

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

function hello() {
  return {
    type: 'hello', protocol: 2, firmware: 'rework-2', sampleHz: 2, runId: 9, runActive: true,
    runStartMs: 0, recentWindowMs: 600000, coarseResolutionMs: 30000, snapshotThroughSeq: 0,
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
      return { ok: true, status: 200, json: async () => ({
        type: 'hello', protocol: 2, firmware: 'rework-2', sampleHz: 2,
        runId: 9, runActive: true, clients: 0, preset: 'slow',
      }) };
    }
    return { ok: true, status: 200, json: async () => ({
      type: 'sample', protocol: 2, runId: 9, runActive: true,
      seq: 4, ms: 2000, temp: 100, fault: 0,
    }) };
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

test('uses websocket first and HTTP fallback for pending commands', async () => {
  FakeWebSocket.instances.length = 0;
  const posts = [];
  const fetch = async (url, options) => {
    posts.push({ url, options });
    return { ok: true, status: 200 };
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
  });

  transport.start();
  const socket = FakeWebSocket.instances[0];
  socket.open();
  const websocketResult = await transport.send(createCommand('run.start'));
  assert.deepEqual(websocketResult, { channel: 'websocket', pending: true });
  assert.deepEqual(JSON.parse(socket.sent[0]), { type: 'run.start' });

  socket.readyState = 3;
  const httpResult = await transport.send(createCommand('run.stop'));
  assert.deepEqual(httpResult, { channel: 'http', pending: true });
  assert.equal(posts[0].url, '/api/cmd');
  assert.deepEqual(JSON.parse(posts[0].options.body), { type: 'run.stop' });
  transport.stop();
});

test('syncState reconciles over HTTP while websocket stays connected', async () => {
  FakeWebSocket.instances.length = 0;
  const requests = [];
  const fetch = async (url) => {
    requests.push(url);
    if (url === '/api/info') {
      return { ok: true, status: 200, json: async () => ({
        type: 'hello', protocol: 2, firmware: 'rework-2', sampleHz: 2,
        runId: 3, runActive: true, clients: 1, preset: 'slow',
      }) };
    }
    return { ok: true, status: 200, json: async () => ({
      type: 'sample', protocol: 2, runId: 3, runActive: true,
      seq: 8, ms: 4000, temp: 125, fault: 0,
    }) };
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
  socket.receive(hello());
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
