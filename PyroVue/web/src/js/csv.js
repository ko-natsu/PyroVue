// @ts-check
// Device single-run CSV storage contract plus the browser-local notes CSV
// serializer. Device routes (Feather only, 404 elsewhere):
//   GET    /api/csv                 storage status
//   GET    /api/run.csv?runId=N     saved temperature CSV (N positive)
//   DELETE /api/run.csv?runId=N     explicit erase (N may be 0 only for the
//                                  known saved file with unknown/corrupt ID)
//   POST   /api/csv/format          {confirm:"ERASE_FFAT"} full partition erase
// Notes CSV export is browser-local, works offline and never touches the
// device. Pure parsing/serialization plus a small fetch client; no DOM here.
(function registerCsv(root) {
  'use strict';

  const api = root.PyroVue;
  if (!api || typeof api.ProtocolError !== 'function') {
    throw new Error('protocol.js must load before csv.js');
  }
  const ProtocolError = api.ProtocolError;

  const UINT32_MAX = 4294967295;
  const FORMAT_CONFIRM = 'ERASE_FFAT';
  const CSV_STATES = new Set([
    'idle',
    'recording',
    'complete',
    'interrupted',
    'duration_limit',
    'error',
  ]);
  // Frozen stable error literals from SingleRunCsv::Status::error.
  const CSV_ERROR_LITERALS = new Set([
    'mount_failed',
    'read_failed',
    'create_failed',
    'write_failed',
    'sync_failed',
    'storage_full',
    'remove_failed',
    'format_failed',
    'corrupt_saved_run',
    'invalid_sample',
  ]);
  const NOTES_CSV_COLUMNS = Object.freeze([
    'run_id',
    'marker_id',
    'elapsed_ms',
    'elapsed_hms',
    'time_basis',
    'note',
  ]);
  const NOTES_CSV_HEADER = NOTES_CSV_COLUMNS.join(',');
  // UTF-8 BOM so spreadsheet apps decode the file as UTF-8. Notes themselves
  // are exported byte-exact; the BOM is file-level encoding, not note content.
  const NOTES_CSV_BOM = '\uFEFF';
  // Leading characters that spreadsheet apps may treat as formulas. The
  // export NEVER rewrites user text; the UI warns to import as text instead.
  const FORMULA_LEADING = new Set(['=', '+', '-', '@', '\t', '\r', '\n']);

  /** @param {unknown} value @param {string} name */
  function requireObject(value, name) {
    if (!value || typeof value !== 'object' || Array.isArray(value)) {
      throw new ProtocolError(`${name} must be an object`);
    }
    return /** @type {Record<string, any>} */ (value);
  }

  /** @param {Record<string, any>} value @param {string} field */
  function requireBoolean(value, field) {
    if (typeof value[field] !== 'boolean') {
      throw new ProtocolError(`${field} must be a boolean`);
    }
  }

  /**
   * @param {Record<string, any>} value
   * @param {string} field
   * @param {number} min
   */
  function requireIntegerInRange(value, field, min) {
    const number = value[field];
    if (!Number.isSafeInteger(number) || number < min || number > UINT32_MAX) {
      throw new ProtocolError(`${field} must be an integer between ${min} and ${UINT32_MAX}`);
    }
  }

  /**
   * Parses one GET /api/csv response. All fields are required exactly as the
   * device contract declares them; the result is frozen.
   * @param {unknown} value
   */
  function parseCsvStatus(value) {
    const status = requireObject(value, 'csv status');
    requireBoolean(status, 'supported');
    requireBoolean(status, 'mounted');
    requireBoolean(status, 'hasRun');
    requireBoolean(status, 'recording');
    requireIntegerInRange(status, 'runId', 0);
    requireIntegerInRange(status, 'sampleCount', 0);
    requireIntegerInRange(status, 'bytes', 0);
    requireIntegerInRange(status, 'freeBytes', 0);
    if (typeof status.state !== 'string' || !CSV_STATES.has(status.state)) {
      throw new ProtocolError(`state must be one of ${Array.from(CSV_STATES).join('/')}`);
    }
    if (status.error !== null
      && (typeof status.error !== 'string' || !CSV_ERROR_LITERALS.has(status.error))) {
      throw new ProtocolError(`error must be null or a stable error literal (${Array.from(CSV_ERROR_LITERALS).join('/')})`);
    }
    requireIntegerInRange(status, 'logIntervalMs', 1);
    requireIntegerInRange(status, 'maxDurationMs', 1);
    requireIntegerInRange(status, 'startRequiredBytes', 1);
    return Object.freeze({
      supported: status.supported,
      mounted: status.mounted,
      hasRun: status.hasRun,
      recording: status.recording,
      runId: status.runId,
      sampleCount: status.sampleCount,
      bytes: status.bytes,
      freeBytes: status.freeBytes,
      state: status.state,
      error: status.error,
      logIntervalMs: status.logIntervalMs,
      maxDurationMs: status.maxDurationMs,
      startRequiredBytes: status.startRequiredBytes,
    });
  }

  /**
   * Per-control gating for one parsed CSV status. Each field is null when the
   * action is allowed, otherwise a short human reason. A saved file with
   * runId 0 means "saved run with unknown/corrupt ID" (failed start or reboot
   * recovery): it must stay erasable but is never downloadable and never
   * displayed as "run 0".
   * @param {Record<string, any>} status parsed CSV status
   */
  function csvActionBlocks(status) {
    const s = requireObject(status, 'csv status');
    const mounted = s.mounted === true;
    const recording = s.recording === true || s.state === 'recording';
    const hasRun = s.hasRun === true;
    const runId = s.runId;
    const bytes = s.bytes;
    let start = null;
    if (!mounted) {
      start = 'Device storage is not mounted';
    } else if (recording) {
      start = 'A device recording is still in progress';
    } else if (hasRun) {
      start = 'A saved device run must be erased before starting';
    } else if (s.error || s.state === 'error') {
      start = 'Device storage error — resolve it before starting';
    } else if (s.freeBytes < s.startRequiredBytes) {
      start = 'Not enough free device storage for a new run';
    }
    let download = null;
    if (!hasRun) {
      download = 'No saved device run to download';
    } else if (recording) {
      download = 'The device is still recording';
    } else if (runId === 0) {
      download = 'The saved file has an unknown or corrupt run ID';
    } else if (bytes === 0) {
      download = 'The saved file is empty';
    }
    let erase = null;
    if (!hasRun) {
      erase = 'No saved device run to erase';
    } else if (recording) {
      erase = 'The device is still recording';
    }
    return Object.freeze({
      start,
      download,
      erase,
      format: mounted ? 'Device storage is mounted' : null,
    });
  }

  /** @param {number} runId */
  function requireRunId(runId) {
    if (!Number.isSafeInteger(runId) || runId <= 0 || runId > UINT32_MAX) {
      throw new RangeError('Run id must be a positive 32-bit integer');
    }
    return runId;
  }

  /**
   * Erase additionally accepts 0, the explicit sentinel for the saved file
   * whose run ID could not be recovered. Only DELETE may use it.
   * @param {number} runId
   */
  function requireEraseRunId(runId) {
    if (!Number.isSafeInteger(runId) || runId < 0 || runId > UINT32_MAX) {
      throw new RangeError('Run id must be a non-negative 32-bit integer');
    }
    return runId;
  }

  /** @param {number} runId */
  function temperaturesCsvFilename(runId) {
    return `run-${requireRunId(runId)}-temperatures.csv`;
  }

  /** @param {number} runId */
  function notesCsvFilename(runId) {
    return `run-${requireRunId(runId)}-notes.csv`;
  }

  /** @param {number} ms */
  function formatElapsedHms(ms) {
    const totalSeconds = Math.floor(ms / 1000);
    const hours = Math.floor(totalSeconds / 3600);
    const minutes = Math.floor((totalSeconds % 3600) / 60);
    const seconds = totalSeconds % 60;
    return `${String(hours).padStart(2, '0')}:${String(minutes).padStart(2, '0')}:${String(seconds).padStart(2, '0')}`;
  }

  /**
   * RFC 4180 quoting: only fields containing comma, quote, CR or LF are
   * quoted, with doubled quotes. Content is never trimmed, prefixed or
   * otherwise rewritten.
   * @param {string} value
   */
  function escapeCsvField(value) {
    return /[",\r\n]/.test(value) ? `"${value.replace(/"/g, '""')}"` : value;
  }

  /**
   * Serializes one run's browser-local notes to CSV text (including the UTF-8
   * BOM). Rows are sorted by captured elapsed ms then marker id and only carry
   * the requested run's markers. Notes are exported exactly as stored —
   * including leading formula-like text, newlines, quotes and commas.
   * @param {number} runId
   * @param {ReadonlyArray<any>} markers
   * @returns {string}
   */
  function formatNotesCsv(runId, markers) {
    requireRunId(runId);
    if (!Array.isArray(markers)) throw new TypeError('markers must be an array');
    /** @type {Array<any>} */
    const rows = [];
    for (const marker of markers) {
      const entry = requireObject(marker, 'marker');
      if (typeof entry.id !== 'string' || entry.id.length === 0) {
        throw new TypeError('marker id must be a non-empty string');
      }
      if (!Number.isSafeInteger(entry.runId) || entry.runId <= 0) {
        throw new TypeError('marker runId must be a positive integer');
      }
      if (typeof entry.ms !== 'number' || !Number.isFinite(entry.ms) || entry.ms < 0) {
        throw new RangeError('marker timestamp must be finite and nonnegative');
      }
      if (typeof entry.note !== 'string') {
        throw new TypeError('marker note must be a string');
      }
      if (entry.runId !== runId) continue;
      rows.push(entry);
    }
    rows.sort((a, b) => a.ms - b.ms || (a.id < b.id ? -1 : a.id > b.id ? 1 : 0));
    const lines = [NOTES_CSV_HEADER];
    for (const marker of rows) {
      lines.push([
        String(runId),
        escapeCsvField(marker.id),
        String(marker.ms),
        formatElapsedHms(marker.ms),
        'estimated',
        escapeCsvField(marker.note),
      ].join(','));
    }
    return NOTES_CSV_BOM + lines.join('\r\n') + '\r\n';
  }

  /**
   * True when any note starts with a character spreadsheets may treat as a
   * formula. The UI warns to import the note column as text; saved notes are
   * never rewritten to be "safe".
   * @param {ReadonlyArray<any>} markers
   */
  function notesNeedFormulaWarning(markers) {
    return Array.isArray(markers) && markers.some((marker) => {
      const note = marker && typeof marker.note === 'string' ? marker.note : '';
      return note.length > 0 && FORMULA_LEADING.has(note[0]);
    });
  }

  const API_ERROR_BASELINES = {
    status: {
      404: 'Device CSV storage is not supported on this device',
      503: 'Device storage is unavailable',
    },
    download: {
      404: 'No saved run on the device',
      409: 'Device refused: run still recording or saved run changed — refresh and retry',
      503: 'Device storage is unavailable',
    },
    erase: {
      404: 'Saved run is already gone',
      409: 'Device refused: recording or download in progress, or the saved run changed',
      503: 'Device storage is unavailable',
    },
    format: {
      409: 'Device refused: a run is saved, recording, or downloading',
      503: 'Device storage format failed',
    },
  };

  /**
   * @param {number} status
   */
  function fallbackBaseline(status) {
    if (status === 404) return 'Not found on the device';
    if (status === 409) return 'Device refused: state changed — refresh and retry';
    if (status >= 500) return 'Device reported an internal error';
    return `Device rejected the request (HTTP ${status})`;
  }

  /**
   * Human-readable message for a failed CSV route: a mapped baseline for the
   * context/status pair, plus whatever detail the device returned in its error
   * JSON (or plain error body).
   * @param {'status' | 'download' | 'erase' | 'format'} context
   * @param {number} status HTTP status
   * @param {unknown} body parsed error body, if any
   */
  function describeApiError(context, status, body) {
    const map = /** @type {Record<number, string>} */ (API_ERROR_BASELINES[context] || {});
    const baseline = map[status] || fallbackBaseline(status);
    const detail = api.readErrorDetail(body);
    return detail && !baseline.includes(detail) ? `${baseline} — ${detail}` : baseline;
  }

  /**
   * Reads the body once: JSON when valid, otherwise plain text. Failed reads
   * reject the operation rather than pretending an acknowledgment arrived.
   * @param {any} response
   */
  async function readBody(response) {
    const text = await response.text();
    if (!text) return null;
    try {
      return JSON.parse(text);
    } catch {
      return text;
    }
  }

  class CsvHttpClient {
    /** @param {typeof fetch} fetchFn */
    constructor(fetchFn) {
      if (typeof fetchFn !== 'function') {
        throw new TypeError('CsvHttpClient requires a fetch function');
      }
      this.fetchFn = fetchFn;
    }

    /**
     * Discovers the device CSV contract. A 404 means this target does not
     * support device CSV storage (non-Feather); the UI hides device controls
     * but keeps the browser notes export.
     */
    async getStatus() {
      const response = await this.fetchFn('/api/csv', { cache: 'no-store' });
      if (response.status === 404) return Object.freeze({ supported: false });
      const body = await readBody(response);
      if (!response.ok) throw new Error(describeApiError('status', response.status, body));
      return parseCsvStatus(body);
    }

    /** @param {number} runId */
    temperaturesUrl(runId) {
      return `/api/run.csv?runId=${requireRunId(runId)}`;
    }

    /**
     * Downloads the saved temperature CSV snapshot. The device rejects active
     * or stale runs; callers bind the URL to the authoritative status runId at
     * click time. @param {number} runId
     */
    async downloadRunCsv(runId) {
      const response = await this.fetchFn(this.temperaturesUrl(runId), { cache: 'no-store' });
      if (!response.ok) {
        throw new Error(describeApiError('download', response.status, await readBody(response)));
      }
      if (typeof response.blob !== 'function') {
        throw new Error('Device download response has no body');
      }
      const blob = await response.blob();
      return Object.freeze({ blob, filename: temperaturesCsvFilename(runId) });
    }

    /**
     * Explicitly erases the saved run. runId 0 targets only the known saved
     * file whose ID could not be recovered; anything else must match exactly.
     * @param {number} runId
     */
    async eraseRun(runId) {
      const response = await this.fetchFn(`/api/run.csv?runId=${requireEraseRunId(runId)}`, {
        method: 'DELETE',
      });
      const body = await readBody(response);
      if (!response.ok) throw new Error(describeApiError('erase', response.status, body));
      if (!body || body.ok !== true) throw new Error('Device erase was not acknowledged');
      return true;
    }

    /**
     * Explicit provisioning only: erases the ENTIRE ffat data partition after
     * UI confirmation. The UI shows this button only while storage is
     * unmounted.
     */
    async formatStorage() {
      const response = await this.fetchFn('/api/csv/format', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ confirm: FORMAT_CONFIRM }),
      });
      const body = await readBody(response);
      if (!response.ok) throw new Error(describeApiError('format', response.status, body));
      if (!body || body.ok !== true) throw new Error('Device format was not acknowledged');
      return true;
    }
  }

  root.PyroVue = Object.assign(api, {
    parseCsvStatus,
    csvActionBlocks,
    formatNotesCsv,
    notesCsvFilename,
    temperaturesCsvFilename,
    notesNeedFormulaWarning,
    describeApiError,
    CsvHttpClient,
  });
})(/** @type {any} */ (globalThis));
