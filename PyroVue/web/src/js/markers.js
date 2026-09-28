// @ts-check
(function registerMarkers(root) {
  'use strict';

  const api = root.PyroVue || {};
  root.PyroVue = api;
  const STORAGE_KEY = 'pyrovue.markers.v1';
  const MAX_NOTE_LENGTH = 500;
  let fallbackIdCounter = 0;

  /** @param {unknown} value */
  function validRunId(value) {
    return typeof value === 'number' && Number.isInteger(value) && value > 0;
  }

  /** @param {any} value */
  function validMarker(value) {
    return value !== null && typeof value === 'object'
      && typeof value.id === 'string' && value.id.length > 0
      && validRunId(value.runId)
      && typeof value.ms === 'number' && Number.isFinite(value.ms) && value.ms >= 0
      && typeof value.note === 'string' && value.note.length <= MAX_NOTE_LENGTH;
  }

  /** @param {any} marker */
  function copyMarker(marker) {
    return { id: marker.id, runId: marker.runId, ms: marker.ms, note: marker.note };
  }

  class MarkerStore {
    /** @param {{getItem:(key:string)=>string|null, setItem:(key:string,value:string)=>void}} storage */
    constructor(storage) {
      if (!storage || typeof storage.getItem !== 'function' || typeof storage.setItem !== 'function') {
        throw new TypeError('MarkerStore requires Storage-like getItem/setItem methods');
      }
      this.storage = storage;
      this.markers = this.readMarkers();
    }

    readMarkers() {
      const raw = this.storage.getItem(STORAGE_KEY);
      if (raw === null) return [];
      let saved;
      try {
        saved = JSON.parse(raw);
      } catch {
        return [];
      }
      if (!saved || saved.version !== 1 || !Array.isArray(saved.markers)) return [];

      const result = [];
      const ids = new Set();
      for (const marker of saved.markers) {
        if (!validMarker(marker) || ids.has(marker.id)) continue;
        ids.add(marker.id);
        result.push(copyMarker(marker));
      }
      return result;
    }

    /** @param {number} runId */
    list(runId) {
      this.assertRunId(runId);
      return this.markers
        .filter((marker) => marker.runId === runId)
        .sort((a, b) => a.ms - b.ms || a.id.localeCompare(b.id))
        .map(copyMarker);
    }

    /** @param {number} runId @param {number} ms */
    add(runId, ms) {
      this.assertRunId(runId);
      if (typeof ms !== 'number' || !Number.isFinite(ms) || ms < 0) {
        throw new RangeError('Marker timestamp must be finite and nonnegative');
      }
      const marker = { id: this.newId(), runId, ms, note: '' };
      this.commit([...this.markers, marker]);
      return copyMarker(marker);
    }

    /** @param {number} runId @param {string} id @param {string} note */
    update(runId, id, note) {
      this.assertRunId(runId);
      if (typeof id !== 'string') throw new TypeError('Marker id must be a string');
      if (typeof note !== 'string') throw new TypeError('Marker note must be a string');
      const normalizedNote = note.trim();
      if (normalizedNote.length > MAX_NOTE_LENGTH) {
        throw new RangeError(`Marker note must be at most ${MAX_NOTE_LENGTH} characters`);
      }
      const index = this.markers.findIndex((marker) => marker.runId === runId && marker.id === id);
      if (index < 0) return null;
      const updated = { ...this.markers[index], note: normalizedNote };
      const next = this.markers.slice();
      next[index] = updated;
      this.commit(next);
      return copyMarker(updated);
    }

    /** @param {number} runId @param {string} id */
    remove(runId, id) {
      this.assertRunId(runId);
      if (typeof id !== 'string') throw new TypeError('Marker id must be a string');
      const index = this.markers.findIndex((marker) => marker.runId === runId && marker.id === id);
      if (index < 0) return false;
      const next = this.markers.slice();
      next.splice(index, 1);
      this.commit(next);
      return true;
    }

    /** @param {number} runId */
    assertRunId(runId) {
      if (!validRunId(runId)) throw new RangeError('Run id must be a positive integer');
    }

    newId() {
      /** @type {string} */
      let id;
      do {
        if (root.crypto && typeof root.crypto.randomUUID === 'function') {
          id = root.crypto.randomUUID();
        } else {
          fallbackIdCounter += 1;
          id = `${Date.now().toString(36)}-${Math.random().toString(36).slice(2)}-${fallbackIdCounter.toString(36)}`;
        }
      } while (this.markers.some((marker) => marker.id === id));
      return id;
    }

    /** @param {Array<{id: string, runId: number, ms: number, note: string}>} next */
    commit(next) {
      const serialized = JSON.stringify({ version: 1, markers: next });
      // Persist before changing memory: a quota/security error must not make an
      // unsaved marker appear to have succeeded in this page session.
      this.storage.setItem(STORAGE_KEY, serialized);
      this.markers = next;
    }
  }

  api.MarkerStore = MarkerStore;
})(/** @type {any} */ (globalThis));
