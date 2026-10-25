// @ts-check
(function registerProtocol(root) {
  'use strict';

  const PROTOCOL_VERSION = 3;
  const MESSAGE_TYPES = new Set([
    'hello',
    'state',
    'history.begin',
    'history',
    'history.end',
    'sample',
    'error',
  ]);
  const LOGGING_STATES = new Set(['idle', 'recording', 'error', 'unavailable']);
  const REQUEST_ID_PATTERN = /^[A-Za-z0-9_-]{1,80}$/;
  const DEVICE_ID_PATTERN = /^[0-9a-f]{12}$/;
  const BOOT_ID_PATTERN = /^[0-9a-f]{16}$/;

  const FAULT_FLAGS = Object.freeze({
    OPEN_THERMOCOUPLE: 1,
    SHORT_TO_GROUND: 2,
    SHORT_TO_VCC: 4,
    NON_FINITE: 8,
  });

  let fallbackIdCounter = 0;

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
   * @param {Record<string, any>} value
   * @param {string} field
   * @param {RegExp} pattern
   * @param {string} description
   */
  function requirePattern(value, field, pattern, description) {
    const text = value[field];
    if (typeof text !== 'string' || !pattern.test(text)) {
      throw new ProtocolError(`${field} must be ${description}`);
    }
  }

  /** @param {Record<string, any>} value @param {string} field */
  function requireRequestId(value, field) {
    requirePattern(value, field, REQUEST_ID_PATTERN, '1..80 ASCII [A-Za-z0-9_-] characters');
  }

  /** @param {Record<string, any>} value @param {string} field */
  function requireDeviceId(value, field) {
    requirePattern(value, field, DEVICE_ID_PATTERN, '12 lowercase hex characters');
  }

  /** @param {Record<string, any>} value @param {string} field */
  function requireBootId(value, field) {
    requirePattern(value, field, BOOT_ID_PATTERN, '16 lowercase hex characters');
  }

  /** @param {Record<string, any>} value @param {string} field */
  function requireNullableErrorCode(value, field) {
    if (value[field] !== null && (typeof value[field] !== 'string' || value[field].length === 0)) {
      throw new ProtocolError(`${field} must be null or a non-empty string`);
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

  /** @param {Record<string, any>} message */
  function validateHello(message) {
    requireString(message, 'firmware');
    requireFiniteNumber(message, 'sampleHz');
    if (message.sampleHz <= 0) throw new ProtocolError('sampleHz must be positive');
    requireInteger(message, 'runId');
    requireBoolean(message, 'runActive');
    requireDeviceId(message, 'deviceId');
    requireBootId(message, 'bootId');
    requireInteger(message, 'acquireIntervalMs', true);
    requireInteger(message, 'logIntervalMs');
    requireInteger(message, 'maxRunMs');

    const capabilities = requireObject(message.capabilities, 'capabilities');
    requireBoolean(capabilities, 'runArchive');

    const logging = requireObject(message.logging, 'logging');
    if (!LOGGING_STATES.has(logging.state)) {
      throw new ProtocolError('logging.state must be idle, recording, error or unavailable');
    }
    requireInteger(logging, 'durableLogSeq');
    requireInteger(logging, 'latestAcqSeq');
    requireInteger(logging, 'bufferedRecords');
    requireInteger(logging, 'acquisitionGaps');
    requireInteger(logging, 'droppedAcquired');
    requireInteger(logging, 'queueHighWater');
    requireNullableErrorCode(logging, 'errorCode');

    const storage = requireObject(message.storage, 'storage');
    requireBoolean(storage, 'mounted');
    requireInteger(storage, 'totalBytes');
    requireInteger(storage, 'usedBytes');
    requireInteger(storage, 'freeBytes');
    requireInteger(storage, 'reservedRemainingBytes');
    requireInteger(storage, 'startRequiredBytes');

    if (message.runStartMs !== undefined) requireInteger(message, 'runStartMs');
    if (message.recentWindowMs !== undefined) requireInteger(message, 'recentWindowMs', true);
    if (message.coarseResolutionMs !== undefined) requireInteger(message, 'coarseResolutionMs', true);
    if (message.snapshotThroughSeq !== undefined) requireInteger(message, 'snapshotThroughSeq');
    if (message.snapshotId !== undefined) requireInteger(message, 'snapshotId');
    if (message.clients !== undefined) requireInteger(message, 'clients');
    if (message.preset !== undefined && typeof message.preset !== 'string') {
      throw new ProtocolError('preset must be a string');
    }
    return message;
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
        validateHello(message);
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
        requireString(message, 'message');
        if (message.requestId !== undefined) requireRequestId(message, 'requestId');
        break;
      default:
        throw new ProtocolError(`unhandled message type ${message.type}`);
    }
    return message;
  }

  /** @param {string | Record<string, any>} input */
  function parseJson(input) {
    if (typeof input !== 'string') return input;
    try {
      return JSON.parse(input);
    } catch (error) {
      throw new ProtocolError(`invalid JSON: ${error instanceof Error ? error.message : String(error)}`);
    }
  }

  /** @param {string | Record<string, any>} input */
  function parseMessage(input) {
    return validateMessage(requireObject(parseJson(input), 'message'));
  }

  /**
   * Strict parser for an HTTP error body:
   * {type:'error',protocol:3,code,message,requestId?}.
   * @param {string | Record<string, any>} input
   * @param {string} [expectedRequestId]
   */
  function parseError(input, expectedRequestId) {
    const message = requireObject(parseJson(input), 'error');
    if (message.type !== 'error') throw new ProtocolError('error type must be "error"');
    validateProtocol(message);
    requireString(message, 'code');
    requireString(message, 'message');
    if (message.requestId !== undefined) {
      requireRequestId(message, 'requestId');
      if (expectedRequestId !== undefined && message.requestId !== expectedRequestId) {
        throw new ProtocolError(`error requestId ${message.requestId} does not match ${expectedRequestId}`);
      }
    }
    return message;
  }

  /**
   * Strict parser for a durable command acknowledgement:
   * {type:'command.result',protocol:3,requestId,ok:true,runId,runActive,targetRunId,durable}.
   * @param {string | Record<string, any>} input
   * @param {{requestId?:string, targetRunId?:number, inactiveTarget?:boolean}} [expected]
   */
  function parseCommandResult(input, expected = {}) {
    const message = requireObject(parseJson(input), 'command result');
    if (message.type !== 'command.result') {
      throw new ProtocolError('command result type must be "command.result"');
    }
    validateProtocol(message);
    requireRequestId(message, 'requestId');
    if (expected.requestId !== undefined && message.requestId !== expected.requestId) {
      throw new ProtocolError(`command result requestId ${message.requestId} does not match ${expected.requestId}`);
    }
    requireBoolean(message, 'ok');
    if (message.ok !== true) {
      requireString(message, 'errorCode');
      return message;
    }
    requireInteger(message, 'runId');
    requireInteger(message, 'targetRunId');
    requireBoolean(message, 'runActive');
    requireBoolean(message, 'durable');
    if (message.errorCode !== undefined && message.errorCode !== null) {
      if (typeof message.errorCode !== 'string' || message.errorCode.length === 0) {
        throw new ProtocolError('command result errorCode must be null or a non-empty string');
      }
    }
    if (expected.targetRunId !== undefined && message.targetRunId !== expected.targetRunId) {
      throw new ProtocolError(
        `command result targetRunId ${message.targetRunId} does not match ${expected.targetRunId}`,
      );
    }
    // A completed stop result whose target is still the current run cannot
    // report that run as active.
    if (expected.inactiveTarget === true
        && message.targetRunId === message.runId
        && message.runActive) {
      throw new ProtocolError('command result contradicts the stopped target state');
    }
    return message;
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

  /** @param {Record<string, any>} source @param {string[]} parts */
  function collectErrorParts(source, parts) {
    if (typeof source.code === 'string' && source.code.length > 0) parts.push(source.code);
    if (typeof source.message === 'string' && source.message.length > 0) parts.push(source.message);
    else if (typeof source.error === 'string' && source.error.length > 0) parts.push(source.error);
  }

  /**
   * Extracts a human-meaningful detail from a device error payload. Accepts the
   * strict protocol error shape ({code,message}), other HTTP JSON error bodies
   * ({error} or {code, message}), or a plain-text body. Returns null when
   * nothing useful is present so callers can fall back to the HTTP status.
   * @param {unknown} body
   * @returns {string | null}
   */
  function readErrorDetail(body) {
    if (typeof body === 'string') {
      const trimmed = body.trim();
      return trimmed.length > 0 ? trimmed : null;
    }
    if (!body || typeof body !== 'object' || Array.isArray(body)) return null;
    const source = /** @type {Record<string, any>} */ (body);
    /** @type {string[]} */
    const parts = [];
    collectErrorParts(source, parts);
    if (source.error && typeof source.error === 'object' && !Array.isArray(source.error)) {
      collectErrorParts(/** @type {Record<string, any>} */ (source.error), parts);
    }
    /** @type {string[]} */
    const unique = [];
    for (const part of parts) {
      if (!unique.includes(part)) unique.push(part);
    }
    return unique.length > 0 ? unique.join(': ') : null;
  }

  /**
   * Generates one operation identity using the historical marker ID algorithm.
   * Command requestIds and note operationIds share this convention
   * (ASCII [A-Za-z0-9_-], <=80 characters).
   */
  function createOperationId() {
    if (root.crypto && typeof root.crypto.randomUUID === 'function') {
      return root.crypto.randomUUID();
    }
    fallbackIdCounter += 1;
    return `${Date.now().toString(36)}-${Math.random().toString(36).slice(2)}-${fallbackIdCounter.toString(36)}`;
  }

  /**
   * Builds the exact protocol-3 command body. run.start maps the current
   * runId to afterRunId; run.stop uses it as the target runId.
   * @param {'run.start' | 'run.stop' | 'preset'} type
   * @param {{requestId:string, bootId:string, runId:number, wallStartUtcMs?:number}} context
   * @param {string} [value]
   */
  function createCommand(type, context, value) {
    if (type !== 'run.start' && type !== 'run.stop' && type !== 'preset') {
      throw new ProtocolError(`invalid command ${type}`);
    }
    const source = requireObject(context, 'command context');
    const requestId = source.requestId;
    const bootId = source.bootId;
    const runId = source.runId;
    requireRequestId(source, 'requestId');
    requireBootId(source, 'bootId');
    requireInteger(source, 'runId');
    if (source.wallStartUtcMs !== undefined) requireInteger(source, 'wallStartUtcMs');

    if (type === 'run.start') {
      /** @type {Record<string, any>} */
      const command = { type, requestId, bootId, afterRunId: runId };
      if (source.wallStartUtcMs !== undefined) command.wallStartUtcMs = source.wallStartUtcMs;
      return Object.freeze(command);
    }
    if (type === 'run.stop') {
      if (runId === 0) throw new ProtocolError('run.stop requires a positive runId');
      return Object.freeze({ type, requestId, bootId, runId });
    }
    if (typeof value !== 'string' || value.length === 0) {
      throw new ProtocolError('invalid command preset');
    }
    return Object.freeze({ type, requestId, bootId, value });
  }

  root.PyroVue = Object.assign(root.PyroVue || {}, {
    PROTOCOL_VERSION,
    FAULT_FLAGS,
    ProtocolError,
    parseMessage,
    parseError,
    parseCommandResult,
    decodeFaults,
    createOperationId,
    createCommand,
    readErrorDetail,
  });
})(/** @type {any} */ (globalThis));
