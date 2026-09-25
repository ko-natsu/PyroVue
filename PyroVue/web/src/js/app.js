// @ts-check
(function bootApplication(root) {
  'use strict';

  if (!root.document) return;
  const api = root.PyroVue;
  if (!api || typeof api.AppStore !== 'function' || typeof api.DeviceTransport !== 'function') {
    throw new Error('PyroVue frontend modules loaded out of order');
  }
  if (typeof api.GraphRenderer !== 'function') {
    throw new Error('PyroVue.GraphRenderer is required by the shell');
  }

  const document = root.document;
  const store = new api.AppStore();
  const transport = new api.DeviceTransport(store);
  const elements = {
    app: document.getElementById('app'),
    runId: document.getElementById('runId'),
    runState: document.getElementById('runState'),
    clock: document.getElementById('clock'),
    bigTemp: document.getElementById('bigTemp'),
    tempUnit: document.getElementById('tempUnit'),
    rateLine: document.getElementById('rateLine'),
    connLine: document.getElementById('connLine'),
    freshLine: document.getElementById('freshLine'),
    faultLine: document.getElementById('faultLine'),
    coneWidget: document.getElementById('coneWidget'),
    coneLabel: document.getElementById('coneLabel'),
    canvas: document.getElementById('chart'),
    expandButton: document.getElementById('expandBtn'),
    sessionButton: document.getElementById('sessionBtn'),
    markerButton: document.getElementById('markerBtn'),
    unitButton: document.getElementById('unitBtn'),
    themeButton: document.getElementById('themeBtn'),
    elapsed: document.getElementById('elapsed'),
    started: document.getElementById('started'),
    inspectLine: document.getElementById('inspectLine'),
    toast: document.getElementById('toast'),
  };

  let unit = root.localStorage.getItem('unit') === 'F' ? 'F' : 'C';
  let pendingCommand = /** @type {'start' | 'stop' | null} */ (null);
  /** @type {ReturnType<typeof root.setTimeout> | null} */
  let commandSyncTimer = null;
  /** @type {ReturnType<typeof root.setTimeout> | null} */
  let commandDeadlineTimer = null;
  let destroyed = false;
  let expanded = false;
  /** @type {null | {currentCone:number, progress?:number}} */
  let coneModel = null;
  /** @type {any} */
  let inspectedPoint = null;
  let renderPending = false;

  /** @param {number} value */
  const toDisplayTemp = (value) => {
    const converted = unit === 'F' ? value * 9 / 5 + 32 : value;
    return converted >= 100 ? String(Math.round(converted)) : converted.toFixed(1);
  };

  /** @param {number} rateCPerHour */
  const toDisplayRate = (rateCPerHour) => {
    const converted = unit === 'F' ? rateCPerHour * 9 / 5 : rateCPerHour;
    return String(Math.round(converted));
  };

  /** @param {number} wallMs */
  function fmtClock(wallMs) {
    const date = new root.Date(wallMs);
    let hours = date.getHours();
    const minutes = String(date.getMinutes()).padStart(2, '0');
    const meridiem = hours >= 12 ? 'PM' : 'AM';
    hours = hours % 12 || 12;
    return `${hours}:${minutes} ${meridiem}`;
  }

  /** @param {number} runMs */
  function fmtElapsed(runMs) {
    const totalSeconds = Math.max(0, Math.floor(runMs / 1000));
    const hours = Math.floor(totalSeconds / 3600);
    const minutes = Math.floor((totalSeconds % 3600) / 60);
    const seconds = totalSeconds % 60;
    return `T+${String(hours).padStart(2, '0')}:${String(minutes).padStart(2, '0')}:${String(seconds).padStart(2, '0')}`;
  }

  /** @param {number} runMs Short run-clock form used by graph inspection. */
  function fmtRunClock(runMs) {
    const totalSeconds = Math.max(0, Math.floor(runMs / 1000));
    const hours = Math.floor(totalSeconds / 3600);
    const minutes = Math.floor((totalSeconds % 3600) / 60);
    const seconds = totalSeconds % 60;
    return `${hours}:${String(minutes).padStart(2, '0')}:${String(seconds).padStart(2, '0')}`;
  }

  /** @param {string} message */
  function showToast(message) {
    if (!elements.toast) return;
    elements.toast.textContent = message;
    elements.toast.classList.add('show');
    root.setTimeout(() => elements.toast?.classList.remove('show'), 1800);
  }

  function scheduleRender() {
    if (renderPending) return;
    renderPending = true;
    root.requestAnimationFrame(() => {
      renderPending = false;
      render();
    });
  }

  function clearCommandTimers() {
    if (commandSyncTimer !== null) root.clearTimeout(commandSyncTimer);
    if (commandDeadlineTimer !== null) root.clearTimeout(commandDeadlineTimer);
    commandSyncTimer = null;
    commandDeadlineTimer = null;
  }

  /** @returns {boolean} true when a pending command was just reconciled */
  function reconcilePendingCommand() {
    if (!pendingCommand) return false;
    const confirmed = pendingCommand === 'start' ? store.run.active : !store.run.active;
    if (!confirmed) return false;
    pendingCommand = null;
    clearCommandTimers();
    return true;
  }

  function scheduleCommandRecovery() {
    const hz = store.device.sampleHz;
    const delay = Number.isFinite(hz) && hz > 0
      ? Math.min(1000, Math.max(250, Math.round(1000 / hz)))
      : 1000;
    commandSyncTimer = root.setTimeout(() => {
      commandSyncTimer = null;
      if (!pendingCommand || destroyed) return;
      void transport.syncState().catch((/** @type {unknown} */ error) => {
        store.lastError = error instanceof Error ? error.message : String(error);
        store.emit('transport.error');
      });
    }, delay);
    commandDeadlineTimer = root.setTimeout(() => {
      commandDeadlineTimer = null;
      if (!pendingCommand || destroyed) return;
      pendingCommand = null;
      clearCommandTimers();
      renderSessionControl();
      showToast('Could not confirm session state. Check device connection and try again.');
    }, 5000);
  }

  function renderSessionControl() {
    if (!elements.sessionButton) return;
    const active = store.run.active;
    if (pendingCommand === 'start') elements.sessionButton.textContent = 'Starting…';
    else if (pendingCommand === 'stop') elements.sessionButton.textContent = 'Stopping…';
    else elements.sessionButton.textContent = active ? 'Stop Session' : 'Start Session';
    elements.sessionButton.disabled = pendingCommand !== null;
    elements.sessionButton.dataset.intent = pendingCommand ? 'pending' : active ? 'stop' : 'start';
  }

  /** @param {ReadonlyArray<any>} points */
  function renderRate(points) {
    if (!elements.rateLine) return;
    if (typeof api.calculateRateOfRise !== 'function') {
      elements.rateLine.hidden = true;
      return;
    }
    const result = api.calculateRateOfRise(points);
    if (!result || !Number.isFinite(result.rateCPerHour) || (result.sampleCount ?? 0) < 2) {
      elements.rateLine.hidden = true;
      return;
    }
    elements.rateLine.hidden = false;
    elements.rateLine.dataset.quality = typeof result.quality === 'string' ? result.quality : '';
    const arrow = result.rateCPerHour >= 0 ? '▲' : '▼';
    elements.rateLine.innerHTML = '';
    const arrowNode = document.createElement('span');
    arrowNode.setAttribute('aria-hidden', 'true');
    arrowNode.textContent = `${arrow} `;
    const valueNode = document.createElement('strong');
    valueNode.textContent = `${toDisplayRate(Math.abs(result.rateCPerHour))}°${unit}/hr`;
    elements.rateLine.append(arrowNode, valueNode);
  }

  function renderConnection() {
    if (!elements.connLine) return;
    const { channel, phase } = store.connection;
    let text = 'Connected';
    let tone = 'ok';
    if (phase === 'disconnected') { text = 'Disconnected — reconnecting'; tone = 'error'; }
    else if (phase === 'connecting') { text = 'Connecting…'; tone = 'warn'; }
    else if (phase === 'awaiting-state') { text = 'Connected — syncing'; tone = 'warn'; }
    else if (phase === 'replaying') { text = 'Syncing history…'; tone = 'warn'; }
    else if (phase === 'polling-degraded') { text = 'Connected · HTTP polling'; tone = 'warn'; }
    else if (channel === 'none') { text = 'Connecting…'; tone = 'warn'; }
    elements.connLine.textContent = text;
    elements.connLine.dataset.tone = tone;
  }

  /** @param {ReturnType<typeof api.decodeFaults>} faults */
  function renderFaultLine(faults) {
    if (!elements.faultLine) return;
    const labels = [];
    if (faults.openThermocouple) labels.push('Open thermocouple');
    if (faults.shortToGround) labels.push('Short to ground');
    if (faults.shortToVcc) labels.push('Short to VCC');
    if (faults.nonFinite) labels.push('Non-finite reading');
    if (faults.unknownMask) labels.push('Unknown fault bits');
    if (labels.length === 0) {
      elements.faultLine.hidden = true;
      return;
    }
    elements.faultLine.textContent = `Sensor fault: ${labels.join(' · ')}`;
    elements.faultLine.hidden = false;
  }

  function renderConeWidget() {
    if (!elements.coneWidget || !elements.coneLabel) return;
    const model = coneModel;
    const ready = !!model && typeof model === 'object' && Number.isFinite(/** @type {any} */ (model).currentCone);
    elements.coneWidget.dataset.model = ready ? 'ready' : 'placeholder';
    elements.coneLabel.textContent = ready ? `Cone ${Math.round(/** @type {any} */ (model).currentCone)}` : 'Cone —';
    const progress = ready ? /** @type {any} */ (model).progress : null;
    if (ready && Number.isFinite(progress)) {
      elements.coneWidget.style.setProperty('--cone-progress', String(Math.min(1, Math.max(0, progress))));
    } else {
      elements.coneWidget.style.removeProperty('--cone-progress');
    }
  }

  /** Milestone 6 seam: the widget renders normalized model data only.
   * @param {{currentCone:number, progress?:number} | null} model */
  function setConeModel(model) {
    coneModel = model;
    renderConeWidget();
  }

  function applyExpanded() {
    if (elements.app) elements.app.dataset.expanded = expanded ? 'true' : 'false';
    if (elements.expandButton) {
      elements.expandButton.setAttribute('aria-pressed', String(expanded));
      elements.expandButton.textContent = expanded ? 'Collapse' : 'Expand';
    }
  }

  /** @param {boolean} next */
  function setExpanded(next) {
    if (expanded === next) return;
    expanded = next;
    applyExpanded();
    root.requestAnimationFrame(() => {
      renderer.resize();
      scheduleRender();
    });
  }

  /** @param {any} payload */
  function renderInspect(payload) {
    inspectedPoint = payload;
    if (!elements.inspectLine) return;
    const ms = payload && typeof payload.ms === 'number' ? payload.ms : null;
    const tempC = payload && typeof payload.valueC === 'number' ? payload.valueC : null;
    if (ms === null && tempC === null) {
      elements.inspectLine.hidden = true;
      return;
    }
    const parts = [];
    if (ms !== null) parts.push(fmtRunClock(ms));
    if (tempC !== null) parts.push(`${toDisplayTemp(tempC)}°${unit}`);
    elements.inspectLine.textContent = parts.join(' · ');
    elements.inspectLine.hidden = false;
  }

  const renderer = new api.GraphRenderer(elements.canvas, {
    onInspect: renderInspect,
    onExpandedChange: (/** @type {any} */ next) => setExpanded(Boolean(next)),
    listenForResize: false,
  });

  function render() {
    const reconciled = reconcilePendingCommand();
    const freshness = store.getFreshness();
    const run = store.run;
    const reading = store.currentReading;
    const latest = reading ? reading.sample : null;


    if (elements.app) elements.app.dataset.run = run.active ? 'active' : 'idle';
    if (elements.runId) {
      elements.runId.textContent = run.id > 0 ? `Run #${String(run.id).padStart(4, '0')}` : 'Run #—';
    }
    if (elements.runState) {
      elements.runState.textContent = run.active ? 'ACTIVE' : 'IDLE';
      elements.runState.dataset.state = run.active ? 'active' : 'idle';
    }
    if (elements.clock) elements.clock.textContent = fmtClock(root.Date.now());

    if (elements.bigTemp) {
      const valid = !!latest && Number.isFinite(latest.tempC) && freshness.sensor !== 'faulted';
      elements.bigTemp.textContent = valid ? toDisplayTemp(latest.tempC) : '—';
    }
    if (elements.tempUnit) elements.tempUnit.textContent = `°${unit}`;

    renderConnection();
    if (elements.freshLine) {
      if (!run.active && reading?.source === 'history') {
        elements.freshLine.textContent = 'Last run reading';
        elements.freshLine.dataset.tone = '';
      } else if (freshness.telemetry === 'fresh') {
        elements.freshLine.textContent = `Live · ${store.device.sampleHz} Hz`;
        elements.freshLine.dataset.tone = 'ok';
      } else if (freshness.telemetry === 'stale') {
        elements.freshLine.textContent = `Data stale · ${Math.max(1, Math.round(freshness.ageMs / 1000))}s`;
        elements.freshLine.dataset.tone = 'warn';
      } else {
        elements.freshLine.textContent = 'Waiting for data…';
        elements.freshLine.dataset.tone = '';
      }
    }
    if (freshness.sensor === 'faulted') renderFaultLine(freshness.faults);
    else if (elements.faultLine) elements.faultLine.hidden = true;

    if (elements.elapsed) {
      const hasRun = run.startedAt !== null || run.elapsedMs > 0;
      elements.elapsed.textContent = hasRun ? fmtElapsed(run.elapsedMs) : 'T+--:--:--';
    }
    if (elements.started) {
      elements.started.textContent = run.startedAt !== null ? `Started ${fmtClock(run.startedAt)}` : 'Started —';
    }

    renderSessionControl();
    if (elements.unitButton) elements.unitButton.textContent = `°${unit}`;

    renderConeWidget();
    const points = store.telemetry.getChartData();
    renderRate(points);
    renderer.render({
      points,
      rateRegions: typeof api.segmentRateRegions === 'function'
        ? api.segmentRateRegions(points, {
          segmentMs: 30 * 60 * 1000,
          regionMinSamples: 2,
          regionMinDurationMs: 30000,
        })
        : [],
      markers: [],
      unit,
      expanded,
    });

    if (reconciled) showToast(run.active ? 'Session started' : 'Session stopped');
  }

  async function handleSessionButton() {
    if (pendingCommand) return;
    const starting = !store.run.active;
    pendingCommand = starting ? 'start' : 'stop';
    renderSessionControl();
    scheduleCommandRecovery();
    try {
      await transport.send(api.createCommand(starting ? 'run.start' : 'run.stop'));
    } catch (error) {
      clearCommandTimers();
      pendingCommand = null;
      renderSessionControl();
      showToast(error instanceof Error ? error.message : String(error));
    }
  }

  /** @param {'light'|'dark'} theme */
  function applyTheme(theme) {
    document.documentElement.dataset.theme = theme;
    if (elements.themeButton) {
      elements.themeButton.textContent = theme === 'dark' ? '\u2600' : '\u263E';
      elements.themeButton.setAttribute(
        'aria-label',
        theme === 'dark' ? 'Switch to light theme' : 'Switch to dark theme',
      );
    }
  }

  let theme = root.localStorage.getItem('theme');
  if (theme !== 'light' && theme !== 'dark') {
    theme = root.matchMedia?.('(prefers-color-scheme: dark)').matches ? 'dark' : 'light';
  }
  applyTheme(theme);

  elements.sessionButton?.addEventListener('click', () => void handleSessionButton());
  elements.markerButton?.addEventListener('click', () => {
    showToast('Event markers arrive in a later update');
  });
  elements.unitButton?.addEventListener('click', () => {
    unit = unit === 'C' ? 'F' : 'C';
    root.localStorage.setItem('unit', unit);
    if (inspectedPoint) renderInspect(inspectedPoint);
    scheduleRender();
  });
  elements.themeButton?.addEventListener('click', () => {
    theme = theme === 'dark' ? 'light' : 'dark';
    root.localStorage.setItem('theme', theme);
    applyTheme(theme);
  });
  elements.expandButton?.addEventListener('click', () => setExpanded(!expanded));
  document.addEventListener('keydown', (/** @type {KeyboardEvent} */ event) => {
    if (event.key === 'Escape' && expanded) setExpanded(false);
  });
  root.addEventListener('resize', () => {
    renderer.resize();
    scheduleRender();
  }, { passive: true });
  root.addEventListener('pagehide', () => {
    destroyed = true;
    clearCommandTimers();
    transport.stop();
    renderer.destroy();
  }, { once: true });

  store.subscribe(scheduleRender);
  root.setInterval(scheduleRender, 1000);

  applyExpanded();
  renderConeWidget();
  root.pyroVueApp = Object.freeze({ store, transport, renderer, setConeModel, setExpanded });
  scheduleRender();
  transport.start();
})(/** @type {any} */ (globalThis));
