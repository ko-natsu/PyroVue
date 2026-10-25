// @ts-check
(function registerTransport(root) {
  'use strict';

  const api = root.PyroVue;
  if (!api || typeof api.AppStore !== 'function') {
    throw new Error('state.js must load before transport.js');
  }

  const MAX_COMMAND_BYTES = 256;
  const DEVICE_ID_PATTERN = /^[0-9a-f]{12}$/;
  const BOOT_ID_PATTERN = /^[0-9a-f]{16}$/;

  /** @param {string} text */
  function utf8ByteLength(text) {
    if (typeof root.TextEncoder === 'function') {
      return new root.TextEncoder().encode(text).length;
    }
    let bytes = 0;
    for (const character of text) {
      const codePoint = character.codePointAt(0) || 0;
      if (codePoint <= 0x7f) bytes += 1;
      else if (codePoint <= 0x7ff) bytes += 2;
      else if (codePoint <= 0xffff) bytes += 3;
      else bytes += 4;
    }
    return bytes;
  }

  class DeviceTransport {
    /**
     * @param {InstanceType<typeof api.AppStore>} store
     * @param {{WebSocket?:any, fetch?:typeof fetch, location?:{protocol:string,host:string}, setTimeout?:typeof setTimeout, clearTimeout?:typeof clearTimeout, setInterval?:typeof setInterval, clearInterval?:typeof clearInterval, reconnectBaseMs?:number, reconnectMaxMs?:number, pollMs?:number, wallNow?:()=>number}} [options]
     */
    constructor(store, options = {}) {
      this.store = store;
      this.WebSocketClass = options.WebSocket || root.WebSocket;
      this.fetchFn = options.fetch || root.fetch?.bind(root);
      this.location = options.location || root.location;
      this.setTimeoutFn = options.setTimeout || root.setTimeout.bind(root);
      this.clearTimeoutFn = options.clearTimeout || root.clearTimeout.bind(root);
      this.setIntervalFn = options.setInterval || root.setInterval.bind(root);
      this.clearIntervalFn = options.clearInterval || root.clearInterval.bind(root);
      this.wallNow = options.wallNow || (() => Date.now());
      this.reconnectBaseMs = options.reconnectBaseMs || 500;
      this.reconnectMaxMs = options.reconnectMaxMs || 10000;
      this.pollMs = options.pollMs || 1000;
      this.socket = null;
      this.reconnectTimer = null;
      this.pollTimer = null;
      this.polling = false;
      this.stopped = true;
      this.attempt = 0;
      /** @type {Promise<{deviceId:string, bootId:string, runId:number}> | null} */
      this.infoPromise = null;
    }

    start() {
      if (!this.stopped) return;
      this.stopped = false;
      this.attempt = 0;
      this.openSocket();
    }

    stop() {
      this.stopped = true;
      if (this.reconnectTimer !== null) this.clearTimeoutFn(this.reconnectTimer);
      if (this.pollTimer !== null) this.clearIntervalFn(this.pollTimer);
      this.reconnectTimer = null;
      this.pollTimer = null;
      if (this.socket) {
        const socket = this.socket;
        this.socket = null;
        socket.onopen = null;
        socket.onmessage = null;
        socket.onerror = null;
        socket.onclose = null;
        socket.close();
      }
      this.store.setConnection('none', 'disconnected', this.attempt);
    }

    openSocket() {
      if (this.stopped || !this.WebSocketClass || !this.location) {
        this.startPolling();
        return;
      }
      this.store.setConnection('websocket', 'connecting', this.attempt);
      const scheme = this.location.protocol === 'https:' ? 'wss:' : 'ws:';
      let socket;
      try {
        socket = new this.WebSocketClass(`${scheme}//${this.location.host}/ws`);
      } catch (error) {
        this.handleTransportError(error);
        this.startPolling();
        this.scheduleReconnect();
        return;
      }
      this.socket = socket;
      socket.onopen = () => {
        if (this.socket !== socket || this.stopped) return;
        this.attempt = 0;
        this.stopPolling();
        this.store.setConnection('websocket', 'awaiting-state', 0);
      };
      socket.onmessage = (/** @type {{data:string}} */ event) => {
        if (this.socket !== socket || this.stopped) return;
        try {
          this.store.handle(event.data);
        } catch (error) {
          this.handleTransportError(error);
        }
      };
      socket.onerror = () => {
        // The close event owns fallback and retry so each failure schedules once.
      };
      socket.onclose = () => {
        if (this.socket !== socket) return;
        this.socket = null;
        if (this.stopped) return;
        this.store.setConnection('http', 'polling-degraded', this.attempt);
        this.startPolling();
        this.scheduleReconnect();
      };
    }

    scheduleReconnect() {
      if (this.stopped || this.reconnectTimer !== null) return;
      const delay = Math.min(this.reconnectMaxMs, this.reconnectBaseMs * (2 ** this.attempt));
      this.attempt += 1;
      this.store.connection.attempt = this.attempt;
      this.reconnectTimer = this.setTimeoutFn(() => {
        this.reconnectTimer = null;
        this.openSocket();
      }, delay);
    }

    startPolling() {
      if (this.stopped || !this.fetchFn || this.pollTimer !== null) return;
      this.store.setConnection('http', 'polling-degraded', this.attempt);
      void this.pollOnce();
      this.pollTimer = this.setIntervalFn(() => void this.pollOnce(), this.pollMs);
    }

    stopPolling() {
      if (this.pollTimer !== null) this.clearIntervalFn(this.pollTimer);
      this.pollTimer = null;
    }

    async pollOnce() {
      if (this.polling || !this.fetchFn || this.stopped) return;
      this.polling = true;
      try {
        await this.requestState();
      } catch (error) {
        this.handleTransportError(error);
      } finally {
        this.polling = false;
      }
    }

    /**
     * Reconcile current device state once without changing connection mode or
     * creating a polling interval.
     */
    async syncState() {
      if (!this.fetchFn) throw new Error('no state transport available');
      await this.requestState();
    }

    async requestState() {
      const infoResponse = await this.fetchFn('/api/info', { cache: 'no-store' });
      if (!infoResponse.ok) throw new Error(`GET /api/info returned ${infoResponse.status}`);
      this.store.handle(await infoResponse.json());

      const sampleResponse = await this.fetchFn('/api/now', { cache: 'no-store' });
      if (!sampleResponse.ok) throw new Error(`GET /api/now returned ${sampleResponse.status}`);
      this.store.handle(await sampleResponse.json());
    }

    /** @param {unknown} error */
    handleTransportError(error) {
      this.store.lastError = error instanceof Error ? error.message : String(error);
      this.store.emit('transport.error');
    }

    /**
     * Snapshot the authoritative current device identity. This deliberately
     * reads the current-device profile (never a viewed archive) and is called
     * synchronously before a command is built.
     * @returns {{deviceId:string, bootId:string, runId:number} | null}
     */
    captureDeviceContext() {
      const device = this.store.device;
      const runId = this.store.run && this.store.run.id;
      if (!device
          || typeof device.deviceId !== 'string'
          || typeof device.bootId !== 'string'
          || !DEVICE_ID_PATTERN.test(device.deviceId)
          || !BOOT_ID_PATTERN.test(device.bootId)
          || typeof runId !== 'number'
          || !Number.isSafeInteger(runId)
          || runId < 0) {
        return null;
      }
      return Object.freeze({ deviceId: device.deviceId, bootId: device.bootId, runId });
    }

    /**
     * Unknown command context blocks until GET /api/info supplies the
     * authoritative hello. Concurrent callers share one request.
     * @returns {Promise<{deviceId:string, bootId:string, runId:number}>}
     */
    ensureDeviceContext() {
      if (!this.fetchFn) return Promise.reject(new Error('no command transport available'));
      if (this.infoPromise) return this.infoPromise;
      const pending = (async () => {
        const response = await this.fetchFn('/api/info', { cache: 'no-store' });
        let body;
        try {
          body = await response.json();
        } catch {
          throw new api.ProtocolError(`GET /api/info returned invalid JSON (HTTP ${response.status})`);
        }
        if (!response.ok) {
          const detail = api.readErrorDetail(body);
          throw new api.ProtocolError(
            `GET /api/info returned ${response.status}${detail ? `: ${detail}` : ''}`,
          );
        }
        this.store.handle(body);
        const context = this.captureDeviceContext();
        if (!context) {
          throw new api.ProtocolError('GET /api/info did not provide a usable device identity');
        }
        return context;
      })().finally(() => {
        this.infoPromise = null;
      });
      this.infoPromise = pending;
      return pending;
    }

    /**
     * @param {any} response
     * @param {string} requestId
     * @param {number | undefined} expectedTargetRunId
     * @param {boolean} [inactiveTarget]
     */
    async readCommandResult(response, requestId, expectedTargetRunId, inactiveTarget = false) {
      let body;
      try {
        body = await response.json();
      } catch {
        throw new api.ProtocolError(
          `command rejected (HTTP ${response.status}): response was not valid JSON`,
        );
      }

      if (!response.ok) {
        if (body && typeof body === 'object' && body.type === 'error') {
          const failure = api.parseError(body, requestId);
          throw new api.ProtocolError(
            `command rejected (HTTP ${response.status}): ${failure.code}: ${failure.message}`,
          );
        }
        if (body && typeof body === 'object' && body.type === 'command.result') {
          const failure = api.parseCommandResult(body, {
            requestId,
            targetRunId: expectedTargetRunId,
            inactiveTarget,
          });
          if (failure.ok === false) {
            throw new api.ProtocolError(`command rejected (HTTP ${response.status}): ${failure.errorCode}`);
          }
        }
        const detail = api.readErrorDetail(body);
        throw new api.ProtocolError(
          `command rejected (HTTP ${response.status})${detail ? `: ${detail}` : ''}`,
        );
      }

      const result = api.parseCommandResult(body, {
        requestId,
        targetRunId: expectedTargetRunId,
        inactiveTarget,
      });
      if (result.ok !== true) {
        throw new api.ProtocolError(`command rejected: ${result.errorCode}`);
      }
      return result;
    }

    /**
     * Commands always travel over POST /api/cmd, even while the websocket is
     * connected. Identity, bootId, latest runId and the device header are
     * captured once, before the request starts, so a device switch cannot
     * retarget an in-flight command.
     * @param {{type:'run.start'|'run.stop'|'preset', value?:string}} command
     */
    async send(command) {
      if (!command || typeof command !== 'object') {
        throw new api.ProtocolError('command must be an object');
      }
      const type = command.type;
      if (type !== 'run.start' && type !== 'run.stop' && type !== 'preset') {
        throw new api.ProtocolError(`invalid command ${type}`);
      }
      if (type === 'preset' && (typeof command.value !== 'string' || command.value.length === 0)) {
        throw new api.ProtocolError('invalid command preset');
      }
      if (!this.fetchFn) throw new Error('no command transport available');

      // Synchronous capture: if the device identity is already authoritative,
      // nothing below may rebind it after an await.
      let context = this.captureDeviceContext();
      if (!context) context = await this.ensureDeviceContext();

      const requestId = api.createOperationId();
      /** @type {{requestId:string, bootId:string, runId:number, wallStartUtcMs?:number}} */
      const commandContext = { requestId, bootId: context.bootId, runId: context.runId };
      if (type === 'run.start') commandContext.wallStartUtcMs = this.wallNow();
      const payload = api.createCommand(type, commandContext, command.value);
      const body = JSON.stringify(payload);
      if (utf8ByteLength(body) > MAX_COMMAND_BYTES) {
        throw new api.ProtocolError('command exceeds 256-byte device limit');
      }
      const headers = Object.freeze({
        'Content-Type': 'application/json',
        'X-PyroVue-Device-Id': context.deviceId,
      });

      const response = await this.fetchFn('/api/cmd', {
        method: 'POST',
        headers,
        body,
      });
      const expectedTargetRunId = type === 'run.stop'
        ? context.runId
        : type === 'preset' ? 0 : undefined;
      const result = await this.readCommandResult(
        response,
        requestId,
        expectedTargetRunId,
        type === 'run.stop',
      );
      if (type === 'run.start' && result.targetRunId < 1) {
        throw new api.ProtocolError('command result did not identify the started run');
      }
      return result;
    }
  }

  root.PyroVue = Object.assign(api, { DeviceTransport });
})(/** @type {any} */ (globalThis));
