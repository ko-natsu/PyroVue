// @ts-check
(function registerState(root) {
  'use strict';

  const api = root.PyroVue;
  if (!api || typeof api.parseMessage !== 'function') {
    throw new Error('protocol.js must load before state.js');
  }

  /** @param {any} sample @param {number} runId */
  function normalizeRaw(sample, runId) {
    return Object.freeze({
      kind: 'raw',
      runId,
      seq: sample.seq,
      ms: sample.ms,
      tempC: sample.temp === null ? Number.NaN : sample.temp,
      fault: sample.fault,
    });
  }

  /** @param {any} sample @param {number} runId @param {number} resolutionMs */
  function normalizeCoarse(sample, runId, resolutionMs) {
    return Object.freeze({
      kind: 'coarse',
      runId,
      ms: sample.ms,
      resolutionMs,
      averageC: sample.avg === null ? Number.NaN : sample.avg,
      minimumC: sample.min === null ? Number.NaN : sample.min,
      maximumC: sample.max === null ? Number.NaN : sample.max,
      fault: sample.fault,
    });
  }

  class TelemetryStore {
    constructor() {
      this.runId = 0;
      /** @type {Map<string, ReturnType<typeof normalizeRaw>>} */
      this.raw = new Map();
      /** @type {Map<string, ReturnType<typeof normalizeCoarse>>} */
      this.coarse = new Map();
      /** @type {ReadonlyArray<any> | null} */
      this.chartCache = null;
    }

    /** @param {number} runId */
    selectRun(runId) {
      if (this.runId === runId) return false;
      this.runId = runId;
      this.raw.clear();
      this.coarse.clear();
      this.chartCache = null;
      return true;
    }

    /** @param {any} sample @param {number} [runId] */
    mergeRaw(sample, runId = sample.runId) {
      if (runId !== this.runId) return false;
      const key = `${runId}:${sample.seq}`;
      if (this.raw.has(key)) return false;
      this.raw.set(key, normalizeRaw(sample, runId));
      this.chartCache = null;
      return true;
    }

    /** @param {any} sample @param {number} runId @param {number} resolutionMs */
    mergeCoarse(sample, runId, resolutionMs) {
      if (runId !== this.runId) return false;
      const key = `${runId}:${sample.ms}:${resolutionMs}`;
      if (this.coarse.has(key)) return false;
      this.coarse.set(key, normalizeCoarse(sample, runId, resolutionMs));
      this.chartCache = null;
      return true;
    }

    /** @param {number} runId @param {Map<string, any>} raw @param {Map<string, any>} coarse */
    replaceSnapshot(runId, raw, coarse) {
      this.runId = runId;
      this.raw.clear();
      this.coarse.clear();
      for (const sample of raw.values()) this.mergeRaw(sample, runId);
      for (const entry of coarse.values()) {
        this.mergeCoarse(entry.sample, runId, entry.resolutionMs);
      }
      this.chartCache = null;
    }

    getLatestRaw() {
      let latest = null;
      for (const sample of this.raw.values()) {
        if (!latest || sample.ms > latest.ms || (sample.ms === latest.ms && sample.seq > latest.seq)) {
          latest = sample;
        }
      }
      return latest;
    }

    /**
     * Returns sorted canonical points. A coarse bucket is omitted whenever any
     * raw sample falls inside its [start, start + resolution) interval.
     */
    getChartData() {
      if (this.chartCache) return this.chartCache;
      const raw = Array.from(this.raw.values()).sort((a, b) => a.ms - b.ms || a.seq - b.seq);
      const coarse = Array.from(this.coarse.values()).sort((a, b) => a.ms - b.ms || a.resolutionMs - b.resolutionMs);
      const visibleCoarse = [];
      let rawIndex = 0;
      for (const point of coarse) {
        while (rawIndex < raw.length && raw[rawIndex].ms < point.ms) rawIndex += 1;
        const overlapsRaw = rawIndex < raw.length && raw[rawIndex].ms < point.ms + point.resolutionMs;
        if (!overlapsRaw) visibleCoarse.push(point);
      }
      this.chartCache = Object.freeze((/** @type {any[]} */ ([...visibleCoarse, ...raw])).sort((a, b) => a.ms - b.ms));
      return this.chartCache;
    }
  }

  class SnapshotAssembler {
    constructor() {
      /** @type {null | {snapshotId:number, runId:number, throughSeq:number, raw:Map<string, any>, coarse:Map<string, any>}} */
      this.transaction = null;
    }

    get active() {
      return this.transaction !== null;
    }

    abort() {
      this.transaction = null;
    }

    /** @param {any} message */
    begin(message) {
      this.transaction = {
        snapshotId: message.snapshotId,
        runId: message.runId,
        throughSeq: message.throughSeq,
        raw: new Map(),
        coarse: new Map(),
      };
    }

    /** @param {any} message */
    addHistory(message) {
      const tx = this.transaction;
      if (!tx || tx.snapshotId !== message.snapshotId || tx.runId !== message.runId) return false;
      if (message.kind === 'raw') {
        for (const sample of message.samples) tx.raw.set(`${message.runId}:${sample.seq}`, sample);
      } else {
        for (const sample of message.samples) {
          tx.coarse.set(`${message.runId}:${sample.ms}:${message.resolutionMs}`, {
            sample,
            resolutionMs: message.resolutionMs,
          });
        }
      }
      return true;
    }

    /** @param {any} message */
    addCatchUp(message) {
      const tx = this.transaction;
      if (!tx || tx.runId !== message.runId) return false;
      tx.raw.set(`${message.runId}:${message.seq}`, message);
      return true;
    }

    /** @param {any} message @param {TelemetryStore} telemetry */
    end(message, telemetry) {
      const tx = this.transaction;
      if (!tx || tx.snapshotId !== message.snapshotId || tx.runId !== message.runId) return false;
      telemetry.replaceSnapshot(tx.runId, tx.raw, tx.coarse);
      this.transaction = null;
      return true;
    }
  }

  class AppStore {
    /** @param {{now?:()=>number, wallNow?:()=>number}} [options] */
    constructor(options = {}) {
      this.now = options.now || (() => performance.now());
      this.wallNow = options.wallNow || (() => Date.now());
      this.telemetry = new TelemetryStore();
      this.snapshot = new SnapshotAssembler();
      this.device = {
        protocol: api.PROTOCOL_VERSION,
        firmware: '',
        sampleHz: 2,
        recentWindowMs: 0,
        coarseResolutionMs: 30000,
        preset: '',
      };
      /** @type {{id:number, active:boolean, startedAt:number|null, elapsedMs:number}} */
      this.run = { id: 0, active: false, startedAt: null, elapsedMs: 0 };
      this.connection = { channel: 'none', phase: 'disconnected', attempt: 0 };
      /** @type {null | Readonly<{sample:ReturnType<typeof normalizeRaw>, receivedAt:number, source:'live'|'history'}>} */
      this.currentReading = null;
      this.lastError = null;
      /** @type {Set<(store:AppStore, reason:string)=>void>} */
      this.listeners = new Set();
    }

    /** @param {(store:AppStore, reason:string)=>void} listener */
    subscribe(listener) {
      this.listeners.add(listener);
      return () => this.listeners.delete(listener);
    }

    /** @param {string} reason */
    emit(reason) {
      for (const listener of this.listeners) listener(this, reason);
    }

    /** @param {string} channel @param {string} phase @param {number} [attempt] */
    setConnection(channel, phase, attempt = this.connection.attempt) {
      this.connection = { channel, phase, attempt };
      this.emit('connection');
    }

    /** @param {number} runId @param {boolean} active */
    updateRun(runId, active) {
      if (this.run.id !== runId) {
        this.telemetry.selectRun(runId);
        this.snapshot.abort();
        this.currentReading = null;
        this.run = { id: runId, active, startedAt: null, elapsedMs: 0 };
      } else {
        this.run.active = active;
      }
    }

    /** @param {any} message @param {number} receivedAt @param {number} wallReceivedAt */
    updateLatest(message, receivedAt, wallReceivedAt) {
      const normalized = normalizeRaw(message, message.runId);
      const previous = this.currentReading;
      if (!previous || previous.source !== 'live' || normalized.ms > previous.sample.ms || normalized.seq >= previous.sample.seq) {
        this.currentReading = Object.freeze({ sample: normalized, receivedAt, source: 'live' });
        this.run.elapsedMs = normalized.ms;
        if (this.run.active) this.run.startedAt = wallReceivedAt - normalized.ms;
      }
    }

    /** @param {string | Record<string, any>} input */
    handle(input) {
      const message = api.parseMessage(input);
      const receivedAt = this.now();
      const wallReceivedAt = this.wallNow();

      switch (message.type) {
        case 'hello':
          this.updateRun(message.runId, message.runActive);
          this.snapshot.abort();
          this.device = {
            protocol: message.protocol,
            firmware: message.firmware,
            sampleHz: message.sampleHz,
            recentWindowMs: message.recentWindowMs ?? this.device.recentWindowMs,
            coarseResolutionMs: message.coarseResolutionMs ?? this.device.coarseResolutionMs,
            preset: typeof message.preset === 'string' ? message.preset : this.device.preset,
          };
          this.emit('hello');
          break;
        case 'state':
          this.updateRun(message.runId, message.runActive);
          this.emit('state');
          break;
        case 'history.begin':
          if (message.runId !== this.run.id) break;
          this.snapshot.begin(message);
          this.connection.phase = 'replaying';
          this.emit('snapshot.begin');
          break;
        case 'history':
          if (this.snapshot.addHistory(message)) this.emit('snapshot.chunk');
          break;
        case 'history.end':
          if (this.snapshot.end(message, this.telemetry)) {
            const latest = this.telemetry.getLatestRaw();
            if (latest && (!this.currentReading || this.currentReading.source !== 'live')) {
              this.currentReading = Object.freeze({ sample: latest, receivedAt, source: 'history' });
              this.run.elapsedMs = latest.ms;
              if (this.run.active) this.run.startedAt = wallReceivedAt - latest.ms;
            }
            this.connection.phase = this.run.active ? 'live-active' : 'live-idle';
            this.emit('snapshot.end');
          }
          break;
        case 'sample':
          if (message.runId === 0) {
            const reading = normalizeRaw(message, message.runId);
            this.currentReading = Object.freeze({ sample: reading, receivedAt, source: 'live' });
          } else if (this.run.active && message.runId === this.run.id) {
            this.updateLatest(message, receivedAt, wallReceivedAt);
            if (this.snapshot.active) this.snapshot.addCatchUp(message);
            else this.telemetry.mergeRaw(message);
          } else {
            break;
          }
          this.emit('sample');
          break;
        case 'error':
          this.lastError = message.code;
          this.emit('error');
          break;
        default:
          break;
      }
      return message;
    }

    /** @param {number} [at] */
    getFreshness(at = this.now()) {
      const latest = this.currentReading;
      const ageMs = latest ? Math.max(0, at - latest.receivedAt) : Number.POSITIVE_INFINITY;
      const staleAfterMs = Math.max(3000, 3000 / this.device.sampleHz);
      const telemetry = !latest ? 'missing' : latest.source !== 'live' || ageMs > staleAfterMs ? 'stale' : 'fresh';
      const sensor = !latest
        ? 'unknown'
        : latest.sample.fault !== 0 || !Number.isFinite(latest.sample.tempC)
          ? 'faulted'
          : 'valid';
      return Object.freeze({
        connection: this.connection.phase,
        telemetry,
        sensor,
        ageMs,
        faults: latest ? api.decodeFaults(latest.sample.fault) : api.decodeFaults(0),
      });
    }
  }

  root.PyroVue = Object.assign(api, { TelemetryStore, SnapshotAssembler, AppStore });
})(/** @type {any} */ (globalThis));
