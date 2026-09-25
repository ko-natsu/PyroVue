// @ts-check
(function registerProtocol(root) {
  'use strict';

  const PROTOCOL_VERSION = 2;
  const MESSAGE_TYPES = new Set([
    'hello',
    'state',
    'history.begin',
    'history',
    'history.end',
    'sample',
    'error',
  ]);

  const FAULT_FLAGS = Object.freeze({
    OPEN_THERMOCOUPLE: 1,
    SHORT_TO_GROUND: 2,
    SHORT_TO_VCC: 4,
    NON_FINITE: 8,
  });

  class ProtocolError extends Error {
    /** @param {string} message */
    constructor(message) {
      super(message);
      this.name = 'ProtocolError';
    }
  }

  /** @param {unknown} value @param {string} name */
  function requireObject(value, name) {
    if (!value || typeof value !== 'object' || Array.isArray(value)) {
      throw new ProtocolError(`${name} must be an object`);
    }
    return /** @type {Record<string, any>} */ (value);
  }

  /** @param {Record<string, any>} value @param {string} field */
  function requireString(value, field) {
    if (typeof value[field] !== 'string' || value[field].length === 0) {
      throw new ProtocolError(`${field} must be a non-empty string`);
    }
  }

  /** @param {Record<string, any>} value @param {string} field */
  function requireBoolean(value, field) {
    if (typeof value[field] !== 'boolean') {
      throw new ProtocolError(`${field} must be a boolean`);
    }
  }

  /** @param {Record<string, any>} value @param {string} field @param {boolean} [positive] */
  function requireInteger(value, field, positive = false) {
    const number = value[field];
    if (!Number.isSafeInteger(number) || number < (positive ? 1 : 0)) {
      throw new ProtocolError(`${field} must be a ${positive ? 'positive' : 'non-negative'} integer`);
    }
  }

  /** @param {Record<string, any>} value @param {string} field */
  function requireFiniteNumber(value, field) {
    if (typeof value[field] !== 'number' || !Number.isFinite(value[field])) {
      throw new ProtocolError(`${field} must be a finite number`);
    }
  }

  /**
   * Temperature values may be null because non-finite firmware values can be
   * serialized as JSON null. The fault bit remains authoritative.
   * @param {Record<string, any>} value
   * @param {string} field
   */
  function requireTemperature(value, field) {
    if (value[field] !== null && (typeof value[field] !== 'number' || !Number.isFinite(value[field]))) {
      throw new ProtocolError(`${field} must be a finite number or null`);
    }
  }

  /** @param {Record<string, any>} value */
  function requireFault(value) {
    requireInteger(value, 'fault');
    if (value.fault > 255) {
      throw new ProtocolError('fault must fit in one byte');
    }
  }

  /** @param {Record<string, any>} message */
  function validateProtocol(message) {
    requireInteger(message, 'protocol', true);
    if (message.protocol !== PROTOCOL_VERSION) {
      throw new ProtocolError(`unsupported protocol ${message.protocol}`);
    }
  }

  /** @param {Record<string, any>} sample @param {boolean} includeRun */
  function validateRawSample(sample, includeRun) {
    if (includeRun) requireInteger(sample, 'runId');
    requireInteger(sample, 'seq');
    requireInteger(sample, 'ms');
    const seq = sample.seq;
    const ms = sample.ms;
    if (includeRun && (seq === 0 || sample.runId === 0)) {
      if (sample.runId !== 0 || seq !== 0 || ms !== 0) {
        throw new ProtocolError('zero sequence is only valid for the idle sample identity');
      }
    } else if (seq <= 0) {
      throw new ProtocolError('seq must be positive');
    }
    requireTemperature(sample, 'temp');
    requireFault(sample);
  }

  /** @param {Record<string, any>} sample */
  function validateCoarseSample(sample) {
    requireInteger(sample, 'ms');
    requireTemperature(sample, 'avg');
    requireTemperature(sample, 'min');
    requireTemperature(sample, 'max');
    requireFault(sample);
  }

  /** @param {Record<string, any>} message */
  function validateMessage(message) {
    requireString(message, 'type');
    if (!MESSAGE_TYPES.has(message.type)) {
      throw new ProtocolError(`unknown message type ${message.type}`);
    }
    validateProtocol(message);

    switch (message.type) {
      case 'hello':
        requireString(message, 'firmware');
        requireFiniteNumber(message, 'sampleHz');
        if (message.sampleHz <= 0) throw new ProtocolError('sampleHz must be positive');
        requireInteger(message, 'runId');
        requireBoolean(message, 'runActive');
        if (message.runStartMs !== undefined) requireInteger(message, 'runStartMs');
        if (message.recentWindowMs !== undefined) requireInteger(message, 'recentWindowMs', true);
        if (message.coarseResolutionMs !== undefined) requireInteger(message, 'coarseResolutionMs', true);
        if (message.snapshotThroughSeq !== undefined) requireInteger(message, 'snapshotThroughSeq');
        if (message.snapshotId !== undefined) requireInteger(message, 'snapshotId');
        break;
      case 'state':
        requireInteger(message, 'runId');
        requireBoolean(message, 'runActive');
        break;
      case 'history.begin':
      case 'history.end':
        requireInteger(message, 'snapshotId');
        requireInteger(message, 'runId');
        requireInteger(message, 'throughSeq');
        break;
      case 'history':
        requireInteger(message, 'snapshotId');
        requireInteger(message, 'runId');
        requireInteger(message, 'resolutionMs', true);
        requireInteger(message, 'chunk');
        if (message.kind !== 'raw' && message.kind !== 'coarse') {
          throw new ProtocolError('history kind must be raw or coarse');
        }
        if (!Array.isArray(message.samples)) {
          throw new ProtocolError('history samples must be an array');
        }
        for (const value of message.samples) {
          const sample = requireObject(value, 'history sample');
          if (message.kind === 'raw') validateRawSample(sample, false);
          else validateCoarseSample(sample);
        }
        break;
      case 'sample':
        validateRawSample(message, true);
        break;
      case 'error':
        requireString(message, 'code');
        break;
      default:
        throw new ProtocolError(`unhandled message type ${message.type}`);
    }
    return message;
  }

  /** @param {string | Record<string, any>} input */
  function parseMessage(input) {
    let value = input;
    if (typeof input === 'string') {
      try {
        value = JSON.parse(input);
      } catch (error) {
        throw new ProtocolError(`invalid JSON: ${error instanceof Error ? error.message : String(error)}`);
      }
    }
    return validateMessage(requireObject(value, 'message'));
  }

  /** @param {number} fault */
  function decodeFaults(fault) {
    if (!Number.isInteger(fault) || fault < 0 || fault > 255) {
      throw new ProtocolError('fault must fit in one byte');
    }
    return Object.freeze({
      mask: fault,
      openThermocouple: Boolean(fault & FAULT_FLAGS.OPEN_THERMOCOUPLE),
      shortToGround: Boolean(fault & FAULT_FLAGS.SHORT_TO_GROUND),
      shortToVcc: Boolean(fault & FAULT_FLAGS.SHORT_TO_VCC),
      nonFinite: Boolean(fault & FAULT_FLAGS.NON_FINITE),
      unknownMask: fault & ~15,
    });
  }

  /** @param {'run.start' | 'run.stop' | 'preset'} type @param {string} [value] */
  function createCommand(type, value) {
    if (type === 'run.start' || type === 'run.stop') return Object.freeze({ type });
    if (type === 'preset' && typeof value === 'string' && value.length > 0) {
      return Object.freeze({ type, value });
    }
    throw new ProtocolError(`invalid command ${type}`);
  }

  root.PyroVue = Object.assign(root.PyroVue || {}, {
    PROTOCOL_VERSION,
    FAULT_FLAGS,
    ProtocolError,
    parseMessage,
    decodeFaults,
    createCommand,
  });
})(/** @type {any} */ (globalThis));
