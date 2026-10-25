#include "config.h"

#if defined(PYROVUE_BOARD_N16R8)

#include "RunLogService.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>

#include <new>
#include <stdio.h>
#include <string.h>

#include "HistoryStore.h"
#include "MarkerReducer.h"
#include "RunArchiveStore.h"
#include "RunManager.h"

// One shared PSRAM scratch for marker mutations. Keeping both mutation
// domains, the reduced marker state and the event JSON here prevents several
// kilobytes of transient stack use in the 8 KB owner-loop task.
struct RunLogService::MarkerScratch {
    MarkerMutation work{};
    MarkerMutation replay{};
    MarkerState marker{};
    MutationResult prior{};
    char json[runlog::kMaxEventPayloadBytes + 1]{};
};

namespace {

// Returns the byte length of the codepoint starting at `at`, or 0 for a
// malformed or non-canonical sequence (overlong form, surrogate, >U+10FFFF).
size_t decodeCodepoint(const char* text, size_t length, size_t at,
                       uint32_t& codepoint) {
    const uint8_t b0 = static_cast<uint8_t>(text[at]);
    if (b0 < 0x80) {
        codepoint = b0;
        return 1;
    }
    if ((b0 & 0xE0) == 0xC0) {
        if (at + 1 >= length) return 0;
        const uint8_t b1 = static_cast<uint8_t>(text[at + 1]);
        if ((b1 & 0xC0) != 0x80) return 0;
        codepoint = (static_cast<uint32_t>(b0 & 0x1F) << 6) |
                    static_cast<uint32_t>(b1 & 0x3F);
        return codepoint >= 0x80 ? 2 : 0;
    }
    if ((b0 & 0xF0) == 0xE0) {
        if (at + 2 >= length) return 0;
        const uint8_t b1 = static_cast<uint8_t>(text[at + 1]);
        const uint8_t b2 = static_cast<uint8_t>(text[at + 2]);
        if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80) return 0;
        codepoint = (static_cast<uint32_t>(b0 & 0x0F) << 12) |
                    (static_cast<uint32_t>(b1 & 0x3F) << 6) |
                    static_cast<uint32_t>(b2 & 0x3F);
        if (codepoint < 0x800 || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
            return 0;
        }
        return 3;
    }
    if ((b0 & 0xF8) == 0xF0) {
        if (at + 3 >= length) return 0;
        const uint8_t b1 = static_cast<uint8_t>(text[at + 1]);
        const uint8_t b2 = static_cast<uint8_t>(text[at + 2]);
        const uint8_t b3 = static_cast<uint8_t>(text[at + 3]);
        if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80 ||
            (b3 & 0xC0) != 0x80) {
            return 0;
        }
        codepoint = (static_cast<uint32_t>(b0 & 0x07) << 18) |
                    (static_cast<uint32_t>(b1 & 0x3F) << 12) |
                    (static_cast<uint32_t>(b2 & 0x3F) << 6) |
                    static_cast<uint32_t>(b3 & 0x3F);
        if (codepoint < 0x10000 || codepoint > 0x10FFFF) return 0;
        return 4;
    }
    return 0;
}

// ECMAScript String.trim() whitespace set, shared with the browser editor.
bool ecmaWhitespace(uint32_t codepoint) {
    if (codepoint >= 0x09 && codepoint <= 0x0D) return true;
    switch (codepoint) {
        case 0x20:
        case 0xA0:
        case 0x1680:
        case 0x2028:
        case 0x2029:
        case 0x202F:
        case 0x205F:
        case 0x3000:
        case 0xFEFF:
            return true;
        default:
            return codepoint >= 0x2000 && codepoint <= 0x200A;
    }
}

// Trims leading/trailing ECMAScript whitespace codepoints, preserving every
// byte of the remaining text (including interior whitespace and Unicode).
// Malformed sequences are treated as ordinary text so validateNote, not this
// function, decides acceptance.
bool normalizeNote(const char* note, char* out, size_t capacity) {
    if (note == nullptr || out == nullptr || capacity == 0) return false;
    const size_t length = strlen(note);
    size_t begin = 0;
    size_t end = 0;
    bool hasText = false;
    for (size_t at = 0; at < length;) {
        uint32_t codepoint = 0;
        size_t advance = decodeCodepoint(note, length, at, codepoint);
        if (advance == 0) {
            if (!hasText) {
                begin = at;
                hasText = true;
            }
            ++at;
            end = at;
            continue;
        }
        if (ecmaWhitespace(codepoint)) {
            at += advance;
            continue;
        }
        if (!hasText) {
            begin = at;
            hasText = true;
        }
        at += advance;
        end = at;
    }
    const size_t trimmed = hasText ? end - begin : 0;
    if (trimmed + 1 > capacity) return false;
    if (trimmed > 0) memcpy(out, note + begin, trimmed);
    out[trimmed] = '\0';
    return true;
}

