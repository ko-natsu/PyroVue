// @ts-check
(function bootApplication(root) {
  'use strict';

  if (!root.document) return;
  const api = root.PyroVue;
  if (!api || typeof api.AppStore !== 'function' || typeof api.DeviceTransport !== 'function') {
    throw new Error('PyroVue frontend modules loaded out of order');
  }
  if (typeof api.GraphRenderer !== 'function' || typeof api.MarkerStore !== 'function' ||
      typeof api.estimateConeProgress !== 'function' || typeof api.deriveHeatTheme !== 'function') {
    throw new Error('PyroVue graph, marker, cone and theme modules are required by the shell');
  }
  if (typeof api.CsvHttpClient !== 'function' || typeof api.formatNotesCsv !== 'function') {
    throw new Error('PyroVue csv module must load before the shell');
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
    coneShape: document.getElementById('coneShape'),
    conePrevious: document.getElementById('conePrevious'),
    coneCurrent: document.getElementById('coneCurrent'),
    coneNext: document.getElementById('coneNext'),
    coneBasis: document.getElementById('coneBasis'),
    canvas: document.getElementById('chart'),
    expandButton: document.getElementById('expandBtn'),
    sessionButton: document.getElementById('sessionBtn'),
    markerButton: document.getElementById('markerBtn'),
    markersToggle: document.getElementById('markersToggle'),
    markersPanel: document.getElementById('markersPanel'),
    markersClose: document.getElementById('markersClose'),
    markersList: document.getElementById('markersList'),
    markerEditor: document.getElementById('markerEditor'),
    markerTime: document.getElementById('markerTime'),
    markerNote: /** @type {HTMLTextAreaElement | null} */ (document.getElementById('markerNote')),
    markerDelete: document.getElementById('markerDelete'),
    unitButton: document.getElementById('unitBtn'),
    themeButton: document.getElementById('themeBtn'),
    elapsed: document.getElementById('elapsed'),
    started: document.getElementById('started'),
    inspectLine: document.getElementById('inspectLine'),
    toast: document.getElementById('toast'),
    csvPanel: document.getElementById('csvPanel'),
    csvTitle: document.getElementById('csvTitle'),
    csvStatusLine: document.getElementById('csvStatus'),
    csvDownloadButton: document.getElementById('csvDownloadBtn'),
    csvEraseButton: document.getElementById('csvEraseBtn'),
    csvFormatButton: document.getElementById('csvFormatBtn'),
    notesTitle: document.getElementById('notesTitle'),
    notesStatusLine: document.getElementById('notesStatus'),
    notesExportButton: document.getElementById('notesExportBtn'),
  };

  /** @type {Storage | null} */
  let storage = null;
  try {
    storage = root.localStorage;
  } catch {
    // Storage getter itself can throw in restricted browser contexts.
  }
  /** @param {string} key */
  function readPreference(key) {
    try { return storage?.getItem(key) ?? null; } catch { return null; }
  }
  /** @param {string} key @param {string} value */
  function savePreference(key, value) {
    try { storage?.setItem(key, value); } catch { /* Preference lasts for this page. */ }
  }
  let unit = readPreference('unit') === 'F' ? 'F' : 'C';
  /** @type {InstanceType<typeof api.MarkerStore> | null} */
  let markerStore = null;
  try {
    markerStore = new api.MarkerStore(storage);
  } catch {
    // Restricted storage must not prevent monitoring or pretend to save notes.
  }
  let markerRunId = store.run.id;
  /** @type {string | null} */
  let selectedMarkerId = null;
  /** @type {Array<{id:string,runId:number,ms:number,note:string}>} */
  let visibleMarkers = [];
  let pendingCommand = /** @type {'start' | 'stop' | null} */ (null);
  /** @type {ReturnType<typeof root.setTimeout> | null} */
  let commandSyncTimer = null;
  /** @type {ReturnType<typeof root.setTimeout> | null} */
  let commandDeadlineTimer = null;
  let destroyed = false;
  let expanded = false;
  /** @type {any} */
  let inspectedPoint = null;
  let renderPending = false;

  // ---- Device CSV storage (Feather single-run) + browser notes export ----
  // The device CSV run and the browser notes run are independent identities:
  // a stale saved run survives reboot and may carry a different runId than the
  // browser's current run. Temperature download/erase always bind to the
  // authoritative GET /api/csv runId; notes export binds to the current run.
  const csvClient = typeof root.fetch === 'function'
    ? new api.CsvHttpClient(root.fetch.bind(root))
    : null;
  /** @type {null | ReturnType<typeof api.parseCsvStatus>} */
  let csvStatus = null;
  /** @type {boolean | null} null until discovery; false hides device controls */
  let csvSupported = null;
  /** @type {string | null} */
  let csvStatusError = null;
  let csvBusy = false;
  let csvRefreshInFlight = false;
  let csvLastAttemptMs = 0;
  /** @type {ReturnType<typeof root.setInterval> | null} */
  let csvPollTimer = null;

  /** @param {number} runId */
  function fmtRunLabel(runId) {
    return `#${String(runId).padStart(4, '0')}`;
  }

  /** @param {number} bytes */
  function fmtBytes(bytes) {
    if (bytes < 1024) return `${bytes} B`;
    if (bytes >= 1048576) return `${(bytes / 1048576).toFixed(1)} MB`;
    return `${Math.round(bytes / 1024)} KB`;
  }

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

  /** @param {unknown} error */
  function errorMessage(error) {
    return error instanceof Error ? error.message : String(error);
  }

  async function refreshCsvStatus() {
    if (destroyed || !csvClient || csvRefreshInFlight || csvSupported === false) return;
    csvRefreshInFlight = true;
    try {
      const status = await csvClient.getStatus();
      if (!status.supported) {
        // 404 (or supported:false): this target has no device CSV storage.
        csvSupported = false;
        csvStatus = null;
        csvStatusError = null;
      } else {
        const firstDiscovery = csvSupported !== true;
        csvSupported = true;
        csvStatus = status;
        csvStatusError = null;
        // Feather supports the durable HTTP command queue; legacy targets
        // keep websocket-first commands forever.
        if (firstDiscovery) transport.preferHttpCommands = true;
      }
    } catch (error) {
      csvStatusError = errorMessage(error);
    } finally {
      csvLastAttemptMs = root.Date.now();
      csvRefreshInFlight = false;
      scheduleRender();
    }
  }

  function csvPollTick() {
    if (destroyed || !csvClient || csvSupported === false) return;
    // Modest cadence: 5s while a run is active so recording failures show up
    // quickly, 30s while idle so Start gating stays current.
    const due = store.run.active || root.Date.now() - csvLastAttemptMs >= 30000;
    if (due) void refreshCsvStatus();
  }

  function startCsvPolling() {
    if (csvPollTimer !== null || destroyed || !csvClient) return;
    csvPollTimer = root.setInterval(csvPollTick, 5000);
  }

  function stopCsvPolling() {
    if (csvPollTimer !== null) root.clearInterval(csvPollTimer);
    csvPollTimer = null;
  }

  /** @param {Blob} blob @param {string} filename */
  function triggerDownload(blob, filename) {
    const url = root.URL.createObjectURL(blob);
    const anchor = document.createElement('a');
    anchor.href = url;
    anchor.download = filename;
    anchor.rel = 'noopener';
    document.body.append(anchor);
    anchor.click();
    anchor.remove();
    root.setTimeout(() => root.URL.revokeObjectURL(url), 10000);
  }

  async function handleDownloadTemperatures() {
    const status = csvStatus;
    if (!csvClient || !status || csvBusy) return;
    const blocks = api.csvActionBlocks(status);
    if (blocks.download) {
      showToast(blocks.download);
      return;
    }
    // Bind to the authoritative device run id now: a reboot may leave a stale
    // saved run whose id differs from the browser's current run.
    const runId = status.runId;
    csvBusy = true;
    scheduleRender();
    try {
      const result = await csvClient.downloadRunCsv(runId);
      triggerDownload(result.blob, result.filename);
      showToast(`Downloaded ${result.filename}`);
    } catch (error) {
      showToast(errorMessage(error));
    } finally {
      csvBusy = false;
      scheduleRender();
      void refreshCsvStatus();
    }
  }

  async function handleEraseSavedRun() {
    const status = csvStatus;
    if (!csvClient || !status || csvBusy) return;
    const blocks = api.csvActionBlocks(status);
    if (blocks.erase) {
      showToast(blocks.erase);
      return;
    }
    const runId = status.runId;
    const label = runId > 0 ? `saved device run ${fmtRunLabel(runId)}` : 'saved device run (run ID unknown)';
    const advice = runId > 0
      ? 'Download the temperatures CSV and export your browser notes CSV first — erased data cannot be recovered.'
      : 'This file is incomplete or corrupt and cannot be downloaded. Erasing it clears the way for new recordings; anything still inside is lost.';
    const confirmed = root.confirm(`Erase the ${label}?\n\n${advice}`);
    if (!confirmed) return;
    csvBusy = true;
    scheduleRender();
    try {
      await csvClient.eraseRun(runId);
      showToast(`Erased the ${label} from the device`);
    } catch (error) {
      showToast(errorMessage(error));
    } finally {
      csvBusy = false;
      scheduleRender();
      await refreshCsvStatus();
    }
  }

  async function handleFormatStorage() {
    const status = csvStatus;
    if (!csvClient || !status || csvBusy || status.mounted) return;
    const confirmed = root.confirm(
      'Format the device data partition (ffat)?\n\n'
      + 'ALL data in the partition will be permanently erased — including any saved runs.',
    );
    if (!confirmed) return;
    csvBusy = true;
    scheduleRender();
    try {
      await csvClient.formatStorage();
      showToast('Device storage formatted');
    } catch (error) {
      showToast(errorMessage(error));
    } finally {
      csvBusy = false;
      scheduleRender();
      await refreshCsvStatus();
    }
  }

  function handleExportNotes() {
    if (!markerStore) {
      showToast('Marker storage unavailable in this browser');
      return;
    }
    // Browser-selected current run, independent of the device CSV run.
    const runId = store.run.id;
    if (runId <= 0) {
      showToast('No current run to export');
      return;
    }
    const markers = markerStore.list(runId);
    try {
      const text = api.formatNotesCsv(runId, markers);
      triggerDownload(
        new root.Blob([text], { type: 'text/csv;charset=utf-8' }),
        api.notesCsvFilename(runId),
      );
    } catch (error) {
      showToast(errorMessage(error));
      return;
    }
    if (api.notesNeedFormulaWarning(markers)) {
      showToast(`Exported ${markers.length} note${markers.length === 1 ? '' : 's'} for run ${fmtRunLabel(runId)} — import the note column as text in spreadsheets`);
    } else {
      showToast(`Exported ${markers.length} note${markers.length === 1 ? '' : 's'} for run ${fmtRunLabel(runId)}`);
    }
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
    let startBlock = null;
    if (!active && pendingCommand === null && csvClient && csvSupported !== false) {
      startBlock = csvStatusError
        ? 'Cannot verify device storage — wait for reconnection'
        : csvStatus ? api.csvActionBlocks(csvStatus).start : 'Checking device storage';
    }
    elements.sessionButton.disabled = pendingCommand !== null || startBlock !== null;
    elements.sessionButton.title = startBlock
      ?? (active ? 'Stop the current session' : 'Start a recording session');
    elements.sessionButton.dataset.intent = pendingCommand ? 'pending' : active ? 'stop' : 'start';
  }

  function renderCsvSection() {
    const supported = csvClient !== null && csvSupported !== false;
    if (elements.csvPanel) elements.csvPanel.hidden = !supported;
    const status = supported ? csvStatus : null;
    const blocks = status ? api.csvActionBlocks(status) : null;

    if (elements.csvTitle) {
      if (status && status.hasRun) {
        elements.csvTitle.textContent = status.runId > 0
          ? `Device CSV · run ${fmtRunLabel(status.runId)}`
          : 'Device CSV · saved run ID unknown';
      } else {
        elements.csvTitle.textContent = 'Device CSV';
      }
    }
    if (elements.csvStatusLine) {
      let text = 'Checking device storage…';
      let tone = '';
      if (!status) {
        if (csvStatusError) {
          text = `Device CSV status unavailable — ${csvStatusError}`;
          tone = 'error';
        }
      } else if (status.error || status.state === 'error') {
        text = `Device storage error: ${status.error || 'recording failed'}`;
        tone = 'error';
      } else if (!status.mounted) {
        text = 'Device storage not mounted — saved contents unknown; format erases the whole partition';
        tone = 'warn';
      } else if (status.recording) {
        text = `Recording to device every ${status.logIntervalMs / 1000}s · ${status.sampleCount} samples · ${fmtBytes(status.bytes)}`;
        tone = 'ok';
      } else if (status.hasRun) {
        const identity = status.runId > 0
          ? `run ${fmtRunLabel(status.runId)}`
          : 'run ID unknown (incomplete or corrupt)';
        text = `Saved device ${identity} (${status.state.replace('_', ' ')}) · ${status.sampleCount} samples · ${fmtBytes(status.bytes)}${blocks?.download ? '' : ' · ready to download'}`;
        tone = status.state === 'interrupted' || status.state === 'duration_limit' ? 'warn' : 'ok';
      } else if (status.freeBytes < status.startRequiredBytes) {
        text = `Storage low — ${fmtBytes(status.freeBytes)} free, ${fmtBytes(status.startRequiredBytes)} needed to start`;
        tone = 'warn';
      } else {
        text = `No saved run · ${fmtBytes(status.freeBytes)} free`;
      }
      if (status && csvStatusError) {
        text += ` · status refresh failing: ${csvStatusError}`;
        tone = 'warn';
      }
      elements.csvStatusLine.textContent = text;
      elements.csvStatusLine.dataset.tone = tone;
    }
    if (elements.csvDownloadButton) {
      elements.csvDownloadButton.disabled = csvBusy || !blocks || blocks.download !== null;
      elements.csvDownloadButton.title = blocks?.download
        ?? (status
          ? `Download run-${status.runId}-temperatures.csv for device run ${fmtRunLabel(status.runId)}`
          : 'No saved device run to download');
    }
    if (elements.csvEraseButton) {
      elements.csvEraseButton.disabled = csvBusy || !blocks || blocks.erase !== null;
      elements.csvEraseButton.title = blocks?.erase ?? 'Erase the saved device run after downloading both files';
    }
    if (elements.csvFormatButton) {
      elements.csvFormatButton.hidden = !(status && !status.mounted);
      elements.csvFormatButton.disabled = csvBusy;
      elements.csvFormatButton.title = 'Provision storage: erases the ENTIRE ffat data partition';
    }

    const notesRunId = store.run.id;
    if (elements.notesTitle) {
      elements.notesTitle.textContent = notesRunId > 0
        ? `Browser notes · run ${fmtRunLabel(notesRunId)}`
        : 'Browser notes';
    }
    if (elements.notesStatusLine) {
      let text = markerStore
        ? 'Saved in this browser only — never on the device'
        : 'Browser note storage unavailable — notes cannot be exported';
      let tone = markerStore ? '' : 'warn';
      if (markerStore && api.notesNeedFormulaWarning(visibleMarkers)) {
        text += ' · some notes start with =, +, -, or @ — import the note column as text in spreadsheets';
        tone = 'warn';
      }
      elements.notesStatusLine.textContent = text;
      elements.notesStatusLine.dataset.tone = tone;
    }
    if (elements.notesExportButton) {
      elements.notesExportButton.disabled = !markerStore || notesRunId <= 0;
      elements.notesExportButton.title = notesRunId > 0
        ? `Download run-${notesRunId}-notes.csv (browser notes for run ${fmtRunLabel(notesRunId)}, ${visibleMarkers.length} notes)`
        : 'No current run selected';
    }
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

  /** @param {number} progress */
  function coneBodyPath(progress) {
    const p = Math.max(0, Math.min(1, Number.isFinite(progress) ? progress : 0));
    const tipX = 36 + 42 * p;
    const tipY = 8 + 44 * p;
    return `M16 64 Q${26 + 18 * p} ${48 - 12 * p} ${tipX} ${tipY} Q${42 + 22 * p} 44 56 64 Z`;
  }

  /** @param {ReturnType<typeof api.estimateConeProgress>} model */
  function renderConeWidget(model) {
    if (!elements.coneWidget || !elements.coneLabel) return;
    const ready = model !== null;
    elements.coneWidget.dataset.model = ready ? 'ready' : 'unavailable';
    elements.coneLabel.textContent = ready ? model.label : 'Estimated cone —';
    elements.coneWidget.setAttribute('aria-label', ready
      ? `${model.label}, ${Math.round(model.progress * 100)}% progression`
      : 'Estimated cone unavailable');
    if (elements.coneShape) elements.coneShape.setAttribute('d', coneBodyPath(ready ? model.progress : 0));
    if (elements.conePrevious) elements.conePrevious.textContent = ready ? model.previousCone ?? '' : '';
    if (elements.coneCurrent) elements.coneCurrent.textContent = ready ? model.cone : '';
    if (elements.coneNext) elements.coneNext.textContent = ready ? model.nextCone ?? '' : '';
    if (elements.coneBasis) {
      elements.coneBasis.textContent = ready
        ? `${model.quality === 'measured-100c' ? 'Final 100°C rise' : 'Recent-rise estimate'} ${Math.round(model.rateCPerHour)}°C/hr · ${model.rateColumnCPerHour}°C/hr chart`
        : 'Awaiting sufficient valid firing history';
    }
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

  /** @param {boolean} open */
  function showMarkerPanel(open) {
    if (!elements.markersPanel || !elements.markersToggle) return;
    if (!open && elements.markersPanel.contains(document.activeElement)) {
      (elements.markersToggle.hidden ? elements.expandButton : elements.markersToggle)?.focus();
    }
    elements.markersPanel.hidden = !open;
    elements.markersToggle.setAttribute('aria-expanded', String(open));
    if (!open) {
      selectedMarkerId = null;
      if (elements.markerEditor) elements.markerEditor.hidden = true;
    }
  }

  function renderMarkerList() {
    const list = elements.markersList;
    if (!list) return;
    list.replaceChildren();
    for (const marker of visibleMarkers) {
      const button = document.createElement('button');
      button.type = 'button';
      button.className = 'btn marker-entry';
      button.textContent = `${fmtElapsed(marker.ms)} · ${marker.note || 'No note'}`;
      button.title = marker.note || 'No note';
      button.setAttribute('aria-current', String(marker.id === selectedMarkerId));
      button.addEventListener('click', () => inspectMarker(marker.id));
      list.append(button);
    }
    if (elements.markersToggle) {
      elements.markersToggle.hidden = visibleMarkers.length === 0;
      elements.markersToggle.textContent = `Markers (${visibleMarkers.length})`;
    }
    if (visibleMarkers.length === 0) showMarkerPanel(false);
  }

  function syncMarkers() {
    if (markerRunId !== store.run.id) {
      markerRunId = store.run.id;
      selectedMarkerId = null;
      showMarkerPanel(false);
      visibleMarkers = markerStore && markerRunId > 0 ? markerStore.list(markerRunId) : [];
      renderMarkerList();
    }
  }

  /** @param {string} id */
  function inspectMarker(id) {
    const marker = visibleMarkers.find((entry) => entry.id === id);
    if (!marker) return;
    selectedMarkerId = id;
    showMarkerPanel(true);
    renderMarkerList();
    if (elements.markerEditor) elements.markerEditor.hidden = false;
    if (elements.markerTime) elements.markerTime.textContent = `Marker ${fmtElapsed(marker.ms)}`;
    if (elements.markerNote) {
      elements.markerNote.value = marker.note;
      elements.markerNote.focus();
    }
  }

  function captureMarker() {
    if (!markerStore) {
      showToast('Marker storage unavailable in this browser');
      return;
    }
    const reading = store.currentReading;
    if (!store.run.active || !reading || reading.source !== 'live'
      || reading.sample.runId !== store.run.id || store.getFreshness().telemetry !== 'fresh') {
      showToast('Wait for a live session reading before adding a marker');
      return;
    }
    const ms = reading.sample.ms + Math.max(0, store.now() - reading.receivedAt);
    try {
      markerStore.add(store.run.id, ms);
      visibleMarkers = markerStore.list(store.run.id);
      renderMarkerList();
      scheduleRender();
      showToast(`Marker captured at ${fmtElapsed(ms)}`);
    } catch {
      showToast('Marker not saved — browser storage unavailable');
    }
  }

  const renderer = new api.GraphRenderer(elements.canvas, {
    onInspect: renderInspect,
    onMarkerInspect: (/** @type {{id?:string}} */ marker) => { if (marker.id) inspectMarker(marker.id); },
    onExpandedChange: (/** @type {any} */ next) => setExpanded(Boolean(next)),
    listenForResize: false,
  });

  function render() {
    const reconciled = reconcilePendingCommand();
    const freshness = store.getFreshness();
    syncMarkers();
    const run = store.run;
    const reading = store.currentReading;
    const latest = reading ? reading.sample : null;

    const liveForRun = run.active && reading?.source === 'live' && latest?.runId === run.id
      && freshness.telemetry === 'fresh' && freshness.sensor === 'valid'
      && freshness.connection !== 'disconnected' && freshness.connection !== 'connecting'
      && freshness.connection !== 'awaiting-state';
    const heat = api.deriveHeatTheme({
      active: run.active,
      fresh: liveForRun,
      valid: freshness.sensor === 'valid',
      tempC: latest?.tempC,
    });
    if (elements.app) {
      elements.app.dataset.heat = heat.stage;
      if (heat.stage === 'neutral') {
        elements.app.style.removeProperty('--heat-intensity');
        elements.app.style.removeProperty('--heat-field-opacity');
        elements.app.style.removeProperty('--heat-dither-opacity');
      } else {
        elements.app.style.setProperty('--heat-intensity', heat.intensity.toFixed(3));
        elements.app.style.setProperty('--heat-field-opacity', (0.24 * heat.intensity).toFixed(3));
        elements.app.style.setProperty('--heat-dither-opacity', (0.34 * heat.intensity).toFixed(3));
      }
    }
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
    renderCsvSection();
    if (elements.markerButton) {
      const ready = !!markerStore && run.active && reading?.source === 'live'
        && reading.sample.runId === run.id && freshness.telemetry === 'fresh';
      elements.markerButton.disabled = !ready;
      elements.markerButton.title = ready ? 'Capture a marker at the current run time' : 'Wait for a live session reading';
    }
    if (elements.unitButton) elements.unitButton.textContent = `°${unit}`;

    const points = store.telemetry.getChartData();
    renderConeWidget(api.estimateConeProgress(points));
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
      markers: visibleMarkers.map((marker) => ({ id: marker.id, ms: marker.ms, label: marker.note })),
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
      void transport.syncState().catch((/** @type {unknown} */ stateError) => {
        store.lastError = errorMessage(stateError);
        store.emit('transport.error');
      });
    } finally {
      void refreshCsvStatus();
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

  const savedTheme = readPreference('theme');
  /** @type {'light'|'dark'} */
  let theme = savedTheme === 'light' || savedTheme === 'dark'
    ? savedTheme
    : root.matchMedia?.('(prefers-color-scheme: dark)').matches ? 'dark' : 'light';
  applyTheme(theme);

  elements.sessionButton?.addEventListener('click', () => void handleSessionButton());
  elements.csvDownloadButton?.addEventListener('click', () => void handleDownloadTemperatures());
  elements.csvEraseButton?.addEventListener('click', () => void handleEraseSavedRun());
  elements.csvFormatButton?.addEventListener('click', () => void handleFormatStorage());
  elements.notesExportButton?.addEventListener('click', handleExportNotes);
  elements.markerButton?.addEventListener('click', captureMarker);
  elements.markersToggle?.addEventListener('click', () => showMarkerPanel(!!elements.markersPanel?.hidden));
  elements.markersClose?.addEventListener('click', () => showMarkerPanel(false));
  elements.markerEditor?.addEventListener('submit', (/** @type {Event} */ event) => {
    event.preventDefault();
    if (!selectedMarkerId || !markerStore) return;
    try {
      markerStore.update(store.run.id, selectedMarkerId, elements.markerNote?.value || '');
      visibleMarkers = markerStore.list(store.run.id);
      renderMarkerList();
      scheduleRender();
      showToast('Marker note saved');
    } catch {
      showToast('Marker note not saved — browser storage unavailable');
    }
  });
  elements.markerDelete?.addEventListener('click', () => {
    if (!selectedMarkerId || !markerStore) return;
    try {
      markerStore.remove(store.run.id, selectedMarkerId);
      selectedMarkerId = null;
      visibleMarkers = markerStore.list(store.run.id);
      renderMarkerList();
      (elements.markersToggle?.hidden ? elements.expandButton : elements.markersToggle)?.focus();
      if (elements.markerEditor) elements.markerEditor.hidden = true;
      scheduleRender();
      showToast('Marker deleted');
    } catch {
      showToast('Marker not deleted — browser storage unavailable');
    }
  });
  elements.unitButton?.addEventListener('click', () => {
    unit = unit === 'C' ? 'F' : 'C';
    savePreference('unit', unit);
    if (inspectedPoint) renderInspect(inspectedPoint);
    scheduleRender();
  });
  elements.themeButton?.addEventListener('click', () => {
    theme = theme === 'dark' ? 'light' : 'dark';
    savePreference('theme', theme);
    applyTheme(theme);
    scheduleRender();
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
    stopCsvPolling();
    transport.stop();
    renderer.destroy();
  }, { once: true });

  store.subscribe(scheduleRender);
  // Device CSV status refreshes on reconnect and whenever the session state
  // flips, so recording failures stay visible and Start gating stays honest.
  let lastConnectionPhase = store.connection.phase;
  let lastRunActive = store.run.active;
  store.subscribe(() => {
    const phase = store.connection.phase;
    if (phase !== lastConnectionPhase) {
      const wasDown = lastConnectionPhase === 'disconnected' || lastConnectionPhase === 'connecting';
      lastConnectionPhase = phase;
      if (wasDown && phase !== 'disconnected' && phase !== 'connecting') void refreshCsvStatus();
    }
    if (store.run.active !== lastRunActive) {
      lastRunActive = store.run.active;
      void refreshCsvStatus();
    }
  });
  document.fonts?.ready.then(scheduleRender);
  root.setInterval(scheduleRender, 1000);

  applyExpanded();
  renderConeWidget(null);
  renderMarkerList();
  root.pyroVueApp = Object.freeze({ store, transport, renderer, setExpanded });
  scheduleRender();
  transport.start();
  startCsvPolling();
  void refreshCsvStatus();
})(/** @type {any} */ (globalThis));
