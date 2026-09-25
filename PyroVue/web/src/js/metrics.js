// @ts-check
(function registerMetrics(root) {
  'use strict';

  const MS_PER_HOUR = 3600000;

  const DEFAULT_OPTIONS = Object.freeze({
    /** Regression window for the current rate of rise. */
    windowMs: 120000,
    /** Fewer usable samples than this within the window => unavailable. */
    minSamples: 3,
    /** Shorter usable span than this within the window => unavailable. */
    minDurationMs: 30000,
    /** Width of each visualization region. */
    segmentMs: 60000,
    /** Fewer usable samples than this within a region => unavailable region. */
    regionMinSamples: 2,
    /** Shorter usable span than this within a region => unavailable region. */
    regionMinDurationMs: 20000,
  });

  const QUALITY_GOOD = 'good';
  const QUALITY_FAIR = 'fair';
  const QUALITY_POOR = 'poor';
  const QUALITY_UNAVAILABLE = 'unavailable';

  /**
   * @param {{windowMs?:number, minSamples?:number, minDurationMs?:number,
   *          segmentMs?:number, regionMinSamples?:number, regionMinDurationMs?:number}} [options]
   */
  function resolveOptions(options) {
    const merged = { ...DEFAULT_OPTIONS, ...(options || {}) };
    /** @param {string} name @param {number} value */
    const requirePositiveDuration = (name, value) => {
      if (!Number.isFinite(value) || value <= 0) {
        throw new TypeError(`${name} must be a positive finite number of milliseconds`);
      }
    };
    /** @param {string} name @param {number} value */
    const requireSampleCount = (name, value) => {
      if (!Number.isInteger(value) || value < 2) {
        throw new TypeError(`${name} must be an integer >= 2`);
      }
    };
    /** @param {string} name @param {number} value */
    const requireDuration = (name, value) => {
      if (!Number.isFinite(value) || value < 0) {
        throw new TypeError(`${name} must be a non-negative finite number of milliseconds`);
      }
    };
    requirePositiveDuration('windowMs', merged.windowMs);
    requirePositiveDuration('segmentMs', merged.segmentMs);
    requireSampleCount('minSamples', merged.minSamples);
    requireSampleCount('regionMinSamples', merged.regionMinSamples);
    requireDuration('minDurationMs', merged.minDurationMs);
    requireDuration('regionMinDurationMs', merged.regionMinDurationMs);
    return merged;
  }

  /**
   * Representative temperature of a canonical point: raw samples report their
   * own reading, coarse buckets report the bucket average.
   * @param {any} point
   */
  function representativeTempC(point) {
    return point.kind === 'coarse' ? point.averageC : point.tempC;
  }

  /**
   * A point is usable only when it carries a healthy fault mask and a finite
   * representative temperature. The fault bitmask is authoritative: any
   * non-zero mask means the sample must not drive a rate calculation.
   * @param {any} point
   */
  function isUsablePoint(point) {
    if (!point || typeof point !== 'object') return false;
    if (typeof point.ms !== 'number' || !Number.isFinite(point.ms)) return false;
    const fault = point.fault;
    if (fault !== 0 && fault !== null && fault !== undefined) return false;
    const tempC = representativeTempC(point);
    return typeof tempC === 'number' && Number.isFinite(tempC);
  }

  /**
   * Deterministic preference between two usable points sharing a timestamp:
   * raw beats coarse, then the newer raw sequence number wins, then the finer
   * coarse resolution wins. Ties keep the incumbent.
   * @param {any} candidate @param {any} incumbent
   */
  function preferPoint(candidate, incumbent) {
    const candidateRank = candidate.kind === 'coarse' ? 1 : 0;
    const incumbentRank = incumbent.kind === 'coarse' ? 1 : 0;
    if (candidateRank !== incumbentRank) return candidateRank < incumbentRank;
    const candidateSeq = typeof candidate.seq === 'number' ? candidate.seq : -Infinity;
    const incumbentSeq = typeof incumbent.seq === 'number' ? incumbent.seq : -Infinity;
    if (candidateSeq !== incumbentSeq) return candidateSeq > incumbentSeq;
    const candidateResolution =
      typeof candidate.resolutionMs === 'number' ? candidate.resolutionMs : Infinity;
    const incumbentResolution =
      typeof incumbent.resolutionMs === 'number' ? incumbent.resolutionMs : Infinity;
    if (candidateResolution !== incumbentResolution) {
      return candidateResolution < incumbentResolution;
    }
    return false;
  }

  /**
   * Drops unusable points, collapses duplicate timestamps to a single winner,
   * and returns the survivors sorted by run-relative ms.
   * @param {any[]} points
   */
  function canonicalize(points) {
    /** @type {Map<number, any>} */
    const byMs = new Map();
    for (const point of points) {
      if (!isUsablePoint(point)) continue;
      const incumbent = byMs.get(point.ms);
      if (incumbent === undefined || preferPoint(point, incumbent)) byMs.set(point.ms, point);
    }
    return Array.from(byMs.values()).sort((a, b) => a.ms - b.ms);
  }

  /**
   * Ordinary least squares over (ms, temperature). The caller guarantees at
   * least two distinct timestamps, so the denominator cannot be zero.
   * @param {any[]} sorted canonicalized points, at least two
   */
  function regressionRateCPerHour(sorted) {
    const n = sorted.length;
    let meanX = 0;
    let meanY = 0;
    for (const point of sorted) {
      meanX += point.ms;
      meanY += representativeTempC(point);
    }
    meanX /= n;
    meanY /= n;

    let covariance = 0;
    let variance = 0;
    for (const point of sorted) {
      const centeredX = point.ms - meanX;
      covariance += centeredX * (representativeTempC(point) - meanY);
      variance += centeredX * centeredX;
    }
    return (covariance / variance) * MS_PER_HOUR;
  }

  /**
   * Quality reflects how well the usable data covers the window it was drawn
   * from. It never invents confidence for sparse or short data.
   * @param {number} sampleCount @param {number} spanMs @param {number} windowMs @param {number} minSamples
   */
  function assessQuality(sampleCount, spanMs, windowMs, minSamples) {
    const coverage = Math.min(1, spanMs / windowMs);
    if (coverage >= 0.9 && sampleCount >= Math.max(minSamples + 1, 4)) return QUALITY_GOOD;
    if (coverage >= 0.5 && sampleCount >= minSamples) return QUALITY_FAIR;
    return QUALITY_POOR;
  }

  /**
   * @param {number} windowMs @param {number} sampleCount
   * @param {number | null} startMs @param {number | null} endMs
   */
  function unavailableResult(windowMs, sampleCount, startMs, endMs) {
    return Object.freeze({
      rateCPerHour: null,
      quality: QUALITY_UNAVAILABLE,
      sampleCount,
      startMs,
      endMs,
      windowMs,
    });
  }

  /**
   * Current rate of rise in degrees Celsius per hour, estimated by least
   * squares regression over the most recent `windowMs` of usable samples.
   * Never an adjacent-sample delta. Returns an unavailable result (null rate)
   * when data or duration within the window are insufficient.
   * @param {any[]} points canonical raw/coarse chart points
   * @param {{windowMs?:number, minSamples?:number, minDurationMs?:number}} [options]
   */
  function calculateRateOfRise(points, options) {
    const resolved = resolveOptions(options);
    const sorted = canonicalize(Array.isArray(points) ? points : []);
    if (sorted.length === 0) return unavailableResult(resolved.windowMs, 0, null, null);

    const endMs = sorted[sorted.length - 1].ms;
    const cutoff = endMs - resolved.windowMs;
    let start = 0;
    while (start < sorted.length && sorted[start].ms < cutoff) start += 1;
    const windowed = sorted.slice(start);

    const sampleCount = windowed.length;
    if (sampleCount === 0) return unavailableResult(resolved.windowMs, 0, null, endMs);
    const startMs = windowed[0].ms;
    const spanMs = endMs - startMs;
    if (sampleCount < resolved.minSamples || spanMs < resolved.minDurationMs) {
      return unavailableResult(resolved.windowMs, sampleCount, startMs, endMs);
    }
    return Object.freeze({
      rateCPerHour: regressionRateCPerHour(windowed),
      quality: assessQuality(sampleCount, spanMs, resolved.windowMs, resolved.minSamples),
      sampleCount,
      startMs,
      endMs,
      windowMs: resolved.windowMs,
    });
  }

  /**
   * Segments the run into contiguous, non-overlapping time regions of at most
   * `segmentMs`, each carrying its own regression rate for graph
   * visualization. Regions with gaps or too little data report null rate and
   * `unavailable` quality instead of interpolated values. Each point belongs
   * to exactly one region (half-open intervals, final region closed).
   * @param {any[]} points canonical raw/coarse chart points
   * @param {{segmentMs?:number, regionMinSamples?:number, regionMinDurationMs?:number}} [options]
   * @returns {ReadonlyArray<{startMs:number, endMs:number, rateCPerHour:number|null, quality:string, sampleCount:number}>}
   */
  function segmentRateRegions(points, options) {
    const resolved = resolveOptions(options);
    const sorted = canonicalize(Array.isArray(points) ? points : []);
    if (sorted.length === 0) return Object.freeze([]);

    const firstMs = sorted[0].ms;
    const lastMs = sorted[sorted.length - 1].ms;

    /** @type {[number, number][]} */
    const bounds = [];
    let cursor = firstMs;
    while (cursor < lastMs) {
      const next = Math.min(cursor + resolved.segmentMs, lastMs);
      bounds.push([cursor, next]);
      cursor = next;
    }
    // A trailing sliver shorter than half a segment carries too little time
    // to be worth its own region; fold it into the previous region instead.
    if (bounds.length > 1) {
      const last = bounds[bounds.length - 1];
      if (last[1] - last[0] < resolved.segmentMs / 2) {
        bounds.pop();
        bounds[bounds.length - 1][1] = last[1];
      }
    }

    /** @type {{startMs:number, endMs:number, rateCPerHour:number|null, quality:string, sampleCount:number}[]} */
    const regions = [];
    let index = 0;
    for (let i = 0; i < bounds.length; i += 1) {
      const [startMs, endMs] = bounds[i];
      const isLast = i === bounds.length - 1;
      const bucket = [];
      while (index < sorted.length && sorted[index].ms < endMs) {
        bucket.push(sorted[index]);
        index += 1;
      }
      if (isLast && index < sorted.length && sorted[index].ms === endMs) {
        bucket.push(sorted[index]);
        index += 1;
      }

      const sampleCount = bucket.length;
      const spanMs = sampleCount > 0 ? bucket[sampleCount - 1].ms - bucket[0].ms : 0;
      if (sampleCount < resolved.regionMinSamples || spanMs < resolved.regionMinDurationMs) {
        regions.push(
          Object.freeze({ startMs, endMs, rateCPerHour: null, quality: QUALITY_UNAVAILABLE, sampleCount }),
        );
        continue;
      }
      regions.push(
        Object.freeze({
          startMs,
          endMs,
          rateCPerHour: regressionRateCPerHour(bucket),
          quality: assessQuality(sampleCount, spanMs, endMs - startMs, resolved.regionMinSamples),
          sampleCount,
        }),
      );
    }
    return Object.freeze(regions);
  }

  root.PyroVue = Object.assign(root.PyroVue || {}, {
    calculateRateOfRise,
    segmentRateRegions,
  });
})(/** @type {any} */ (globalThis));