const char* eventTypeName(MarkerMutationKind kind) {
    switch (kind) {
        case MarkerMutationKind::Create: return "marker.created";
        case MarkerMutationKind::Update: return "marker.updated";
        case MarkerMutationKind::Delete: return "marker.deleted";
    }
    return "marker.created";
}

const char* timeBasisName(TimeBasis basis);

// Single deterministic journal serializer for both new appends and
// idempotent re-syncs, so the store can byte-compare a repeated last frame.
size_t buildMutationJson(const MarkerMutation& mutation, uint32_t revision,
                         char* out, size_t capacity) {
    JsonDocument doc;
    doc["revision"] = revision;
    doc["type"] = eventTypeName(mutation.kind);
    doc["operationId"] = mutation.operationId;
    doc["id"] = mutation.id;
    doc["baseRevision"] = mutation.baseRevision;
    if (mutation.kind != MarkerMutationKind::Delete) {
        if (mutation.hasMs) {
            doc["ms"] = mutation.ms;
        } else {
            doc["ms"] = nullptr;
        }
        doc["note"] = mutation.note;
        doc["timeBasis"] = timeBasisName(mutation.timeBasis);
    }
    return serializeJson(doc, out, capacity);
}

void copyText(char* dst, size_t capacity, const char* src) {
    if (dst == nullptr || capacity == 0) return;
    if (src == nullptr) {
        dst[0] = '\0';
        return;
    }
    size_t i = 0;
    for (; i + 1 < capacity && src[i] != '\0'; ++i) dst[i] = src[i];
    dst[i] = '\0';
}

bool validToken(const char* text, size_t maxBytes) {
    if (text == nullptr || text[0] == '\0') return false;
    size_t len = 0;
    for (; text[len] != '\0'; ++len) {
        if (len >= maxBytes) return false;
        const char c = text[len];
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return len > 0;
}

// Note text is stored exactly; this only rejects malformed/oversized input.
ArchiveError validateNote(const char* note) {
    if (note == nullptr) return ArchiveError::INVALID_REQUEST;
    const size_t len = strlen(note);
    if (len > runlog::kMaxNoteBytes) return ArchiveError::INVALID_REQUEST;
    size_t scalars = 0;
    for (size_t i = 0; i < len;) {
        const uint8_t b = static_cast<uint8_t>(note[i]);
        if (b == 0) return ArchiveError::INVALID_REQUEST;
        if (b < 0x80) {
            ++i;
        } else if ((b & 0xE0) == 0xC0) {
            if (i + 1 >= len || (static_cast<uint8_t>(note[i + 1]) & 0xC0) != 0x80) {
                return ArchiveError::INVALID_REQUEST;
            }
            const uint32_t cp = (static_cast<uint32_t>(b & 0x1F) << 6) |
                                static_cast<uint8_t>(note[i + 1] & 0x3F);
            if (cp < 0x80) return ArchiveError::INVALID_REQUEST;
            i += 2;
        } else if ((b & 0xF0) == 0xE0) {
            if (i + 2 >= len) return ArchiveError::INVALID_REQUEST;
            const uint8_t b1 = static_cast<uint8_t>(note[i + 1]);
            const uint8_t b2 = static_cast<uint8_t>(note[i + 2]);
            if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80) {
                return ArchiveError::INVALID_REQUEST;
            }
            const uint32_t cp = (static_cast<uint32_t>(b & 0x0F) << 12) |
                                (static_cast<uint32_t>(b1 & 0x3F) << 6) |
                                static_cast<uint32_t>(b2 & 0x3F);
            if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF)) {
                return ArchiveError::INVALID_REQUEST;
            }
            i += 3;
        } else if ((b & 0xF8) == 0xF0) {
            if (i + 3 >= len) return ArchiveError::INVALID_REQUEST;
            const uint8_t b1 = static_cast<uint8_t>(note[i + 1]);
            const uint8_t b2 = static_cast<uint8_t>(note[i + 2]);
            const uint8_t b3 = static_cast<uint8_t>(note[i + 3]);
            if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80 ||
                (b3 & 0xC0) != 0x80) {
                return ArchiveError::INVALID_REQUEST;
            }
            const uint32_t cp = (static_cast<uint32_t>(b & 0x07) << 18) |
                                (static_cast<uint32_t>(b1 & 0x3F) << 12) |
                                (static_cast<uint32_t>(b2 & 0x3F) << 6) |
                                static_cast<uint32_t>(b3 & 0x3F);
            if (cp < 0x10000 || cp > 0x10FFFF) return ArchiveError::INVALID_REQUEST;
            i += 4;
        } else {
            return ArchiveError::INVALID_REQUEST;
        }
        if (++scalars > runlog::kMaxNoteScalars) return ArchiveError::INVALID_REQUEST;
    }
    return ArchiveError::NONE;
}

