// @ts-check
(function registerTransport(root) {
  'use strict';

  const api = root.PyroVue;
  if (!api || typeof api.AppStore !== 'function') {
    throw new Error('state.js must load before transport.js');
  }

  class DeviceTransport {
    /**
     * @param {InstanceType<typeof api.AppStore>} store
     * @param {{WebSocket?:any, fetch?:typeof fetch, location?:{protocol:string,host:string}, setTimeout?:typeof setTimeout, clearTimeout?:typeof clearTimeout, setInterval?:typeof setInterval, clearInterval?:typeof clearInterval, reconnectBaseMs?:number, reconnectMaxMs?:number, pollMs?:number}} [options]
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
      this.reconnectBaseMs = options.reconnectBaseMs || 500;
      this.reconnectMaxMs = options.reconnectMaxMs || 10000;
      this.pollMs = options.pollMs || 1000;
      this.socket = null;
      this.reconnectTimer = null;
      this.pollTimer = null;
      this.polling = false;
      this.stopped = true;
      this.attempt = 0;
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

    /** @param {Readonly<{type:string, value?:string}>} command */
    async send(command) {
      const validated = api.createCommand(command.type, command.value);
      const payload = JSON.stringify(validated);
      if (payload.length > 256) throw new api.ProtocolError('command exceeds 256-byte device limit');

      const openState = this.WebSocketClass && this.WebSocketClass.OPEN !== undefined
        ? this.WebSocketClass.OPEN
        : 1;
      if (this.socket && this.socket.readyState === openState) {
        this.socket.send(payload);
        return Object.freeze({ channel: 'websocket', pending: true });
      }
      if (!this.fetchFn) throw new Error('no command transport available');
      const response = await this.fetchFn('/api/cmd', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: payload,
      });
      if (!response.ok) throw new Error(`POST /api/cmd returned ${response.status}`);
      return Object.freeze({ channel: 'http', pending: true });
    }
  }

  root.PyroVue = Object.assign(api, { DeviceTransport });
})(/** @type {any} */ (globalThis));
