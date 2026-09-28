// @ts-check
(function registerTheme(root) {
  'use strict';

  /**
   * Map an authoritative live firing temperature to an aesthetic heat stage.
   * This is a visual mapping, not a physical color-temperature claim.
   * @param {{active?:boolean, fresh?:boolean, valid?:boolean, tempC?:number}} reading
   * @returns {{stage:string, intensity:number}}
   */
  function deriveHeatTheme(reading) {
    const { active, fresh, valid, tempC } = reading || {};
    if (active !== true || fresh !== true || valid !== true ||
        typeof tempC !== 'number' || !Number.isFinite(tempC)) {
      return Object.freeze({ stage: 'neutral', intensity: 0 });
    }

    /** @type {string} */
    let stage = 'neutral';
    if (tempC >= 1250) stage = 'white';
    else if (tempC >= 1100) stage = 'yellow';
    else if (tempC >= 900) stage = 'orange';
    else if (tempC >= 700) stage = 'red';
    else if (tempC >= 500) stage = 'dull-red';

    const intensity = Math.max(0, Math.min(1, (tempC - 400) / 900));
    return Object.freeze({ stage, intensity });
  }

  root.PyroVue = root.PyroVue || {};
  root.PyroVue.deriveHeatTheme = deriveHeatTheme;
})(/** @type {any} */ (globalThis));