const char* timeBasisName(TimeBasis basis) {
    switch (basis) {
        case TimeBasis::Estimated: return "estimated";
        case TimeBasis::Manual: return "manual";
        case TimeBasis::Untimed: return "untimed";
    }
    return "untimed";
}

bool sameMutation(const MarkerMutation& a, const MarkerMutation& b) {
    return a.kind == b.kind && strcmp(a.id, b.id) == 0 &&
           a.baseRevision == b.baseRevision && a.hasMs == b.hasMs &&
           (!a.hasMs || a.ms == b.ms) && strcmp(a.note, b.note) == 0 &&
           a.timeBasis == b.timeBasis;
}

}  // namespace

RunLogService::RunLogService(RunManager& runManager, HistoryStore& history,
                             RunArchiveStore& store,
                             const DeviceIdentity& identity)
    : runManager_(runManager),
      history_(history),
      store_(store),
      identity_(identity) {
    published_.identity = identity_;
    copyText(published_.firmware, sizeof(published_.firmware), FIRMWARE_VERSION);
    published_.protocolVersion = PROTOCOL_VERSION;
    published_.sampleHz = SAMPLE_HZ;
    published_.runArchive = true;
    published_.acquireIntervalMs = 1000U / SAMPLE_HZ;
    published_.logIntervalMs = RUN_LOG_INTERVAL_MS;
    published_.maxRunMs = RUN_MAX_DURATION_MS;
    published_.logging.state = LoggingState::Unavailable;
    published_.logging.error = ArchiveError::ARCHIVE_UNAVAILABLE;
    clearQueue();
}

RunLogService::~RunLogService() {
    if (markerScratch_ != nullptr) {
        markerScratch_->~MarkerScratch();
        heap_caps_free(markerScratch_);
        markerScratch_ = nullptr;
    }
}

