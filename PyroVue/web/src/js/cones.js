// @ts-check
(function registerCones(root) {
  'use strict';

  /**
   * Orton, “Temperature Equivalents for Orton Pyrometric Cones (°C) Cone
   * Numbers 022-14”, ©2016 Orton Ceramic Foundation. Transcribed from the
   * self-supporting cone 15/60/150°C/hr columns only. Canonical resource:
   * https://www.ortonceramic.com/pyrometric-cones-resources
   * Chart copy: https://hazelaar.nl/wp-content/uploads/2026/01/Orton-Cone-Chart-C-022-14-2016.pdf
   */
  const CONE_SEQUENCE = Object.freeze([
    '022', '021', '020', '019', '018', '017', '016', '015', '014', '013', '012', '011', '010', '09', '08', '07', '06',
    '05½', '05', '04', '03', '02', '01', '1', '2', '3', '4', '5', '5½', '6', '7', '8', '9', '10', '11', '12', '13', '14',
  ]);

  /** @type {Record<string, readonly [number|null, number|null, number|null]>} */
  const chart = {
    '022': [null, 586, 590], '021': [null, 600, 617], '020': [null, 626, 638],
    '019': [656, 678, 695], '018': [686, 715, 734], '017': [705, 738, 763],
    '016': [742, 772, 796], '015': [750, 791, 818], '014': [757, 807, 838],
    '013': [807, 837, 861], '012': [843, 861, 882], '011': [857, 875, 894],
    '010': [891, 903, 915], '09': [907, 920, 930], '08': [922, 942, 956],
    '07': [962, 976, 987], '06': [981, 998, 1013], '05½': [1004, 1015, 1025],
    '05': [1021, 1031, 1044], '04': [1046, 1063, 1077], '03': [1071, 1086, 1104],
    '02': [1078, 1102, 1122], '01': [1093, 1119, 1138], '1': [1109, 1137, 1154],
    '2': [1112, 1142, 1164], '3': [1115, 1152, 1170], '4': [1141, 1162, 1183],
    '5': [1159, 1186, 1207], '5½': [1167, 1203, 1225], '6': [1185, 1222, 1243],
    '7': [1201, 1239, 1257], '8': [1211, 1249, 1271], '9': [1224, 1260, 1280],
    '10': [1251, 1285, 1305], '11': [1272, 1294, 1315], '12': [1285, 1306, 1326],
    '13': [1310, 1331, 1348], '14': [1351, 1365, 1384],
  };
  /** @type {Record<string, readonly [number|null, number|null, number|null]>} */
  const immutableChart = {};
  for (const cone of CONE_SEQUENCE) immutableChart[cone] = Object.freeze(chart[cone]);
  const SELF_SUPPORTING_EQUIVALENT_C = Object.freeze(immutableChart);
  const RATE_COLUMNS = Object.freeze([15, 60, 150]);
  const REGRESSION_OPTIONS = Object.freeze({ windowMs: 7200000, minSamples: 3, minDurationMs: 1800000 });

  /** @param {any} point */
  function temperatureOf(point) {
    return point && point.kind === 'coarse' ? point.averageC : point && point.tempC;
  }

  /** @param {any} point */
  function usable(point) {
    const tempC = temperatureOf(point);
    return Boolean(point && typeof point === 'object' && Number.isFinite(point.ms) &&
      (point.fault === 0 || point.fault === null || point.fault === undefined) && Number.isFinite(tempC) &&
      (point.kind === 'raw' || point.kind === 'coarse'));
  }

  /** @param {any} candidate @param {any} incumbent */
  function prefer(candidate, incumbent) {
    const candidateRaw = candidate.kind !== 'coarse';
    const incumbentRaw = incumbent.kind !== 'coarse';
    if (candidateRaw !== incumbentRaw) return candidateRaw;
    const candidateSeq = Number.isFinite(candidate.seq) ? candidate.seq : -Infinity;
    const incumbentSeq = Number.isFinite(incumbent.seq) ? incumbent.seq : -Infinity;
    if (candidateSeq !== incumbentSeq) return candidateSeq > incumbentSeq;
    const candidateResolution = Number.isFinite(candidate.resolutionMs) ? candidate.resolutionMs : Infinity;
    const incumbentResolution = Number.isFinite(incumbent.resolutionMs) ? incumbent.resolutionMs : Infinity;
    return candidateResolution < incumbentResolution;
  }

  /**
   * Sort while retaining invalid events as barriers; collapse usable duplicates
   * using metrics.js precedence. An invalid event at a duplicate timestamp
   * remains a barrier around that timestamp's usable winner.
   * @param {any[]} points
   * @returns {any[][]}
   */
  function contiguousSegments(points) {
    /** @type {Array<{point:any,index:number}>} */
    const sorted = [];
    /** @type {Array<{point:any,index:number}>} */
    let chunk = [];
    const flushSortedChunk = () => {
      chunk.sort((a, b) => a.point.ms - b.point.ms || a.index - b.index);
      for (const entry of chunk) sorted.push(entry);
      chunk = [];
    };
    for (let index = 0; index < points.length; index += 1) {
      const point = points[index];
      if (!Number.isFinite(point?.ms)) {
        flushSortedChunk();
        sorted.push({ point, index });
      } else {
        chunk.push({ point, index });
      }
    }
    flushSortedChunk();
    /** @type {any[][]} */
    const segments = [];
    /** @type {any[]} */
    let segment = [];
    /** @param {any} point */
    const flush = (point) => {
      if (segment.length) segments.push(segment);
      segment = point ? [point] : [];
    };
    for (let i = 0; i < sorted.length;) {
      const point = sorted[i].point;
      const ms = point && point.ms;
      if (!Number.isFinite(ms)) { flush(null); i += 1; continue; }
      let end = i + 1;
      while (end < sorted.length && sorted[end].point && sorted[end].point.ms === ms) end += 1;
      const group = sorted.slice(i, end).map((entry) => entry.point);
      let winner = null;
      let hasInvalid = false;
      for (const candidate of group) {
        if (!usable(candidate)) { hasInvalid = true; continue; }
        if (!winner || prefer(candidate, winner)) winner = candidate;
      }
      if (hasInvalid) flush(null);
      if (winner) {
        const previous = segment[segment.length - 1];
        if (previous && !continuous(previous, winner)) flush(null);
        segment.push(winner);
      }
      if (hasInvalid) flush(null);
      i = end;
    }
    if (segment.length) segments.push(segment);
    return segments;
  }

  /** @param {any} previous @param {any} next */
  function continuous(previous, next) {
    if (next.ms <= previous.ms) return false;
    if (previous.kind === 'raw' && next.kind === 'raw') {
      return Number.isInteger(previous.seq) && Number.isInteger(next.seq) && next.seq === previous.seq + 1;
    }
    if (previous.kind === 'coarse' && next.kind === 'coarse') {
      const resolutionMs = Number.isFinite(previous.resolutionMs) && previous.resolutionMs > 0 ? previous.resolutionMs : 0;
      return next.ms <= previous.ms + resolutionMs;
    }
    const coarse = previous.kind === 'coarse' ? previous : next;
    const resolutionMs = Number.isFinite(coarse.resolutionMs) && coarse.resolutionMs > 0 ? coarse.resolutionMs : 0;
    return next.ms - previous.ms <= resolutionMs;
  }

  /** @param {number} rate */
  function nearestRateIndex(rate) {
    let selected = 0;
    for (let i = 1; i < RATE_COLUMNS.length; i += 1) {
      if (Math.abs(rate - RATE_COLUMNS[i]) <= Math.abs(rate - RATE_COLUMNS[selected])) selected = i;
    }
    return selected;
  }

  /** @param {string} cone @param {number} preferredIndex @param {number} measuredRate */
  function coneEndpoint(cone, preferredIndex, measuredRate) {
    const values = SELF_SUPPORTING_EQUIVALENT_C[cone];
    const selectedValue = values[preferredIndex];
    if (selectedValue !== null) return { tempC: selectedValue, rate: RATE_COLUMNS[preferredIndex] };
    let selected = -1;
    for (let i = 0; i < values.length; i += 1) {
      if (values[i] === null) continue;
      if (selected < 0 || Math.abs(measuredRate - RATE_COLUMNS[i]) <= Math.abs(measuredRate - RATE_COLUMNS[selected])) selected = i;
    }
    return selected < 0 ? null : { tempC: /** @type {number} */ (values[selected]), rate: RATE_COLUMNS[selected] };
  }

  /** @param {any[]} points */
  function estimateConeProgress(points) {
    if (!Array.isArray(points) || typeof root.PyroVue?.calculateRateOfRise !== 'function') return null;
    const segments = contiguousSegments(points);
    let peak = null;
    let peakSegment = null;
    for (const candidateSegment of segments) {
      let segmentMaximum = -Infinity;
      for (const point of candidateSegment) segmentMaximum = Math.max(segmentMaximum, temperatureOf(point));
      let firstPeak = null;
      for (let i = 0; i < candidateSegment.length; i += 1) {
        if (temperatureOf(candidateSegment[i]) === segmentMaximum &&
            (i === 0 || temperatureOf(candidateSegment[i - 1]) !== segmentMaximum)) firstPeak = candidateSegment[i];
      }
      if (!firstPeak) continue;
      if (!peak || segmentMaximum > temperatureOf(peak) ||
          (segmentMaximum === temperatureOf(peak) && firstPeak.ms > peak.ms)) {
        // The first point of a plateau is its arrival; a later reheat at the
        // same global maximum supersedes an earlier firing.
        peak = firstPeak;
        peakSegment = candidateSegment;
      }
    }
    if (!peak || !peakSegment) return null;

    const peakIndex = peakSegment.indexOf(peak);
    const peakC = temperatureOf(peak);
    const thresholdC = peakC - 100;
    let crossingMs = null;
    for (let i = 0; i < peakIndex; i += 1) {
      const lower = peakSegment[i];
      const upper = peakSegment[i + 1];
      const lowerC = temperatureOf(lower);
      const upperC = temperatureOf(upper);
      if (lowerC <= thresholdC && upperC >= thresholdC && upperC > lowerC) {
        crossingMs = lower.ms + ((thresholdC - lowerC) / (upperC - lowerC)) * (upper.ms - lower.ms);
      }
    }

    let rateCPerHour;
    let quality;
    const measuredRate = crossingMs !== null && peak.ms > crossingMs
      ? 100 / ((peak.ms - crossingMs) / 3600000)
      : null;
    if (measuredRate !== null && Number.isFinite(measuredRate) && measuredRate > 0) {
      rateCPerHour = measuredRate;
      quality = 'measured-100c';
    } else {
      const prefix = peakSegment.slice(0, peakIndex + 1);
      const result = root.PyroVue.calculateRateOfRise(prefix, REGRESSION_OPTIONS);
      if (!result || !(result.quality === 'fair' || result.quality === 'good') ||
          !Number.isFinite(result.rateCPerHour) || result.rateCPerHour <= 0) return null;
      rateCPerHour = result.rateCPerHour;
      quality = 'estimated-regression';
    }

    const preferredIndex = nearestRateIndex(rateCPerHour);
    const endpoints = CONE_SEQUENCE.map((cone) => coneEndpoint(cone, preferredIndex, rateCPerHour));
    const validEndpoints = endpoints.map((endpoint, index) => ({ endpoint, index }))
      .filter((entry) => entry.endpoint !== null)
      .map((entry) => ({ endpoint: /** @type {{tempC:number,rate:number}} */ (entry.endpoint), index: entry.index }));
    if (!validEndpoints.length) return null;
    const first = validEndpoints[0];
    const last = validEndpoints[validEndpoints.length - 1];
    let cone;
    let previousCone;
    let nextCone;
    let progress;
    let label;
    let rateColumnCPerHour;
    if (peakC < first.endpoint.tempC) {
      cone = CONE_SEQUENCE[first.index];
      previousCone = null;
      nextCone = CONE_SEQUENCE[first.index + 1] ?? null;
      progress = 0;
      label = `Below cone ${cone}`;
      rateColumnCPerHour = first.endpoint.rate;
    } else if (peakC > last.endpoint.tempC) {
      cone = CONE_SEQUENCE[last.index];
      previousCone = CONE_SEQUENCE[last.index - 1] ?? null;
      nextCone = null;
      progress = 1;
      label = `Cone ${cone}+`;
      rateColumnCPerHour = last.endpoint.rate;
    } else {
      let upperPosition = validEndpoints.findIndex((entry) => peakC <= entry.endpoint.tempC);
      if (upperPosition < 0) upperPosition = validEndpoints.length - 1;
      const upper = validEndpoints[upperPosition];
      if (peakC === upper.endpoint.tempC || upperPosition === 0) {
        const index = upper.index;
        cone = CONE_SEQUENCE[index];
        previousCone = CONE_SEQUENCE[index - 1] ?? null;
        nextCone = CONE_SEQUENCE[index + 1] ?? null;
        progress = 1;
        label = `Cone ${cone}`;
        rateColumnCPerHour = upper.endpoint.rate;
      } else {
        const lower = validEndpoints[upperPosition - 1];
        const fraction = (peakC - lower.endpoint.tempC) / (upper.endpoint.tempC - lower.endpoint.tempC);
        cone = CONE_SEQUENCE[upper.index];
        previousCone = CONE_SEQUENCE[lower.index];
        nextCone = CONE_SEQUENCE[upper.index + 1] ?? null;
        progress = Math.max(0, Math.min(1, fraction));
        label = `Cone ${cone}`;
        rateColumnCPerHour = upper.endpoint.rate;
      }
    }
    return Object.freeze({ label, cone, previousCone, nextCone, progress, peakC, rateCPerHour, rateColumnCPerHour, quality });
  }

  root.PyroVue = Object.assign(root.PyroVue || {}, {
    CONE_SEQUENCE,
    SELF_SUPPORTING_EQUIVALENT_C,
    estimateConeProgress,
  });
})(/** @type {any} */ (globalThis));
