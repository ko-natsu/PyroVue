// @ts-check
(function registerGraph(root) {
  'use strict';

  const api = root.PyroVue;
  if (!api || typeof api.TelemetryStore !== 'function') {
    throw new Error('state.js must load before graph.js');
  }

  const MINUTE_MS = 60000;
  const HOUR_MS = 3600000;

  /**
   * Human-friendly run-time steps, smallest first. Tick intervals stay on this
   * grid so the live curve ends between ticks instead of dragging the axis.
   * @type {ReadonlyArray<number>}
   */
  const TIME_STEPS_MS = Object.freeze([
    10 * 1000,
    15 * 1000,
    30 * 1000,
    MINUTE_MS,
    2 * MINUTE_MS,
    5 * MINUTE_MS,
    10 * MINUTE_MS,
    15 * MINUTE_MS,
    30 * MINUTE_MS,
    HOUR_MS,
    2 * HOUR_MS,
    3 * HOUR_MS,
    6 * HOUR_MS,
    12 * HOUR_MS,
    24 * HOUR_MS,
  ]);

  /**
   * Fallback palette. The shell can override any entry with a CSS custom
   * property `--pv-graph-<key>` on the canvas for light/dark theming.
   * @type {{grid:string, axis:string, raw:string, coarse:string, band:string, marker:string, inspect:string, inspectText:string, rateCool:string, rateMid:string, rateHot:string}}
   */
  const DEFAULT_COLORS = Object.freeze({
    grid: 'rgba(128,132,148,0.22)',
    axis: '#7d8494',
    raw: '#d05138',
    coarse: '#c28a3f',
    band: 'rgba(194,138,63,0.20)',
    marker: '#b04ad6',
    inspect: '#7d8494',
    inspectText: '#262b36',
    rateCool: '#78b5a4',
    rateMid: '#dcc45e',
    rateHot: '#dd6a45',
  });

  /** @type {ReadonlyArray<keyof typeof DEFAULT_COLORS>} */
  const COLOR_KEYS = Object.freeze([
    'grid', 'axis', 'raw', 'coarse', 'band', 'marker', 'inspect', 'inspectText',
    'rateCool', 'rateMid', 'rateHot',
  ]);

  // ---------------------------------------------------------------------------
  // Pure axis / domain helpers (no canvas, no DOM, deterministic)
  // ---------------------------------------------------------------------------

  /**
   * Snaps a raw interval onto the 1 / 2 / 2.5 / 5 ladder.
   * @param {number} rawStep
   * @returns {number}
   */
  function niceStep(rawStep) {
    if (!Number.isFinite(rawStep) || rawStep <= 0) return 1;
    const magnitude = Math.pow(10, Math.floor(Math.log10(rawStep)));
    const fraction = rawStep / magnitude;
    if (fraction <= 1) return magnitude;
    if (fraction <= 2) return 2 * magnitude;
    if (fraction <= 2.5) return 2.5 * magnitude;
    if (fraction <= 5) return 5 * magnitude;
    return 10 * magnitude;
  }

  /**
   * Compact clock label for a run-relative duration.
   * Without a step: `m:ss` under one hour (`4:05`), `h:mm` at or above it
   * (`3:35`). With an axis step of a minute or more, labels stay on the
   * `h:mm` scale so 30 minutes reads `0:30`, keeping one consistent scale
   * per graph.
   * @param {number} ms
   * @param {number} [stepMs] tick interval of the axis being labelled
   * @returns {string}
   */
  function formatDuration(ms, stepMs) {
    const totalSeconds = Math.max(0, Math.round(ms / 1000));
    const hours = Math.floor(totalSeconds / 3600);
    const minutes = Math.floor((totalSeconds % 3600) / 60);
    const seconds = totalSeconds % 60;
    if (stepMs !== undefined && stepMs >= MINUTE_MS) {
      return `${hours}:${String(minutes).padStart(2, '0')}`;
    }
    if (hours > 0) return `${hours}:${String(minutes).padStart(2, '0')}`;
    return `${minutes}:${String(seconds).padStart(2, '0')}`;
  }

  /**
   * Stable rounded time domain: starts at 0 and ends on the next step boundary
   * at or after the newest sample, so the current runtime never becomes a tick
   * (a 3:35 run gets a 0:00..4:00 hourly axis). Falls back to 0..5min with no
   * data.
   * @param {number} maxMs newest run-relative sample time
   * @param {{maxTicks?: number}} [options]
   * @returns {{startMs:number, endMs:number, stepMs:number}}
   */
  function computeTimeDomain(maxMs, options = {}) {
    const maxTicks = Math.max(2, Math.floor(options.maxTicks ?? 8));
    if (!(maxMs > 0) || !Number.isFinite(maxMs)) {
      return { startMs: 0, endMs: 5 * MINUTE_MS, stepMs: MINUTE_MS };
    }
    let stepMs = TIME_STEPS_MS[TIME_STEPS_MS.length - 1];
    for (const candidate of TIME_STEPS_MS) {
      // Labels span 0..end inclusive; keep end/candidate <= maxTicks - 1.
      if (Math.ceil(maxMs / candidate) <= maxTicks - 1) {
        stepMs = candidate;
        break;
      }
    }
    return { startMs: 0, endMs: Math.ceil(maxMs / stepMs) * stepMs, stepMs };
  }

  /**
   * @param {{startMs:number, endMs:number, stepMs:number}} domain
   * @returns {{ms:number, label:string}[]}
   */
  function computeTimeTicks(domain) {
    /** @type {{ms:number, label:string}[]} */
    const ticks = [];
    for (let ms = domain.startMs; ms <= domain.endMs; ms += domain.stepMs) {
      ticks.push({ ms, label: formatDuration(ms, domain.stepMs) });
    }
    return ticks;
  }

  /**
   * Canonical temperatures are Celsius; display conversion never touches
   * source points.
   * @param {number} celsius
   * @param {'C'|'F'} unit
   * @returns {number}
   */
  function convertTemperature(celsius, unit) {
    return unit === 'F' ? celsius * 9 / 5 + 32 : celsius;
  }

  /**
   * @param {number} celsiusPerHour
   * @param {'C'|'F'} unit
   * @returns {number}
   */
  function convertRate(celsiusPerHour, unit) {
    return unit === 'F' ? celsiusPerHour * 9 / 5 : celsiusPerHour;
  }

  /**
   * Rate tiers are judged on the canonical °C/hr value so tiering is stable
   * across display units.
   * @param {number} rateCPerHour
   * @returns {'cool'|'mid'|'hot'}
   */
  function rateTier(rateCPerHour) {
    if (!(rateCPerHour > 60)) return 'cool';
    if (rateCPerHour <= 120) return 'mid';
    return 'hot';
  }

  /**
   * Sensible rounded Y domain over already-converted display values. Includes
   * every supplied value (callers pass coarse min/max too), snaps bounds onto
   * the nice ladder, and zooms instead of pinning to zero when the data sits
   * far above it.
   * @param {ReadonlyArray<number>} values display-unit finite numbers
   * @param {{targetTicks?: number, includeZero?: boolean}} [options]
   * @returns {{min:number, max:number, step:number, ticks:{value:number,label:string}[]}}
   */
  function computeTemperatureDomain(values, options = {}) {
    const targetTicks = Math.min(16, Math.max(2, Math.floor(options.targetTicks ?? 6)));
    let lo = Number.POSITIVE_INFINITY;
    let hi = Number.NEGATIVE_INFINITY;
    for (const value of values) {
      if (!Number.isFinite(value)) continue;
      if (value < lo) lo = value;
      if (value > hi) hi = value;
    }
    if (!Number.isFinite(lo) || !Number.isFinite(hi)) {
      lo = 0;
      hi = 100;
    }
    let span = hi - lo;
    if (!(span > 0)) span = Math.max(Math.abs(hi) * 0.1, 10);
    const includeZero = options.includeZero ?? (lo > 0 && lo < span);

    let step = niceStep(span / targetTicks);
    let low = includeZero ? 0 : Math.floor(lo / step) * step;
    let high = Math.ceil(hi / step) * step;
    if (high <= low) {
      low -= step;
      high += step;
    }
    if (Math.round((high - low) / step) > targetTicks + 2) {
      step = niceStep((high - low) / targetTicks);
      low = includeZero ? 0 : Math.floor(lo / step) * step;
      high = Math.ceil(hi / step) * step;
      if (high <= low) high = low + step;
    }

    /** @type {{value:number, label:string}[]} */
    const ticks = [];
    const count = Math.max(1, Math.round((high - low) / step));
    for (let index = 0; index <= count; index += 1) {
      const value = low + index * step;
      ticks.push({
        value,
        label: step >= 1 ? String(Math.round(value)) : value.toFixed(1),
      });
    }
    return { min: low, max: high, step, ticks };
  }

  /**
   * @param {number} value display-unit temperature
   * @returns {string}
   */
  function formatTemperature(value) {
    if (!Number.isFinite(value)) return '—';
    return Math.abs(value) >= 100 ? String(Math.round(value)) : String(Math.round(value * 10) / 10);
  }

  // ---------------------------------------------------------------------------
  // Pure series building: validity, gap segmentation, display-unit view
  // ---------------------------------------------------------------------------

  /**
   * A view-model copy of a canonical point. Canonical Celsius values are kept
   * alongside display-unit values; source points are never mutated.
   * @typedef {{ms:number, kind:'raw'|'coarse', tempC:number, value:number,
   *            minimumC:number, maximumC:number, minValue:number, maxValue:number,
   *            band:boolean}} PointModel
   */

  /**
   * Raw points are valid only when un-faulted with a finite temperature.
   * @param {any} point
   * @returns {boolean}
   */
  function isRawValid(point) {
    return point.fault === 0 && Number.isFinite(point.tempC);
  }

  /**
   * A faulted or non-finite coarse bucket is invalid as a whole.
   * @param {any} point
   * @returns {boolean}
   */
  function isCoarseValid(point) {
    return point.fault === 0 && Number.isFinite(point.averageC);
  }

  /**
   * Splits canonical chart data into drawable segments per layer. Any invalid
   * (faulted or non-finite) point breaks its layer's segment so gaps stay
   * visible rather than being drawn through. Returns display-unit models plus
   * every finite display value (coarse extrema included) for Y scaling.
   * @param {ReadonlyArray<any>} points canonical chart data from the store
   * @param {{unit?: 'C'|'F'}} [options]
   * @returns {{rawSegments:PointModel[][], coarseSegments:PointModel[][], temperatureValues:number[], maxMs:number}}
   */
  function buildSeries(points, options = {}) {
    const unit = options.unit === 'F' ? 'F' : 'C';
    const convert = (/** @type {number} */ celsius) => convertTemperature(celsius, unit);
    /** @type {PointModel[][]} */
    const rawSegments = [];
    /** @type {PointModel[][]} */
    const coarseSegments = [];
    /** @type {number[]} */
    const temperatureValues = [];
    let maxMs = 0;
    /** @type {PointModel[]} */
    let rawRun = [];
    /** @type {number|null} */
    let lastRawSequence = null;
    /** @type {PointModel[]} */
    let coarseRun = [];
    /** @type {number|null} */
    let lastCoarseEndMs = null;

    const flushRaw = () => {
      if (rawRun.length) rawSegments.push(rawRun);
      rawRun = [];
      lastRawSequence = null;
    };
    const flushCoarse = () => {
      if (coarseRun.length) coarseSegments.push(coarseRun);
      coarseRun = [];
      lastCoarseEndMs = null;
    };

    const ordered = Array.from(points).sort((a, b) => a.ms - b.ms);
    for (const point of ordered) {
      if (Number.isFinite(point.ms) && point.ms > maxMs) maxMs = point.ms;
      if (point.kind === 'coarse') {
        if (!isCoarseValid(point)) {
          flushCoarse();
          continue;
        }
        const averageC = point.averageC;
        const minimumC = Number.isFinite(point.minimumC) ? point.minimumC : averageC;
        const maximumC = Number.isFinite(point.maximumC) ? point.maximumC : averageC;
        const resolutionMs = Number.isFinite(point.resolutionMs) && point.resolutionMs > 0
          ? point.resolutionMs
          : 0;
        if (lastCoarseEndMs !== null && point.ms > lastCoarseEndMs) flushCoarse();
        /** @type {PointModel} */
        const model = {
          ms: point.ms,
          kind: 'coarse',
          tempC: averageC,
          value: convert(averageC),
          minimumC,
          maximumC,
          minValue: convert(minimumC),
          maxValue: convert(maximumC),
          band: Number.isFinite(point.minimumC) && Number.isFinite(point.maximumC),
        };
        temperatureValues.push(model.value);
        if (model.band) temperatureValues.push(model.minValue, model.maxValue);
        coarseRun.push(model);
        lastCoarseEndMs = point.ms + resolutionMs;
      } else {
        if (!isRawValid(point)) {
          flushRaw();
          continue;
        }
        if (
          lastRawSequence !== null
          && Number.isInteger(point.seq)
          && point.seq > lastRawSequence + 1
        ) {
          flushRaw();
        }
        /** @type {PointModel} */
        const model = {
          ms: point.ms,
          kind: 'raw',
          tempC: point.tempC,
          value: convert(point.tempC),
          minimumC: Number.NaN,
          maximumC: Number.NaN,
          minValue: Number.NaN,
          maxValue: Number.NaN,
          band: false,
        };
        temperatureValues.push(model.value);
        rawRun.push(model);
        lastRawSequence = Number.isInteger(point.seq) ? point.seq : null;
      }
    }
    flushRaw();
    flushCoarse();

    return { rawSegments, coarseSegments, temperatureValues, maxMs };
  }

  /**
   * Inspection selection: the nearest valid point in time (x) within a
   * touch-friendly radius. Invalid points never appear in `models`, so they
   * can never be selected.
   * @template T
   * @param {ReadonlyArray<(T & {ms:number, x:number})>} models valid point models with pixel x
   * @param {number} xPx pointer x in CSS pixels
   * @param {number} radiusPx
   * @returns {(T & {ms:number, x:number}) | null}
   */
  function pickPoint(models, xPx, radiusPx) {
    if (!Number.isFinite(xPx)) return null;
    /** @type {(T & {ms:number, x:number}) | null} */
    let best = null;
    let bestDistance = Number.POSITIVE_INFINITY;
    for (const model of models) {
      const distance = Math.abs(model.x - xPx);
      if (distance < bestDistance || (distance === bestDistance && best && model.ms < best.ms)) {
        best = model;
        bestDistance = distance;
      }
    }
    return best && bestDistance <= radiusPx ? best : null;
  }

  // ---------------------------------------------------------------------------
  // Renderer
  // ---------------------------------------------------------------------------

  /**
   * @typedef {Object} InspectPayload
   * @property {number} ms run-relative sample time
   * @property {'raw'|'coarse'} kind
   * @property {'C'|'F'} unit
   * @property {number} x pixel x of the point
   * @property {number} y pixel y of the point
   * @property {number} valueC canonical Celsius value (raw temp or coarse avg)
   * @property {number} value display-unit value
   * @property {number|null} minimumC canonical coarse minimum (null for raw)
   * @property {number|null} maximumC canonical coarse maximum (null for raw)
   */

  /**
   * @typedef {Object} GraphRenderPayload
   * @property {ReadonlyArray<any>} [points] canonical chart data from the store
   * @property {ReadonlyArray<{startMs:number, endMs:number, rateCPerHour:number, quality?:string, sampleCount?:number}>} [rateRegions]
   * @property {ReadonlyArray<{ms:number, label?:string}>} [markers]
   * @property {'C'|'F'} [unit]
   * @property {boolean} [expanded]
   */

  /**
   * @typedef {Object} GraphRendererOptions
   * @property {(payload: InspectPayload | null) => void} [onInspect]
   * @property {(expanded: boolean) => void} [onExpandedChange]
   * @property {'C'|'F'} [unit]
   * @property {number} [hitRadiusPx]
   * @property {Partial<typeof DEFAULT_COLORS>} [colors]
   * @property {boolean} [listenForResize]
   */

  /**
   * @typedef {{plotLeft:number, plotTop:number, plotRight:number, plotBottom:number}} PlotRect
   */

  class GraphRenderer {
    /**
     * @param {HTMLCanvasElement} canvas
     * @param {GraphRendererOptions} [options]
     */
    constructor(canvas, options = {}) {
      this.canvas = canvas;
      this.options = options;
      /** @type {'C'|'F'} */
      this.unit = options.unit === 'F' ? 'F' : 'C';
      this.hitRadiusPx = Math.max(12, options.hitRadiusPx ?? 32);
      this.expanded = false;
      this.destroyed = false;
      this.pointerDown = false;
      this.listeningForResize = false;
      /** @type {GraphRenderPayload | null} */
      this.lastPayload = null;
      /** @type {{ms:number, kind:'raw'|'coarse'} | null} */
      this.selection = null;
      /** @type {Array<{ms:number, kind:'raw'|'coarse', x:number, y:number, tempC:number, value:number, minimumC:number, maximumC:number}>} */
      this.models = [];
      /** @type {PlotRect | null} */
      this.plot = null;

      /** @type {(event: PointerEvent) => void} */
      this.onPointerDown = (event) => {
        if (this.destroyed) return;
        event.preventDefault();
        this.pointerDown = true;
        const position = this.eventPosition(event);
        if (position) this.inspectAt(position.x, position.y, true);
      };
      /** @type {(event: PointerEvent) => void} */
      this.onPointerMove = (event) => {
        if (this.destroyed || !this.pointerDown) return;
        const position = this.eventPosition(event);
        if (position) this.inspectAt(position.x, position.y, false);
      };
      /** @type {() => void} */
      this.onPointerEnd = () => {
        this.pointerDown = false;
      };
      /** @type {() => void} */
      this.onDoubleClick = () => {
        if (this.destroyed) return;
        const next = !this.expanded;
        this.expanded = next;
        if (typeof this.options.onExpandedChange === 'function') {
          this.options.onExpandedChange(next);
        }
        // Re-render with the new expanded state so it survives subsequent
        // re-renders of the last payload.
        this.render({ ...(this.lastPayload || {}), expanded: next });
      };
      /** @type {() => void} */
      this.onWindowResize = () => {
        if (!this.destroyed) this.resize();
      };

      const canvasAny = /** @type {any} */ (canvas);
      canvasAny.addEventListener('pointerdown', this.onPointerDown);
      canvasAny.addEventListener('pointermove', this.onPointerMove);
      canvasAny.addEventListener('pointerup', this.onPointerEnd);
      canvasAny.addEventListener('pointercancel', this.onPointerEnd);
      canvasAny.addEventListener('dblclick', this.onDoubleClick);
      if (options.listenForResize !== false && typeof root.addEventListener === 'function') {
        root.addEventListener('resize', this.onWindowResize);
        this.listeningForResize = true;
      } else {
        this.listeningForResize = false;
      }
    }

    /**
     * Renders (or re-renders) the graph from canonical chart data. Every
     * payload field is optional; the shell re-renders on store updates.
     * @param {GraphRenderPayload} [payload]
     */
    render(payload = {}) {
      if (this.destroyed) return;
      this.lastPayload = { ...payload };
      if (payload.unit === 'C' || payload.unit === 'F') this.unit = payload.unit;
      if (typeof payload.expanded === 'boolean') this.expanded = payload.expanded;
      this.draw(payload);
    }

    /** Re-measures the canvas backing store and redraws the last payload. */
    resize() {
      if (this.destroyed) return;
      if (this.lastPayload) this.render(this.lastPayload);
    }

    /** Releases listeners; the instance stops rendering afterwards. */
    destroy() {
      if (this.destroyed) return;
      this.destroyed = true;
      const canvasAny = /** @type {any} */ (this.canvas);
      canvasAny.removeEventListener('pointerdown', this.onPointerDown);
      canvasAny.removeEventListener('pointermove', this.onPointerMove);
      canvasAny.removeEventListener('pointerup', this.onPointerEnd);
      canvasAny.removeEventListener('pointercancel', this.onPointerEnd);
      canvasAny.removeEventListener('dblclick', this.onDoubleClick);
      if (this.listeningForResize && typeof root.removeEventListener === 'function') {
        root.removeEventListener('resize', this.onWindowResize);
      }
      this.listeningForResize = false;
      this.models = [];
      this.selection = null;
    }

    /**
     * Inspect at a canvas-relative position. Selects the nearest valid point
     * within the hit radius and fires `onInspect`; a tap on empty space clears
     * the selection (null payload). Exposed for tests and custom input wiring.
     * @param {number} xPx
     * @param {number} yPx
     * @param {boolean} [tap] true when the gesture begins
     * @returns {object | null}
     */
    inspectAt(xPx, yPx, tap = false) {
      if (this.destroyed) return null;
      const model = pickPoint(this.models, xPx, this.hitRadiusPx);
      if (model) {
        this.selection = { ms: model.ms, kind: model.kind };
        if (typeof this.options.onInspect === 'function') {
          this.options.onInspect(this.inspectPayload(model));
        }
      } else if (tap && this.selection) {
        this.selection = null;
        if (typeof this.options.onInspect === 'function') this.options.onInspect(null);
      }
      if (this.lastPayload) this.render(this.lastPayload);
      return model;
    }

    /** Clears any active inspection selection. */
    clearSelection() {
      const had = this.selection !== null;
      this.selection = null;
      if (had && typeof this.options.onInspect === 'function') this.options.onInspect(null);
      if (this.lastPayload) this.render(this.lastPayload);
    }

    /**
     * @param {{ms:number, kind:'raw'|'coarse', x:number, y:number, tempC:number, value:number, minimumC:number, maximumC:number}} model
     * @returns {InspectPayload}
     */
    inspectPayload(model) {
      return {
        ms: model.ms,
        kind: model.kind,
        unit: this.unit,
        x: model.x,
        y: model.y,
        valueC: model.tempC,
        value: model.value,
        minimumC: model.kind === 'coarse' ? model.minimumC : null,
        maximumC: model.kind === 'coarse' ? model.maximumC : null,
      };
    }

    /** @returns {{width:number, height:number}} CSS-pixel size, 0 when unmeasured */
    measure() {
      const canvasAny = /** @type {any} */ (this.canvas);
      let width = 0;
      let height = 0;
      if (typeof canvasAny.clientWidth === 'number' && canvasAny.clientWidth > 0) {
        width = canvasAny.clientWidth;
        height = canvasAny.clientHeight || 0;
      } else if (typeof canvasAny.getBoundingClientRect === 'function') {
        const rect = canvasAny.getBoundingClientRect();
        if (rect && Number.isFinite(rect.width)) {
          width = rect.width;
          height = Number.isFinite(rect.height) ? rect.height : 0;
        }
      }
      return { width: Math.max(0, width), height: Math.max(0, height) };
    }

    /** @returns {Record<string, string>} theme colors with defaults */
    resolveColors() {
      const base = { ...DEFAULT_COLORS, ...(this.options.colors || {}) };
      try {
        if (typeof root.getComputedStyle === 'function') {
          const style = root.getComputedStyle(this.canvas);
          if (style && typeof style.getPropertyValue === 'function') {
            for (const key of COLOR_KEYS) {
              const value = style.getPropertyValue(`--pv-graph-${key}`).trim();
              if (value) base[key] = value;
            }
          }
        }
      } catch {
        // Headless/test environments: defaults are fine.
      }
      return base;
    }

    /**
     * @param {PointerEvent} event
     * @returns {{x:number, y:number} | null}
     */
    eventPosition(event) {
      const canvasAny = /** @type {any} */ (this.canvas);
      let x = Number.NaN;
      let y = Number.NaN;
      if (Number.isFinite(event.offsetX) && Number.isFinite(event.offsetY)) {
        x = event.offsetX;
        y = event.offsetY;
      } else if (typeof canvasAny.getBoundingClientRect === 'function') {
        const rect = canvasAny.getBoundingClientRect();
        if (rect && Number.isFinite(rect.left)) {
          x = event.clientX - rect.left;
          y = event.clientY - rect.top;
        }
      }
      return Number.isFinite(x) && Number.isFinite(y) ? { x, y } : null;
    }

    /**
     * @param {GraphRenderPayload} payload
     */
    draw(payload) {
      const canvasAny = /** @type {any} */ (this.canvas);
      const context = /** @type {CanvasRenderingContext2D | null} */ (canvasAny.getContext('2d'));
      if (!context) return;
      const size = this.measure();
      if (size.width < 2 || size.height < 2) return;
      const dpr = Number(root.devicePixelRatio) > 0 ? Number(root.devicePixelRatio) : 1;
      canvasAny.width = Math.round(size.width * dpr);
      canvasAny.height = Math.round(size.height * dpr);
      context.setTransform(dpr, 0, 0, dpr, 0, 0);

      const colors = this.resolveColors();
      const points = payload.points || [];
      const rateRegions = payload.rateRegions || [];
      const markers = payload.markers || [];
      const expanded = this.expanded;
      const axisFont = `${expanded ? 12 : 11}px system-ui, sans-serif`;
      const chipFont = `${expanded ? 13 : 12}px system-ui, sans-serif`;

      const series = buildSeries(points, { unit: this.unit });
      const timeTargetTicks = Math.max(3, Math.min(10, Math.floor(size.width / 90)));
      const timeDomain = computeTimeDomain(series.maxMs, { maxTicks: timeTargetTicks });
      const tempTargetTicks = Math.max(3, Math.min(9, Math.floor(size.height / 55)));
      const tempDomain = computeTemperatureDomain(series.temperatureValues, { targetTicks: tempTargetTicks });

      const hasRateBand = rateRegions.length > 0;
      const rateBandHeight = hasRateBand ? 24 : 0;
      const rateBandGap = hasRateBand ? 6 : 0;
      const xLabelRow = (expanded ? 18 : 16) + rateBandGap;
      const padTop = expanded ? 10 : 14;
      const padRight = 12;
      const padBottom = (expanded ? 22 : 26) + xLabelRow + rateBandHeight;
      context.font = axisFont;
      let widestYLabel = 0;
      for (const tick of tempDomain.ticks) {
        const measured = context.measureText(tick.label).width;
        if (Number.isFinite(measured) && measured > widestYLabel) widestYLabel = measured;
      }
      const padLeft = Math.max(38, (widestYLabel || 30) + 14);

      const plotLeft = padLeft;
      const plotTop = padTop;
      const plotRight = Math.max(plotLeft + 1, size.width - padRight);
      const plotBottom = Math.max(plotTop + 1, size.height - padBottom);
      const plotWidth = plotRight - plotLeft;
      const plotHeight = plotBottom - plotTop;
      this.plot = { plotLeft, plotTop, plotRight, plotBottom };
      const rateBandTop = plotBottom + xLabelRow;

      /** @param {number} ms */
      const xForMs = (ms) => plotLeft + ((ms - timeDomain.startMs) / (timeDomain.endMs - timeDomain.startMs)) * plotWidth;
      /** @param {number} value */
      const yForValue = (value) => plotBottom - ((value - tempDomain.min) / (tempDomain.max - tempDomain.min)) * plotHeight;

      // Pixel models for inspection hit testing (valid points only).
      this.models = [];
      for (const segment of series.rawSegments) {
        for (const model of segment) this.models.push(this.toPixelModel(model, xForMs, yForValue));
      }
      for (const segment of series.coarseSegments) {
        for (const model of segment) this.models.push(this.toPixelModel(model, xForMs, yForValue));
      }
      this.models.sort((a, b) => a.ms - b.ms);

      context.clearRect(0, 0, size.width, size.height);

      if (!series.temperatureValues.length) {
        context.fillStyle = colors.axis;
        context.font = chipFont;
        context.textAlign = 'center';
        context.textBaseline = 'middle';
        context.fillText('Waiting for telemetry…', (plotLeft + plotRight) / 2, (plotTop + plotBottom) / 2);
        return;
      }

      // Grid
      const timeTicks = computeTimeTicks(timeDomain);
      context.strokeStyle = colors.grid;
      context.lineWidth = 1;
      context.beginPath();
      for (const tick of timeTicks) {
        const x = Math.round(xForMs(tick.ms)) + 0.5;
        if (x <= plotLeft || x > plotRight) continue;
        context.moveTo(x, plotTop);
        context.lineTo(x, plotBottom);
      }
      for (const tick of tempDomain.ticks) {
        const y = Math.round(yForValue(tick.value)) + 0.5;
        if (y < plotTop || y > plotBottom) continue;
        context.moveTo(plotLeft, y);
        context.lineTo(plotRight, y);
      }
      context.stroke();

      // X tick labels (human-friendly, clamped inside the plot)
      context.fillStyle = colors.axis;
      context.font = axisFont;
      context.textAlign = 'center';
      context.textBaseline = 'top';
      for (const tick of timeTicks) {
        const x = xForMs(tick.ms);
        if (x < plotLeft - 1 || x > plotRight + 1) continue;
        const halfWidth = (context.measureText(tick.label).width || 0) / 2;
        const clampedX = Math.min(Math.max(x, plotLeft + halfWidth), plotRight - halfWidth);
        context.fillText(tick.label, clampedX, plotBottom + 6);
      }
      // Y tick labels
      context.textAlign = 'right';
      context.textBaseline = 'middle';
      for (const tick of tempDomain.ticks) {
        const y = yForValue(tick.value);
        if (y < plotTop - 4 || y > plotBottom + 4) continue;
        context.fillText(tick.label, plotLeft - 6, y);
      }

      // Coarse layer: translucent min/max extent band + per-point extent bars +
      // dashed average line. Raw is drawn over it.
      context.fillStyle = colors.band;
      context.beginPath();
      for (const segment of series.coarseSegments) {
        const bandPoints = segment.filter((model) => model.band);
        if (bandPoints.length < 2) continue;
        context.moveTo(xForMs(bandPoints[0].ms), yForValue(bandPoints[0].maxValue));
        for (let index = 1; index < bandPoints.length; index += 1) {
          context.lineTo(xForMs(bandPoints[index].ms), yForValue(bandPoints[index].maxValue));
        }
        for (let index = bandPoints.length - 1; index >= 0; index -= 1) {
          context.lineTo(xForMs(bandPoints[index].ms), yForValue(bandPoints[index].minValue));
        }
        context.closePath();
      }
      context.fill();

      context.strokeStyle = colors.coarse;
      context.globalAlpha = 0.35;
      context.lineWidth = 1.5;
      context.beginPath();
      for (const segment of series.coarseSegments) {
        for (const model of segment) {
          if (!model.band) continue;
          const x = xForMs(model.ms);
          context.moveTo(x, yForValue(model.minValue));
          context.lineTo(x, yForValue(model.maxValue));
        }
      }
      context.stroke();
      context.globalAlpha = 1;

      this.strokeSegments(context, series.coarseSegments, xForMs, yForValue, colors.coarse, 1.5, [5, 4]);
      this.strokeSegments(context, series.rawSegments, xForMs, yForValue, colors.raw, 2, null);

      // Rate-of-rise segmented band directly below the plot.
      if (hasRateBand) {
        this.drawRateBand(context, colors, rateRegions, xForMs, rateBandTop, rateBandHeight, plotLeft, plotRight);
      }

      // Event markers (Milestone 5 supplies data; today this is the hook).
      for (const marker of markers) {
        this.drawMarker(context, colors, marker, xForMs, plotTop, plotBottom, chipFont, size.width);
      }

      // Inspection selection.
      const selection = this.selection;
      const selected = selection
        ? this.models.find((model) => model.ms === selection.ms && model.kind === selection.kind) || null
        : null;
      if (selected) {
        this.drawSelection(context, colors, selected, plotTop, plotBottom, chipFont, size.width);
      }
    }

    /**
     * @param {PointModel} model
     * @param {(ms:number) => number} xForMs
     * @param {(value:number) => number} yForValue
     * @returns {{ms:number, kind:'raw'|'coarse', x:number, y:number, tempC:number, value:number, minimumC:number, maximumC:number}}
     */
    toPixelModel(model, xForMs, yForValue) {
      return {
        ms: model.ms,
        kind: model.kind,
        x: xForMs(model.ms),
        y: yForValue(model.value),
        tempC: model.tempC,
        value: model.value,
        minimumC: model.kind === 'coarse' ? model.minimumC : Number.NaN,
        maximumC: model.kind === 'coarse' ? model.maximumC : Number.NaN,
      };
    }

    /**
     * Strokes one polyline per segment so invalid-point gaps stay visible.
     * Single-point segments still render as a visible dot.
     * @param {CanvasRenderingContext2D} context
     * @param {ReadonlyArray<ReadonlyArray<PointModel>>} segments
     * @param {(ms:number) => number} xForMs
     * @param {(value:number) => number} yForValue
     * @param {string} color
     * @param {number} lineWidth
     * @param {ReadonlyArray<number> | null} dash
     */
    strokeSegments(context, segments, xForMs, yForValue, color, lineWidth, dash) {
      context.strokeStyle = color;
      context.fillStyle = color;
      context.lineWidth = lineWidth;
      context.lineJoin = 'round';
      context.lineCap = 'round';
      context.setLineDash(dash ? Array.from(dash) : []);
      context.beginPath();
      for (const segment of segments) {
        let started = false;
        for (const model of segment) {
          const x = xForMs(model.ms);
          const y = yForValue(model.value);
          if (!started) {
            context.moveTo(x, y);
            started = true;
          } else {
            context.lineTo(x, y);
          }
        }
      }
      context.stroke();
      context.setLineDash([]);
      if (segments.some((segment) => segment.length === 1)) {
        context.beginPath();
        for (const segment of segments) {
          if (segment.length !== 1) continue;
          const model = segment[0];
          context.moveTo(xForMs(model.ms) + 2.2, yForValue(model.value));
          context.arc(xForMs(model.ms), yForValue(model.value), 2.2, 0, Math.PI * 2);
        }
        context.fill();
      }
    }

    /**
     * Rate band: one colored cell per region, labelled with the display-unit
     * rate when the cell is wide enough to read. Lower quality dims the cell.
     * @param {CanvasRenderingContext2D} context
     * @param {Record<string, string>} colors
     * @param {ReadonlyArray<{startMs:number, endMs:number, rateCPerHour:number, quality?:string, sampleCount?:number}>} rateRegions
     * @param {(ms:number) => number} xForMs
     * @param {number} top
     * @param {number} bandHeight
     * @param {number} plotLeft
     * @param {number} plotRight
     */
    drawRateBand(context, colors, rateRegions, xForMs, top, bandHeight, plotLeft, plotRight) {
      for (const region of rateRegions) {
        if (!Number.isFinite(region.startMs) || !Number.isFinite(region.endMs)) continue;
        const x0 = Math.max(plotLeft, xForMs(region.startMs));
        const x1 = Math.min(plotRight, xForMs(region.endMs));
        if (x1 - x0 < 1) continue;
        const hasRate = Number.isFinite(region.rateCPerHour);
        const tier = hasRate ? rateTier(region.rateCPerHour) : null;
        context.globalAlpha = hasRate ? (region.quality && region.quality !== 'good' ? 0.45 : 1) : 0.18;
        context.fillStyle = tier === 'cool'
          ? colors.rateCool
          : tier === 'mid'
            ? colors.rateMid
            : tier === 'hot'
              ? colors.rateHot
              : colors.grid;
        context.fillRect(x0, top, x1 - x0, bandHeight);
        context.globalAlpha = 1;
        context.strokeStyle = colors.grid;
        context.lineWidth = 1;
        context.strokeRect(x0 + 0.5, top + 0.5, x1 - x0 - 1, bandHeight - 1);
        const rate = convertRate(region.rateCPerHour, this.unit);
        if (Number.isFinite(rate) && x1 - x0 >= 44) {
          const label = `${Math.round(rate)}°${this.unit}/hr`;
          context.font = '10px system-ui, sans-serif';
          const textWidth = context.measureText(label).width;
          if (Number.isFinite(textWidth) && textWidth < x1 - x0 - 8) {
            context.fillStyle = colors.axis;
            context.textAlign = 'center';
            context.textBaseline = 'middle';
            context.fillText(label, (x0 + x1) / 2, top + bandHeight / 2);
          }
        }
      }
    }

    /**
     * Event-marker hook: dashed vertical line, triangle glyph at the top, and
     * an optional truncated label chip.
     * @param {CanvasRenderingContext2D} context
     * @param {Record<string, string>} colors
     * @param {{ms:number, label?:string}} marker
     * @param {(ms:number) => number} xForMs
     * @param {number} plotTop
     * @param {number} plotBottom
     * @param {string} chipFont
     * @param {number} canvasWidth
     */
    drawMarker(context, colors, marker, xForMs, plotTop, plotBottom, chipFont, canvasWidth) {
      if (!Number.isFinite(marker.ms)) return;
      const x = xForMs(marker.ms);
      if (x < 0 || x > canvasWidth) return;
      context.save();
      context.strokeStyle = colors.marker;
      context.lineWidth = 1;
      context.setLineDash([3, 4]);
      context.beginPath();
      context.moveTo(x, plotTop);
      context.lineTo(x, plotBottom);
      context.stroke();
      context.setLineDash([]);
      context.fillStyle = colors.marker;
      context.beginPath();
      context.moveTo(x, plotTop + 2);
      context.lineTo(x - 4, plotTop + 9);
      context.lineTo(x + 4, plotTop + 9);
      context.closePath();
      context.fill();
      if (marker.label) {
        const text = marker.label.length > 28 ? `${marker.label.slice(0, 27)}…` : marker.label;
        context.font = chipFont;
        const measured = context.measureText(text).width;
        const textWidth = Number.isFinite(measured) ? measured : text.length * 6;
        const chipWidth = textWidth + 12;
        const chipX = Math.min(Math.max(x - chipWidth / 2, 2), Math.max(2, canvasWidth - chipWidth - 2));
        const chipY = plotTop + 12;
        context.globalAlpha = 0.85;
        context.fillStyle = colors.marker;
        context.fillRect(chipX, chipY, chipWidth, 16);
        context.globalAlpha = 1;
        context.fillStyle = colors.inspectText;
        context.textAlign = 'left';
        context.textBaseline = 'middle';
        context.fillText(text, chipX + 6, chipY + 8);
      }
      context.restore();
    }

    /**
     * Inspection highlight: full-height guide, point ring, and value chip.
     * @param {CanvasRenderingContext2D} context
     * @param {Record<string, string>} colors
     * @param {{ms:number, x:number, y:number, value:number}} model
     * @param {number} plotTop
     * @param {number} plotBottom
     * @param {string} chipFont
     * @param {number} canvasWidth
     */
    drawSelection(context, colors, model, plotTop, plotBottom, chipFont, canvasWidth) {
      context.save();
      context.strokeStyle = colors.inspect;
      context.lineWidth = 1;
      context.setLineDash([2, 3]);
      context.beginPath();
      context.moveTo(model.x, plotTop);
      context.lineTo(model.x, plotBottom);
      context.stroke();
      context.setLineDash([]);
      context.fillStyle = colors.raw;
      context.beginPath();
      context.arc(model.x, model.y, 4.5, 0, Math.PI * 2);
      context.fill();
      context.restore();

      const label = `${formatTemperature(model.value)}°${this.unit} · ${formatDuration(model.ms)}`;
      context.font = chipFont;
      const measured = context.measureText(label).width;
      const textWidth = Number.isFinite(measured) ? measured : label.length * 7;
      const chipWidth = textWidth + 14;
      const chipHeight = 20;
      const chipX = Math.min(Math.max(model.x - chipWidth / 2, 2), Math.max(2, canvasWidth - chipWidth - 2));
      const chipY = Math.max(2, model.y - chipHeight - 10);
      context.fillStyle = colors.inspectText;
      context.fillRect(chipX, chipY, chipWidth, chipHeight);
      context.fillStyle = '#f4f2ec';
      context.textAlign = 'center';
      context.textBaseline = 'middle';
      context.fillText(label, chipX + chipWidth / 2, chipY + chipHeight / 2);
    }
  }

  api.GraphAxis = Object.freeze({
      niceStep,
      formatDuration,
      computeTimeDomain,
      computeTimeTicks,
      convertTemperature,
      convertRate,
      rateTier,
      computeTemperatureDomain,
      formatTemperature,
      buildSeries,
      pickPoint,
  });
  api.GraphRenderer = GraphRenderer;
})(/** @type {any} */ (globalThis));