bool RunLogService::ensureMarkerScratch() {
    if (markerScratch_ != nullptr) return true;
    void* memory = heap_caps_malloc(sizeof(MarkerScratch),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (memory == nullptr) {
        memory = heap_caps_malloc(sizeof(MarkerScratch), MALLOC_CAP_8BIT);
    }
    if (memory == nullptr) return false;
    markerScratch_ = new (memory) MarkerScratch();
    return true;
}

ArchiveError RunLogService::begin(uint32_t nowMs) {
    clearQueue();
    recorder_.reset(0);
    recording_ = false;
    hasDirty_ = false;
    overflowed_ = false;
    storeFailure_ = false;
    storeFailureError_ = ArchiveError::NONE;
    dirtySinceMs_ = 0;
    durableLogSeq_ = 0;
    lastSelectedLogSeq_ = 0;
    latestAcqSeq_ = 0;
    acquisitionGaps_ = 0;
    droppedAcquired_ = 0;
    queueHighWater_ = 0;
    catalogValid_ = false;
    catalogError_ = ArchiveError::ARCHIVE_UNAVAILABLE;
    ensureMarkerScratch();

    const ArchiveError mount = store_.begin(identity_.deviceId, identity_.bootId);
    CatalogSummary catalog{};
    const ArchiveError scan = mount == ArchiveError::NONE ? store_.scanCatalog(catalog) : mount;
    runManager_.begin(nowMs, scan == ArchiveError::NONE ? catalog.maxRunId : 0);
    catalogValid_ = mount == ArchiveError::NONE && scan == ArchiveError::NONE;
    catalogError_ = mount != ArchiveError::NONE ? mount : scan;

    storageCache_ = store_.storageStatus();
    lastStorageRefreshMs_ = nowMs;
    statusInitialized_ = true;

    if (!storageCache_.mounted || scan == ArchiveError::ARCHIVE_UNAVAILABLE) {
        state_ = LoggingState::Unavailable;
        loggingError_ = ArchiveError::ARCHIVE_UNAVAILABLE;
    } else if (scan != ArchiveError::NONE) {
        state_ = LoggingState::Error;
        loggingError_ = scan;
    } else {
        state_ = LoggingState::Idle;
        loggingError_ = ArchiveError::NONE;
    }
    refreshPublishedStatus(nowMs, true);
    return scan;
}

bool RunLogService::enqueueSample(const TempSample& sample) {
    if (!recording_ || !runManager_.active()) return false;
    if (sample.runId != runManager_.runId()) return false;

    if (sample.seq > latestAcqSeq_) latestAcqSeq_ = sample.seq;

    if (overflowed_ || storeFailure_) {
        ++droppedAcquired_;
        return false;
    }
    if (queueCount_ >= RUN_SAMPLE_QUEUE_CAPACITY) {
        ++droppedAcquired_;
        overflowed_ = true;
        return false;
    }
    pushQueued(sample);
    if (queueCount_ > queueHighWater_) queueHighWater_ = queueCount_;
    return true;
}

void RunLogService::enforceLimit(uint32_t nowMs) {
    if (!recording_) return;
    const uint32_t elapsed = runManager_.elapsedMs(nowMs);
    if (elapsed < RUN_MAX_DURATION_MS) return;

    if (overflowed_) {
        drainQueue(false);
        finalizeRun(RunStatus::StorageError, ArchiveError::STORAGE_UNAVAILABLE, "", nowMs);
    } else if (storeFailure_) {
        finalizeRun(RunStatus::StorageError, storeFailureError_, "", nowMs);
    } else {
        // Every queued sample older than the limit is selectable; the recorder
        // itself refuses records beyond the run window.
        drainQueue(true);
        if (storeFailure_) {
            finalizeRun(RunStatus::StorageError, storeFailureError_, "", nowMs);
        } else {
            finalizeRun(RunStatus::DurationLimit, ArchiveError::NONE, "", nowMs);
        }
    }
}

void RunLogService::poll(uint32_t nowMs) {
    if (!recording_) return;

    const uint32_t elapsed = runManager_.elapsedMs(nowMs);

    if (overflowed_) {
        drainQueue(false);
        finalizeRun(RunStatus::StorageError, ArchiveError::STORAGE_UNAVAILABLE, "", nowMs);
        return;
    }
    if (storeFailure_) {
        finalizeRun(RunStatus::StorageError, storeFailureError_, "", nowMs);
        return;
    }

    drainQueue(false);
    if (storeFailure_) {
        finalizeRun(RunStatus::StorageError, storeFailureError_, "", nowMs);
        return;
    }

    if (hasDirty_ && static_cast<uint32_t>(elapsed - dirtySinceMs_) >= RUN_LOG_FLUSH_MS) {
        const ArchiveError syncError = syncPending();
        if (syncError != ArchiveError::NONE) {
            finalizeRun(RunStatus::StorageError, syncError, "", nowMs);
            return;
        }
        refreshPublishedStatus(nowMs, true);
        return;
    }

    if (!statusInitialized_ || static_cast<uint32_t>(nowMs - lastStorageRefreshMs_) >= 1000U) {
        refreshPublishedStatus(nowMs, true);
    }
}

CommandResult RunLogService::handleCommand(const CommandRequest& request) {
    // One accepted time per owner-loop command; every origin/metadata stamp
    // below uses this value.
    const uint32_t nowMs = millis();

    CommandResult result{};
    copyText(result.requestId, sizeof(result.requestId), request.requestId);
    result.runId = runManager_.runId();
    result.runActive = runManager_.active();

    if (request.command == CommandKind::Preset) {
        // Preset state belongs to main/PresetManager, not the archive service.
        result.error = ArchiveError::INVALID_REQUEST;
        refreshPublishedStatus(nowMs, true);
        return result;
    }

    if (request.command == CommandKind::Start) {
        if (!validToken(request.requestId, 80)) {
            result.error = ArchiveError::INVALID_REQUEST;
            refreshPublishedStatus(nowMs, true);
            return result;
        }
        bool found = false;
        RunMeta existing{};
        const ArchiveError lookup = store_.findStartRequest(request.requestId, found, existing);
        if (lookup != ArchiveError::NONE) {
            result.error = lookup;
            refreshPublishedStatus(nowMs, true);
            return result;
        }
        if (found) {
            // Replayed start: report the persisted target without reactivation.
            result.ok = true;
            result.targetRunId = existing.runId;
            result.runId = runManager_.runId();
            result.runActive = runManager_.active();
            result.durable = true;
            refreshPublishedStatus(nowMs, true);
            return result;
        }
        if (!catalogValid_) {
            // A new allocation without validated archive ids could reuse one.
            // Replays above are reads and remain available.
            result.error = catalogError_ == ArchiveError::NONE
                               ? ArchiveError::STORAGE_UNAVAILABLE
                               : catalogError_;
            refreshPublishedStatus(nowMs, true);
            return result;
        }

        if (runManager_.active()) {
            result.error = ArchiveError::STALE_COMMAND;
            refreshPublishedStatus(nowMs, true);
            return result;
        }
        if (strncmp(request.bootId, identity_.bootId, sizeof(request.bootId)) != 0 ||
            request.afterRunId != runManager_.runId()) {
            result.error = ArchiveError::STALE_COMMAND;
            refreshPublishedStatus(nowMs, true);
            return result;
        }

        ArchiveError preflight = store_.checkStart();
        if (preflight != ArchiveError::NONE) {
            result.error = preflight;
            refreshPublishedStatus(nowMs, true);
            return result;
        }

        const RunManager::StartResult prepared = runManager_.prepareRun(nowMs);
        if (prepared == RunManager::START_ALREADY_ACTIVE) {
            result.error = ArchiveError::STALE_COMMAND;
            refreshPublishedStatus(nowMs, true);
            return result;
        }
        if (prepared == RunManager::START_STORE_FAILED) {
            result.error = ArchiveError::RUN_ID_SAVE_FAILED;
            refreshPublishedStatus(nowMs, true);
            return result;
        }
        if (prepared == RunManager::START_ID_EXHAUSTED) {
            result.error = ArchiveError::RUN_ID_EXHAUSTED;
            refreshPublishedStatus(nowMs, true);
            return result;
        }

        RunStart start{};
        start.runId = runManager_.runId();
        start.startUptimeMs = nowMs;
        copyText(start.startRequestId, sizeof(start.startRequestId), request.requestId);
        start.identity = identity_;
        copyText(start.firmware, sizeof(start.firmware), FIRMWARE_VERSION);
        start.protocolVersion = PROTOCOL_VERSION;
        start.acquireIntervalMs = 1000U / SAMPLE_HZ;
        start.logIntervalMs = RUN_LOG_INTERVAL_MS;
        start.graphBucketMs = COARSE_BUCKET_MS;
        start.maxRunMs = RUN_MAX_DURATION_MS;
        start.hasWallStartUtcMs = request.hasWallStartUtcMs;
        start.wallStartUtcMs = request.wallStartUtcMs;
        start.wallTimeSource = request.hasWallStartUtcMs
                                   ? WallTimeSource::ClientEstimated
                                   : WallTimeSource::Unavailable;

        RunMeta meta{};
        const ArchiveError preparedError = store_.prepareRun(start, meta);
        if (preparedError != ArchiveError::NONE) {
            // The allocated identity stays consumed; partial staging remains
            // interrupted/corrupt and is never rolled back.
            runManager_.abandonPreparedRun();
            result.runId = runManager_.runId();
            result.runActive = false;
            result.durable = false;
            result.error = preparedError;
            refreshPublishedStatus(nowMs, true);
            return result;
        }
        if (!runManager_.activatePreparedRun()) {
            runManager_.abandonPreparedRun();
            result.runId = runManager_.runId();
            result.runActive = false;
            result.durable = false;
            result.error = ArchiveError::WRITE_FAILED;
            refreshPublishedStatus(nowMs, true);
            return result;
        }

        const uint32_t runId = meta.runId != 0 ? meta.runId : runManager_.runId();
        recorder_.reset(runId);
        history_.clear(runId);
        clearQueue();
        hasDirty_ = false;
        overflowed_ = false;
        storeFailure_ = false;
        storeFailureError_ = ArchiveError::NONE;
        dirtySinceMs_ = 0;
        durableLogSeq_ = 0;
        lastSelectedLogSeq_ = 0;
        latestAcqSeq_ = 0;
        acquisitionGaps_ = 0;
        droppedAcquired_ = 0;
        queueHighWater_ = 0;
        recording_ = true;
        state_ = LoggingState::Recording;
        loggingError_ = ArchiveError::NONE;

        result.ok = true;
        result.runId = runId;
        result.runActive = true;
        result.targetRunId = runId;
        result.durable = true;
        refreshPublishedStatus(nowMs, true);
        return result;
    }

    // Stop.
    if (request.targetRunId == 0 || !validToken(request.requestId, 80)) {
        result.error = ArchiveError::INVALID_REQUEST;
        refreshPublishedStatus(nowMs, true);
        return result;
    }

    if (runManager_.active() && request.targetRunId == runManager_.runId()) {
        if (strncmp(request.bootId, identity_.bootId, sizeof(request.bootId)) != 0) {
            result.error = ArchiveError::STALE_COMMAND;
            refreshPublishedStatus(nowMs, true);
            return result;
        }

        drainQueue(false);
        RunStatus terminalStatus = RunStatus::Complete;
        ArchiveError terminalCause = ArchiveError::NONE;
        if (overflowed_) {
            terminalStatus = RunStatus::StorageError;
            terminalCause = ArchiveError::STORAGE_UNAVAILABLE;
        } else if (storeFailure_) {
            terminalStatus = RunStatus::StorageError;
            terminalCause = storeFailureError_;
        }
        const ArchiveError finalError =
            finalizeRun(terminalStatus, terminalCause, request.requestId, nowMs);
        result.runId = runManager_.runId();
        result.runActive = false;
        result.targetRunId = request.targetRunId;
        if (finalError == ArchiveError::NONE) {
            result.ok = true;
            result.durable = true;
        } else {
            result.error = finalError;
            result.durable = false;
        }
        refreshPublishedStatus(nowMs, true);
        return result;
    }

    // Already-ended target: an idempotent retry is harmless success and can
    // never stop the current run. This matches the frozen wire contract; a
    // failed first attempt already returned its actual non-200 result.
    RunMeta ended{};
    const ArchiveError metaError = store_.getMeta(request.targetRunId, ended);
    if (metaError != ArchiveError::NONE) {
        result.error = metaError;
    } else if (ended.status == RunStatus::Active) {
        // The only active target is the current run, handled above.
        result.error = ArchiveError::STALE_COMMAND;
    } else {
        result.ok = true;
        result.durable = true;
        result.targetRunId = request.targetRunId;
    }
    result.runId = runManager_.runId();
    result.runActive = runManager_.active();
    refreshPublishedStatus(nowMs, true);
    return result;
}

bool RunLogService::mutateMarkerStep(uint32_t runId, const MarkerMutation& mutation,
                                     uint32_t nowMs, MutationResult& out,
                                     ArchiveError& error) {
    out = MutationResult{};
    out.runId = runId;
    copyText(out.operationId, sizeof(out.operationId), mutation.operationId);
    copyText(out.id, sizeof(out.id), mutation.id);
    out.persisted = false;
    out.deleted = mutation.kind == MarkerMutationKind::Delete;
    error = ArchiveError::NONE;

    if (runId == 0 || !validToken(mutation.operationId, 80) ||
        !validToken(mutation.id, 80)) {
        error = ArchiveError::INVALID_REQUEST;
        return true;
    }
    if (!ensureMarkerScratch()) {
        error = ArchiveError::STORAGE_UNAVAILABLE;
        return true;
    }
    MarkerScratch& scratch = *markerScratch_;
    MarkerMutation& work = scratch.work;
    new (&work) MarkerMutation{};
    work.kind = mutation.kind;
    copyText(work.operationId, sizeof(work.operationId), mutation.operationId);
    copyText(work.id, sizeof(work.id), mutation.id);
    work.baseRevision = mutation.baseRevision;
    if (mutation.kind != MarkerMutationKind::Delete) {
        work.hasMs = mutation.hasMs;
        work.ms = mutation.ms;
        work.timeBasis = mutation.timeBasis;
        // The editor trims before save; the journal stores the normalized
        // text exactly. Duplicate detection compares this same domain.
        if (!normalizeNote(mutation.note, work.note, sizeof(work.note))) {
            error = ArchiveError::INVALID_REQUEST;
            return true;
        }
        const ArchiveError noteError = validateNote(work.note);
        if (noteError != ArchiveError::NONE) {
            error = noteError;
            return true;
        }
    }

    ArchiveError warmError = ArchiveError::NONE;
    if (!store_.warmMarkerCacheStep(runId, warmError)) return false;
    if (warmError != ArchiveError::NONE) {
        error = warmError;
        return true;
    }
    const MarkerReducer& reducer = store_.markerReducer();
    if (!reducer.ready()) return false;

    out.revision = reducer.revision();

    // Operation lookup precedes every revision/time check so a retry of an
    // already-journaled mutation replays its original durable outcome.
    // warmMarkerCacheStep is ready only at the current durable revision (it
    // recovers/truncates and fsyncs before reporting ready), so a reducer hit
    // here is a durable result, not a cached unacknowledged frame.
    MarkerMutation& replay = scratch.replay;
    MutationResult& prior = scratch.prior;
    memset(&replay, 0, sizeof(replay));
    memset(&prior, 0, sizeof(prior));
    if (reducer.lookupOperation(work.operationId, replay, prior)) {
        if (!sameMutation(replay, work)) {
            error = ArchiveError::OPERATION_ID_CONFLICT;
            return true;
        }
        out = prior;
        out.persisted = true;
        error = ArchiveError::NONE;
        return true;
    }

    MarkerState& state = scratch.marker;
    memset(&state, 0, sizeof(state));
    const bool markerKnown = reducer.lookupMarker(work.id, state);
    if (work.kind == MarkerMutationKind::Create) {
        if (work.baseRevision != 0 || markerKnown) {
            error = ArchiveError::REVISION_CONFLICT;
            return true;
        }
    } else {
        if (!markerKnown || state.deleted) {
            error = ArchiveError::MARKER_NOT_FOUND;
            return true;
        }
        if (state.revision != work.baseRevision) {
            error = ArchiveError::REVISION_CONFLICT;
            return true;
        }
    }

    if (work.kind != MarkerMutationKind::Delete) {
        if ((work.timeBasis == TimeBasis::Untimed) == work.hasMs) {
            error = ArchiveError::EVENT_TIME_OUT_OF_RANGE;
            return true;
        }
        if (work.hasMs) {
            uint32_t boundary = 0;
            const ArchiveError boundaryError =
                markerBoundary(runId, nowMs, boundary);
            if (boundaryError != ArchiveError::NONE) {
                error = boundaryError;
                return true;
            }
            if (work.ms > boundary) {
                error = ArchiveError::EVENT_TIME_OUT_OF_RANGE;
                return true;
            }
        }
    }

    const uint32_t nextRevision = reducer.revision() + 1;
    const size_t length = buildMutationJson(work, nextRevision, scratch.json,
                                            sizeof(scratch.json));
    if (length == 0 || length > runlog::kMaxEventPayloadBytes) {
        error = ArchiveError::BODY_TOO_LARGE;
        return true;
    }

    uint32_t durableRevision = 0;
    const ArchiveError appendError = store_.appendEventAndSync(
        runId, reinterpret_cast<const uint8_t*>(scratch.json), length,
        durableRevision);
    if (appendError != ArchiveError::NONE) {
        error = appendError;
        out.persisted = false;
        return true;
    }

    out.runId = runId;
    copyText(out.operationId, sizeof(out.operationId), work.operationId);
    copyText(out.id, sizeof(out.id), work.id);
    out.revision = durableRevision;
    out.persisted = true;
    out.deleted = work.kind == MarkerMutationKind::Delete;
    error = ArchiveError::NONE;
    return true;
}

RunArchiveStore& RunLogService::archive() { return store_; }

void RunLogService::copyPublishedStatus(NetworkStatusSnapshot& out) const {
#if defined(ARDUINO_ARCH_ESP32)
    portENTER_CRITICAL(&statusMux_);
    out = published_;
    portEXIT_CRITICAL(&statusMux_);
#else
    out = published_;
#endif
}

void RunLogService::clearQueue() {
    queueHead_ = 0;
    queueTail_ = 0;
    queueCount_ = 0;
}

void RunLogService::pushQueued(const TempSample& sample) {
    queue_[queueTail_] = sample;
    queueTail_ = (queueTail_ + 1U) % RUN_SAMPLE_QUEUE_CAPACITY;
    ++queueCount_;
}

bool RunLogService::popQueued(TempSample& out) {
    if (queueCount_ == 0) return false;
    out = queue_[queueHead_];
    queueHead_ = (queueHead_ + 1U) % RUN_SAMPLE_QUEUE_CAPACITY;
    --queueCount_;
    return true;
}

void RunLogService::drainQueue(bool limitOnly) {
    TempSample sample{};
    while (!storeFailure_ && popQueued(sample)) {
        if (limitOnly && sample.ms > RUN_MAX_DURATION_MS) continue;
        LoggedSample record{};
        if (!recorder_.select(sample, record)) continue;
        appendSelected(record);
    }
}

void RunLogService::appendSelected(const LoggedSample& record) {
    const ArchiveError error = store_.appendSelected(runManager_.runId(), record);
    if (error != ArchiveError::NONE) {
        storeFailure_ = true;
        storeFailureError_ = error;
        return;
    }
    lastSelectedLogSeq_ = record.logSeq;
    if (!hasDirty_) {
        hasDirty_ = true;
        dirtySinceMs_ = record.ms;
    }
    if ((record.flags & runlog::kAcquisitionGap) != 0) ++acquisitionGaps_;
}

ArchiveError RunLogService::syncPending() {
    if (!hasDirty_) return ArchiveError::NONE;
    const ArchiveError error = store_.sync(runManager_.runId());
    if (error != ArchiveError::NONE) {
        storeFailure_ = true;
        storeFailureError_ = error;
        return error;
    }
    durableLogSeq_ = lastSelectedLogSeq_;
    hasDirty_ = false;
    return ArchiveError::NONE;
}

ArchiveError RunLogService::finalizeRun(RunStatus status, ArchiveError cause,
                                        const char* requestId, uint32_t nowMs) {
    if (!recording_) return ArchiveError::NONE;
    const uint32_t runId = runManager_.runId();
    const uint32_t endMs = status == RunStatus::DurationLimit
                               ? RUN_MAX_DURATION_MS
                               : runManager_.elapsedMs(nowMs);

    ArchiveError failure = ArchiveError::NONE;

    if (!storeFailure_) {
        LoggedSample terminal{};
        if (recorder_.finish(terminal)) {
            const ArchiveError error = store_.appendSelected(runId, terminal);
            if (error != ArchiveError::NONE) {
                storeFailure_ = true;
                storeFailureError_ = error;
                status = RunStatus::StorageError;
                if (cause == ArchiveError::NONE) cause = error;
                failure = error;
            } else {
                lastSelectedLogSeq_ = terminal.logSeq;
                if (!hasDirty_) {
                    hasDirty_ = true;
                    dirtySinceMs_ = terminal.ms;
                }
                if ((terminal.flags & runlog::kAcquisitionGap) != 0) {
                    ++acquisitionGaps_;
                }
            }
        }
    }

    if (hasDirty_) {
        const ArchiveError syncError = store_.sync(runId);
        if (syncError != ArchiveError::NONE) {
            status = RunStatus::StorageError;
            if (cause == ArchiveError::NONE) cause = syncError;
            failure = syncError;
        } else {
            durableLogSeq_ = lastSelectedLogSeq_;
        }
        hasDirty_ = false;
    }

    const ArchiveError finalizeError = store_.finalize(
        runId, status, endMs, requestId == nullptr ? "" : requestId,
        droppedAcquired_, cause);
    if (finalizeError != ArchiveError::NONE) {
        if (failure == ArchiveError::NONE) failure = finalizeError;
        status = RunStatus::StorageError;
    }
    if (status == RunStatus::StorageError && failure == ArchiveError::NONE) {
        failure = cause != ArchiveError::NONE ? cause : ArchiveError::WRITE_FAILED;
    }

    recording_ = false;
    runManager_.stopRun();
    recorder_.reset(0);
    clearQueue();
    overflowed_ = false;
    storeFailure_ = false;
    storeFailureError_ = ArchiveError::NONE;
    hasDirty_ = false;

    if (status == RunStatus::StorageError) {
        state_ = LoggingState::Error;
        loggingError_ = failure != ArchiveError::NONE
                            ? failure
                            : (cause != ArchiveError::NONE ? cause
                                                           : ArchiveError::WRITE_FAILED);
    } else {
        state_ = LoggingState::Idle;
        loggingError_ = ArchiveError::NONE;
    }

    refreshPublishedStatus(nowMs, true);
    return failure;
}

ArchiveError RunLogService::markerBoundary(uint32_t runId, uint32_t nowMs,
                                           uint32_t& boundary) {
    boundary = 0;
    if (runManager_.active() && runId == runManager_.runId()) {
        boundary = runManager_.elapsedMs(nowMs);
        return ArchiveError::NONE;
    }

    RunMeta meta{};
    const ArchiveError metaError = store_.getMeta(runId, meta);
    if (metaError != ArchiveError::NONE) return metaError;
    if (meta.hasEndElapsedMs) {
        boundary = meta.endElapsedMs;
        return ArchiveError::NONE;
    }

    // Interrupted run: the last validated sample timestamp is the boundary.
    RunSnapshot snapshot{};
    const ArchiveError snapshotError = store_.captureSnapshot(runId, snapshot);
    if (snapshotError != ArchiveError::NONE) return snapshotError;
    if (snapshot.throughLogSeq == 0) return ArchiveError::NONE;

    LoggedSample last{};
    PageInfo page{};
    const ArchiveError readError = store_.readSamplesPage(
        runId, snapshot.throughLogSeq - 1U, snapshot.throughLogSeq, 1, &last, 1, page);
    if (readError != ArchiveError::NONE) return readError;
    if (page.count > 0) boundary = last.ms;
    return ArchiveError::NONE;
}

void RunLogService::refreshPublishedStatus(uint32_t nowMs, bool forceStorage) {
    if (forceStorage || !statusInitialized_ ||
        static_cast<uint32_t>(nowMs - lastStorageRefreshMs_) >= 1000U) {
        storageCache_ = store_.storageStatus();
        lastStorageRefreshMs_ = nowMs;
        statusInitialized_ = true;
    }

    NetworkStatusSnapshot snapshot{};
    snapshot.identity = identity_;
    copyText(snapshot.firmware, sizeof(snapshot.firmware), FIRMWARE_VERSION);
    snapshot.protocolVersion = PROTOCOL_VERSION;
    snapshot.sampleHz = SAMPLE_HZ;
    snapshot.runId = runManager_.runId();
    snapshot.runActive = runManager_.active();
    snapshot.runStartMs = runManager_.originMs();
    snapshot.runArchive = true;
    snapshot.acquireIntervalMs = 1000U / SAMPLE_HZ;
    snapshot.logIntervalMs = RUN_LOG_INTERVAL_MS;
    snapshot.maxRunMs = RUN_MAX_DURATION_MS;
    snapshot.logging.state = state_;
    snapshot.logging.durableLogSeq = durableLogSeq_;
    snapshot.logging.latestAcqSeq = latestAcqSeq_;
    snapshot.logging.bufferedRecords =
        lastSelectedLogSeq_ > durableLogSeq_ ? lastSelectedLogSeq_ - durableLogSeq_ : 0;
    snapshot.logging.acquisitionGaps = acquisitionGaps_;
    snapshot.logging.droppedAcquired = droppedAcquired_;
    snapshot.logging.queueHighWater = queueHighWater_;
    snapshot.logging.error = loggingError_;
    snapshot.storage = storageCache_;

#if defined(ARDUINO_ARCH_ESP32)
    portENTER_CRITICAL(&statusMux_);
    published_ = snapshot;
    portEXIT_CRITICAL(&statusMux_);
#else
    published_ = snapshot;
#endif
}

#endif  // PYROVUE_BOARD_N16R8
