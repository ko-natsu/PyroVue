#include "ArchiveRoutes.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>

#include <math.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <memory>
#include <utility>

#include "ArchiveResponse.h"
#include "config.h"

#if defined(PYROVUE_RUN_ARCHIVE)
#include "RunArchiveStore.h"
#include "RunLogService.h"
#endif

#if defined(ARDUINO_ARCH_ESP32) && defined(BOARD_HAS_PSRAM)
#include <esp_heap_caps.h>
#endif

namespace {

constexpr uint8_t kMaxSlotCount = 4;
constexpr uint8_t kArchiveSlotCount = 4;
constexpr uint8_t kArchiveJobCapacity = 8;
constexpr size_t kArchiveSlotBytes = 32768;
constexpr size_t kMonitorSlotBytes = 1024;
constexpr size_t kCommandBodyBytes = 256;
constexpr size_t kMarkerBodyBytes = 4096;
constexpr size_t kMaxListLimit = 16;
constexpr size_t kMaxSampleLimit = 64;
constexpr size_t kMaxEventLimit = 8;
constexpr size_t kEventArrayOffset = 256;
constexpr size_t kEventSuffixReserve = 96;
constexpr size_t kFrameScratchBytes = 4104;
constexpr size_t kJsonArenaBytes = 16384;

enum class RouteKind : uint8_t {
    Invalid,
    Command,
    ListRuns,
    GetMeta,
    Snapshot,
    Samples,
    Events,
    MarkerCreate,
    MarkerUpdate,
    MarkerDelete,
    DeleteRun
};

enum class ResponseSlotState : uint8_t {
    Free,
    Assembling,
    Queued,
    Ready,
    Sending,
    Cancelled
};

struct RouteMatch {
    RouteKind kind = RouteKind::Invalid;
    uint32_t runId = 0;
    char markerId[81]{};
};

// ---------------------------------------------------------------------------
// Small bounded primitives
// ---------------------------------------------------------------------------

class SlotLock {
public:
    void lock() {
#if defined(ARDUINO_ARCH_ESP32)
        portENTER_CRITICAL(&mux_);
#else
        noInterrupts();
#endif
    }
    void unlock() {
#if defined(ARDUINO_ARCH_ESP32)
        portEXIT_CRITICAL(&mux_);
#else
        interrupts();
#endif
    }
private:
#if defined(ARDUINO_ARCH_ESP32)
    portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
#endif
};

class LockGuard {
public:
    explicit LockGuard(SlotLock& lock) : lock_(lock) { lock_.lock(); }
    ~LockGuard() { lock_.unlock(); }
    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;

private:
    SlotLock& lock_;
};

struct alignas(8) ArenaHeader {
    size_t size;
};

// Reusable bounded ArduinoJson allocator.  Used only from request callbacks,
// where exactly one document is parsed at a time.  When no arena is available
// (monitor targets) it falls back to the default allocator.
class BoundedArena : public Allocator {
public:
    void configure(uint8_t* base, size_t capacity) {
        base_ = base;
        capacity_ = capacity;
        used_ = 0;
    }

    void reset() { used_ = 0; }

    void* allocate(size_t size) override {
        if (base_ == nullptr) return malloc(size);
        size_t aligned = (size + 7u) & ~static_cast<size_t>(7u);
        size_t offset = (used_ + 7u) & ~static_cast<size_t>(7u);
        if (offset + sizeof(ArenaHeader) + aligned > capacity_) return nullptr;
        ArenaHeader* header = reinterpret_cast<ArenaHeader*>(base_ + offset);
        header->size = aligned;
        void* result = base_ + offset + sizeof(ArenaHeader);
        used_ = offset + sizeof(ArenaHeader) + aligned;
        return result;
    }

    void deallocate(void* ptr) override {
        if (base_ == nullptr) {
            free(ptr);
            return;
        }
        (void)ptr;  // Bump allocator; reset() reclaims everything.
    }

    void* reallocate(void* ptr, size_t newSize) override {
        if (ptr == nullptr) return allocate(newSize);
        if (base_ == nullptr) return realloc(ptr, newSize);
        ArenaHeader* header =
            reinterpret_cast<ArenaHeader*>(static_cast<uint8_t*>(ptr) -
                                           sizeof(ArenaHeader));
        const size_t oldSize = header->size;
        uint8_t* end = static_cast<uint8_t*>(ptr) + oldSize;
        const size_t aligned = (newSize + 7u) & ~static_cast<size_t>(7u);
        if (end == base_ + used_) {
            const size_t offset =
                static_cast<size_t>(static_cast<uint8_t*>(ptr) - base_);
            if (offset + aligned <= capacity_) {
                header->size = aligned;
                used_ = offset + aligned;
                return ptr;
            }
        }
        void* replacement = allocate(newSize);
        if (replacement == nullptr) return nullptr;
        memcpy(replacement, ptr, oldSize < newSize ? oldSize : newSize);
        return replacement;
    }

private:
    uint8_t* base_ = nullptr;
    size_t capacity_ = 0;
    size_t used_ = 0;
};

struct JsonOut {
    uint8_t* data;
    size_t capacity;
    size_t len;
    bool ok;

    JsonOut(uint8_t* buffer, size_t bufferCapacity)
        : data(buffer), capacity(bufferCapacity), len(0), ok(buffer != nullptr) {}

    bool raw(const char* text, size_t bytes) {
        if (!ok) return false;
        if (bytes > capacity - len) {
            ok = false;
            return false;
        }
        if (bytes != 0) memcpy(data + len, text, bytes);
        len += bytes;
        return true;
    }

    bool str(const char* text) {
        if (text == nullptr) return raw("null", 4);
        return raw(text, strlen(text));
    }

    bool ch(char value) { return raw(&value, 1); }

    bool u32(uint32_t value) {
        char buffer[16];
        int written = snprintf(buffer, sizeof(buffer), "%lu",
                               static_cast<unsigned long>(value));
        if (written < 0) {
            ok = false;
            return false;
        }
        return raw(buffer, static_cast<size_t>(written));
    }

    bool u64(uint64_t value) {
        char buffer[24];
        int written = snprintf(buffer, sizeof(buffer), "%llu",
                               static_cast<unsigned long long>(value));
        if (written < 0) {
            ok = false;
            return false;
        }
        return raw(buffer, static_cast<size_t>(written));
    }

    bool boolean(bool value) { return str(value ? "true" : "false"); }

    bool quoted(const char* text) {
        if (text == nullptr) return raw("null", 4);
        if (!ch('"')) return false;
        for (const uint8_t* p = reinterpret_cast<const uint8_t*>(text);
             *p != 0; ++p) {
            const uint8_t c = *p;
            switch (c) {
                case '"': if (!raw("\\\"", 2)) return false; break;
                case '\\': if (!raw("\\\\", 2)) return false; break;
                case '\b': if (!raw("\\b", 2)) return false; break;
                case '\f': if (!raw("\\f", 2)) return false; break;
                case '\n': if (!raw("\\n", 2)) return false; break;
                case '\r': if (!raw("\\r", 2)) return false; break;
                case '\t': if (!raw("\\t", 2)) return false; break;
                default:
                    if (c < 0x20) {
                        char escape[7];
                        snprintf(escape, sizeof(escape), "\\u%04x", static_cast<unsigned>(c));
                        if (!raw(escape, 6)) return false;
                    } else if (!ch(static_cast<char>(c))) {
                        return false;
                    }
                    break;
            }
        }
        return ch('"');
    }

    bool nullableCode(const char* code) {
        return code == nullptr ? raw("null", 4) : quoted(code);
    }
};

// ---------------------------------------------------------------------------
// Basic validation helpers
// ---------------------------------------------------------------------------

bool parseUint32(const char* text, size_t length, uint32_t& out) {
    if (text == nullptr || length == 0 || length > 10) return false;
    uint64_t value = 0;
    for (size_t i = 0; i < length; ++i) {
        const char c = text[i];
        if (c < '0' || c > '9') return false;
        value = value * 10u + static_cast<uint32_t>(c - '0');
        if (value > 0xFFFFFFFFull) return false;
    }
    out = static_cast<uint32_t>(value);
    return true;
}

bool parseUint32(const String& text, uint32_t& out) {
    return parseUint32(text.c_str(), text.length(), out);
}

bool validToken(const char* text, size_t length, size_t maxLength) {
    if (text == nullptr || length == 0 || length > maxLength) return false;
    for (size_t i = 0; i < length; ++i) {
        const char c = text[i];
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

bool validBootId(const char* text, size_t length) {
    if (text == nullptr || length != 16) return false;
    for (size_t i = 0; i < length; ++i) {
        const char c = text[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

// Decodes one UTF-8 scalar.  Rejects overlong encodings, surrogates and
// values beyond U+10FFFF.  Embedded NUL is reported to the caller through cp.
bool decodeUtf8(const uint8_t* text, size_t length, size_t& used,
                uint32_t& cp) {
    if (length == 0) return false;
    const uint8_t b0 = text[0];
    if (b0 < 0x80) {
        used = 1;
        cp = b0;
        return true;
    }
    if ((b0 & 0xE0) == 0xC0) {
        if (length < 2) return false;
        const uint8_t b1 = text[1];
        if ((b1 & 0xC0) != 0x80) return false;
        cp = (static_cast<uint32_t>(b0 & 0x1F) << 6) |
             static_cast<uint32_t>(b1 & 0x3F);
        if (cp < 0x80) return false;
        used = 2;
        return true;
    }
    if ((b0 & 0xF0) == 0xE0) {
        if (length < 3) return false;
        const uint8_t b1 = text[1];
        const uint8_t b2 = text[2];
        if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80) return false;
        cp = (static_cast<uint32_t>(b0 & 0x0F) << 12) |
             (static_cast<uint32_t>(b1 & 0x3F) << 6) |
             static_cast<uint32_t>(b2 & 0x3F);
        if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        used = 3;
        return true;
    }
    if ((b0 & 0xF8) == 0xF0) {
        if (length < 4) return false;
        const uint8_t b1 = text[1];
        const uint8_t b2 = text[2];
        const uint8_t b3 = text[3];
        if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80 ||
            (b3 & 0xC0) != 0x80) {
            return false;
        }
        cp = (static_cast<uint32_t>(b0 & 0x07) << 18) |
             (static_cast<uint32_t>(b1 & 0x3F) << 12) |
             (static_cast<uint32_t>(b2 & 0x3F) << 6) |
             static_cast<uint32_t>(b3 & 0x3F);
        if (cp < 0x10000 || cp > 0x10FFFF) return false;
        used = 4;
        return true;
    }
    return false;
}

bool isEcmascriptTrim(uint32_t cp) {
    return (cp >= 0x0009 && cp <= 0x000D) || cp == 0x0020 || cp == 0x00A0 ||
           cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 ||
           cp == 0x2029 || cp == 0x202F || cp == 0x205F || cp == 0x3000 ||
           cp == 0xFEFF;
}

bool validUtf8NoNul(const char* text, size_t length) {
    if (text == nullptr) return length == 0;
    size_t i = 0;
    while (i < length) {
        size_t used = 0;
        uint32_t cp = 0;
        if (!decodeUtf8(reinterpret_cast<const uint8_t*>(text) + i, length - i,
                        used, cp)) {
            return false;
        }
        if (cp == 0) return false;
        i += used;
    }
    return true;
}

// Trims ECMAScript whitespace at both ends, then enforces the note scalar and
// byte budgets.  The note arrives as JSON text, so embedded NUL and malformed
// UTF-8 are rejected even when they only appear in trimmed-off whitespace.
ArchiveError prepareNote(const char* text, size_t length, char* out,
                         size_t outCapacity) {
    if (text == nullptr || out == nullptr || outCapacity < 2) {
        return ArchiveError::INVALID_REQUEST;
    }
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(text);
    size_t first = 0;
    size_t last = 0;
    bool have = false;
    size_t i = 0;
    while (i < length) {
        size_t used = 0;
        uint32_t cp = 0;
        if (!decodeUtf8(bytes + i, length - i, used, cp) || cp == 0) {
            return ArchiveError::INVALID_REQUEST;
        }
        if (!isEcmascriptTrim(cp)) {
            if (!have) {
                first = i;
                have = true;
            }
            last = i + used;
        }
        i += used;
    }
    const size_t trimmedBytes = have ? last - first : 0;
    if (trimmedBytes > runlog::kMaxNoteBytes ||
        trimmedBytes + 1 > outCapacity) {
        return ArchiveError::INVALID_REQUEST;
    }
    size_t scalars = 0;
    i = 0;
    while (i < trimmedBytes) {
        size_t used = 0;
        uint32_t cp = 0;
        if (!decodeUtf8(bytes + first + i, trimmedBytes - i, used, cp) ||
            cp == 0) {
            return ArchiveError::INVALID_REQUEST;
        }
        ++scalars;
        if (scalars > runlog::kMaxNoteScalars) {
            return ArchiveError::INVALID_REQUEST;
        }
        i += used;
    }
    if (trimmedBytes != 0) memcpy(out, bytes + first, trimmedBytes);
    out[trimmedBytes] = '\0';
    return ArchiveError::NONE;
}

// ---------------------------------------------------------------------------
// Error mapping and serialization
// ---------------------------------------------------------------------------

const char* runStatusName(RunStatus status) {
    switch (status) {
        case RunStatus::Active: return "active";
        case RunStatus::Complete: return "complete";
        case RunStatus::Interrupted: return "interrupted";
        case RunStatus::DurationLimit: return "duration_limit";
        case RunStatus::StorageError: return "storage_error";
        case RunStatus::Corrupt: return "corrupt";
    }
    return "interrupted";
}

const char* wallTimeSourceName(WallTimeSource source) {
    return source == WallTimeSource::ClientEstimated ? "client-estimated"
                                                     : "unavailable";
}

int httpStatusFor(ArchiveError error) {
    switch (error) {
        case ArchiveError::NONE: return 200;
        case ArchiveError::INVALID_REQUEST: return 400;
        case ArchiveError::RUN_NOT_FOUND:
        case ArchiveError::MARKER_NOT_FOUND:
        case ArchiveError::ARCHIVE_UNAVAILABLE: return 404;
        case ArchiveError::RUN_ACTIVE:
        case ArchiveError::RUN_ID_CONFLICT:
        case ArchiveError::REVISION_CONFLICT:
        case ArchiveError::OPERATION_ID_CONFLICT:
        case ArchiveError::EVENT_TIME_OUT_OF_RANGE:
        case ArchiveError::CORRUPT_RUN:
        case ArchiveError::STALE_COMMAND:
        case ArchiveError::DEVICE_ID_MISMATCH:
        case ArchiveError::RUN_ID_EXHAUSTED: return 409;
        case ArchiveError::BODY_TOO_LARGE: return 413;
        case ArchiveError::INVALID_CURSOR: return 416;
        case ArchiveError::BUSY:
        case ArchiveError::STORAGE_UNAVAILABLE:
        case ArchiveError::WRITE_FAILED: return 503;
        case ArchiveError::INSUFFICIENT_STORAGE:
        case ArchiveError::EVENT_BUDGET_EXHAUSTED: return 507;
        case ArchiveError::RUN_ID_SAVE_FAILED: return 500;
    }
    return 400;
}

const char* errorMessage(ArchiveError error) {
    switch (error) {
        case ArchiveError::NONE: return "OK";
        case ArchiveError::INVALID_REQUEST: return "Invalid request";
        case ArchiveError::RUN_NOT_FOUND: return "Run not found";
        case ArchiveError::MARKER_NOT_FOUND: return "Marker not found";
        case ArchiveError::ARCHIVE_UNAVAILABLE: return "Archive unavailable";
        case ArchiveError::RUN_ACTIVE: return "Run is active";
        case ArchiveError::RUN_ID_CONFLICT: return "Run id conflict";
        case ArchiveError::REVISION_CONFLICT: return "Revision conflict";
        case ArchiveError::OPERATION_ID_CONFLICT:
            return "Operation id conflict";
        case ArchiveError::EVENT_TIME_OUT_OF_RANGE:
            return "Event time out of range";
        case ArchiveError::CORRUPT_RUN: return "Run is corrupt";
        case ArchiveError::STALE_COMMAND: return "Stale command";
        case ArchiveError::DEVICE_ID_MISMATCH: return "Device id mismatch";
        case ArchiveError::BODY_TOO_LARGE: return "Body too large";
        case ArchiveError::INVALID_CURSOR: return "Invalid cursor";
        case ArchiveError::BUSY: return "Busy";
        case ArchiveError::STORAGE_UNAVAILABLE: return "Storage unavailable";
        case ArchiveError::WRITE_FAILED: return "Write failed";
        case ArchiveError::INSUFFICIENT_STORAGE: return "Insufficient storage";
        case ArchiveError::EVENT_BUDGET_EXHAUSTED:
            return "Event budget exhausted";
        case ArchiveError::RUN_ID_SAVE_FAILED: return "Run id save failed";
        case ArchiveError::RUN_ID_EXHAUSTED: return "Run id exhausted";
    }
    return "Invalid request";
}

bool writeErrorJson(JsonOut& out, ArchiveError error, const char* requestId) {
    if (error == ArchiveError::NONE) error = ArchiveError::INVALID_REQUEST;
    const char* code = archiveErrorName(error);
    if (code == nullptr) code = "INVALID_REQUEST";
    out.str("{\"type\":\"error\",\"protocol\":");
    out.u32(static_cast<uint32_t>(PROTOCOL_VERSION));
    out.str(",\"code\":\"");
    out.str(code);
    out.str("\",\"message\":\"");
    out.str(errorMessage(error));
    out.ch('"');
    if (requestId != nullptr && requestId[0] != '\0') {
        out.str(",\"requestId\":");
        out.quoted(requestId);
    }
    out.ch('}');
    return out.ok;
}

bool writeRunMetaJson(JsonOut& out, const RunMeta& meta) {
    if (meta.corrupt) {
        out.str("{\"schemaVersion\":");
        out.u32(runlog::kSchemaVersion);
        out.str(",\"deviceId\":");
        out.quoted(meta.deviceId);
        out.str(",\"runId\":");
        out.u32(meta.runId);
        out.str(",\"status\":\"corrupt\",\"path\":");
        out.quoted(meta.path);
        out.str(",\"sizeBytes\":");
        out.u32(meta.sizeBytes);
        out.str(",\"recoveredTailBytes\":");
        out.u32(meta.recoveredTailBytes);
        out.str(",\"rawComplete\":false,\"errorCode\":\"CORRUPT_RUN\"}");
        return out.ok;
    }

    out.str("{\"schemaVersion\":");
    out.u32(runlog::kSchemaVersion);
    out.str(",\"deviceId\":");
    out.quoted(meta.deviceId);
    out.str(",\"bootId\":");
    out.quoted(meta.bootId);
    out.str(",\"runId\":");
    out.u32(meta.runId);
    out.str(",\"firmware\":");
    out.quoted(meta.firmware);
    out.str(",\"protocolVersion\":");
    out.u32(meta.protocolVersion);
    out.str(",\"status\":\"");
    out.str(runStatusName(meta.status));
    out.str("\",\"rawComplete\":");
    out.boolean(meta.rawComplete);
    out.str(",\"acquireIntervalMs\":");
    out.u32(meta.acquireIntervalMs);
    out.str(",\"logIntervalMs\":");
    out.u32(meta.logIntervalMs);
    out.str(",\"graphBucketMs\":");
    out.u32(meta.graphBucketMs);
    out.str(",\"maxRunMs\":");
    out.u32(meta.maxRunMs);
    out.str(",\"startUptimeMs\":");
    out.u32(meta.startUptimeMs);
    out.str(",\"endElapsedMs\":");
    if (meta.hasEndElapsedMs) {
        out.u32(meta.endElapsedMs);
    } else {
        out.str("null");
    }
    out.str(",\"wallStartUtcMs\":");
    if (meta.hasWallStartUtcMs) {
        out.u64(meta.wallStartUtcMs);
    } else {
        out.str("null");
    }
    out.str(",\"wallTimeSource\":\"");
    out.str(wallTimeSourceName(meta.wallTimeSource));
    out.str("\",\"sampleCount\":");
    out.u32(meta.sampleCount);
    out.str(",\"durableLogSeq\":");
    out.u32(meta.durableLogSeq);
    out.str(",\"latestAcqSeq\":");
    out.u32(meta.latestAcqSeq);
    out.str(",\"eventRevision\":");
    out.u32(meta.eventRevision);
    out.str(",\"annotationCount\":");
    out.u32(meta.annotationCount);
    out.str(",\"acquisitionGaps\":");
    out.u32(meta.acquisitionGaps);
    out.str(",\"droppedAcquired\":");
    out.u32(meta.droppedAcquired);
    out.str(",\"sizeBytes\":");
    out.u32(meta.sizeBytes);
    out.str(",\"recoveredTailBytes\":");
    out.u32(meta.recoveredTailBytes);
    out.str(",\"errorCode\":");
    out.nullableCode(archiveErrorName(meta.error));
    out.ch('}');
    return out.ok;
}

bool writeCommandResultJson(JsonOut& out, const CommandResult& result,
                            const char* fallbackRequestId) {
    const char* requestId =
        result.requestId[0] != '\0' ? result.requestId : fallbackRequestId;
    out.str("{\"type\":\"command.result\",\"protocol\":");
    out.u32(static_cast<uint32_t>(PROTOCOL_VERSION));
    out.str(",\"requestId\":");
    out.quoted(requestId);
    out.str(",\"ok\":true,\"runId\":");
    out.u32(result.runId);
    out.str(",\"runActive\":");
    out.boolean(result.runActive);
    out.str(",\"targetRunId\":");
    out.u32(result.targetRunId);
    out.str(",\"durable\":");
    out.boolean(result.durable);
    out.ch('}');
    return out.ok;
}

bool writeMutationResultJson(JsonOut& out, const MutationResult& result) {
    out.str("{\"schemaVersion\":");
    out.u32(runlog::kSchemaVersion);
    out.str(",\"runId\":");
    out.u32(result.runId);
    out.str(",\"operationId\":");
    out.quoted(result.operationId);
    out.str(",\"id\":");
    out.quoted(result.id);
    out.str(",\"revision\":");
    out.u32(result.revision);
    out.str(",\"persisted\":");
    out.boolean(result.persisted);
    out.str(",\"deleted\":");
    out.boolean(result.deleted);
    out.ch('}');
    return out.ok;
}

bool writeDeleteResultJson(JsonOut& out, uint32_t runId, bool deleted) {
    out.str("{\"schemaVersion\":");
    out.u32(runlog::kSchemaVersion);
    out.str(",\"runId\":");
    out.u32(runId);
    out.str(",\"deleted\":");
    out.boolean(deleted);
    out.ch('}');
    return out.ok;
}

bool parseRoute(const String& url, WebRequestMethod method, RouteMatch& out) {
    out = RouteMatch{};

    if (url == "/api/cmd") {
        if (method != AsyncWebRequestMethod::HTTP_POST) return false;
        out.kind = RouteKind::Command;
        return true;
    }

    if (url == "/api/runs") {
        if (method != AsyncWebRequestMethod::HTTP_GET) return false;
        out.kind = RouteKind::ListRuns;
        return true;
    }

    if (!url.startsWith("/api/runs/")) return false;
    const char* path = url.c_str() + 10;
    const char* slash = strchr(path, '/');
    const size_t idLength = slash != nullptr ? static_cast<size_t>(slash - path)
                                             : strlen(path);
    uint32_t runId = 0;
    if (!parseUint32(path, idLength, runId) || runId == 0) return false;
    out.runId = runId;

    if (slash == nullptr) {
        if (method == AsyncWebRequestMethod::HTTP_GET) {
            out.kind = RouteKind::GetMeta;
            return true;
        }
        if (method == AsyncWebRequestMethod::HTTP_DELETE) {
            out.kind = RouteKind::DeleteRun;
            return true;
        }
        return false;
    }

    const char* sub = slash + 1;
    if (strcmp(sub, "snapshot") == 0) {
        if (method != AsyncWebRequestMethod::HTTP_GET) return false;
        out.kind = RouteKind::Snapshot;
        return true;
    }
    if (strcmp(sub, "samples") == 0) {
        if (method != AsyncWebRequestMethod::HTTP_GET) return false;
        out.kind = RouteKind::Samples;
        return true;
    }
    if (strcmp(sub, "events") == 0) {
        if (method != AsyncWebRequestMethod::HTTP_GET) return false;
        out.kind = RouteKind::Events;
        return true;
    }
    if (strcmp(sub, "markers") == 0) {
        if (method != AsyncWebRequestMethod::HTTP_POST) return false;
        out.kind = RouteKind::MarkerCreate;
        return true;
    }
    if (strncmp(sub, "markers/", 8) == 0) {
        const char* markerId = sub + 8;
        const size_t markerLength = strlen(markerId);
        if (!validToken(markerId, markerLength, 80)) return false;
        if (method == AsyncWebRequestMethod::HTTP_PUT) {
            out.kind = RouteKind::MarkerUpdate;
        } else if (method == AsyncWebRequestMethod::HTTP_DELETE) {
            out.kind = RouteKind::MarkerDelete;
        } else {
            return false;
        }
        memcpy(out.markerId, markerId, markerLength);
        out.markerId[markerLength] = '\0';
        return true;
    }
    return false;
}

bool requiresBody(RouteKind kind) {
    return kind == RouteKind::Command || kind == RouteKind::MarkerCreate ||
           kind == RouteKind::MarkerUpdate || kind == RouteKind::MarkerDelete;
}

size_t bodyLimitFor(RouteKind kind) {
    if (kind == RouteKind::Command) return kCommandBodyBytes;
    if (kind == RouteKind::MarkerCreate || kind == RouteKind::MarkerUpdate ||
        kind == RouteKind::MarkerDelete) {
        return kMarkerBodyBytes;
    }
    return 0;
}

bool isArchiveRoute(RouteKind kind) {
    return kind != RouteKind::Invalid && kind != RouteKind::Command;
}

// ---------------------------------------------------------------------------
// JSON request parsing
// ---------------------------------------------------------------------------

bool jsonString(JsonVariant value, const char*& text, size_t& length) {
    JsonString string = value.as<JsonString>();
    if (string.isNull()) return false;
    text = string.c_str();
    length = string.size();
    return text != nullptr;
}

ArchiveError parseCommandBody(const uint8_t* body, size_t length,
                              BoundedArena& arena, CommandRequest& out) {
    out = CommandRequest{};
    arena.reset();
    JsonDocument document(&arena);
    DeserializationError parseError =
        deserializeJson(document, body, length);
    if (parseError || document.overflowed() || !document.is<JsonObject>()) {
        return ArchiveError::INVALID_REQUEST;
    }

    const char* type = nullptr;
    size_t typeLength = 0;
    if (!jsonString(document["type"], type, typeLength)) {
        return ArchiveError::INVALID_REQUEST;
    }

    const char* requestId = nullptr;
    size_t requestIdLength = 0;
    if (!jsonString(document["requestId"], requestId, requestIdLength) ||
        !validToken(requestId, requestIdLength, 80)) {
        return ArchiveError::INVALID_REQUEST;
    }
    memcpy(out.requestId, requestId, requestIdLength);
    out.requestId[requestIdLength] = '\0';

    const char* bootId = nullptr;
    size_t bootIdLength = 0;
    if (!jsonString(document["bootId"], bootId, bootIdLength) ||
        !validBootId(bootId, bootIdLength)) {
        return ArchiveError::INVALID_REQUEST;
    }
    memcpy(out.bootId, bootId, bootIdLength);
    out.bootId[bootIdLength] = '\0';

    if (typeLength == 9 && memcmp(type, "run.start", 9) == 0) {
        out.command = CommandKind::Start;
        JsonVariant afterRunId = document["afterRunId"];
        if (!afterRunId.is<uint32_t>()) return ArchiveError::INVALID_REQUEST;
        out.afterRunId = afterRunId.as<uint32_t>();
        JsonVariant wallStart = document["wallStartUtcMs"];
        if (!wallStart.isNull()) {
            if (!wallStart.is<uint64_t>()) {
                return ArchiveError::INVALID_REQUEST;
            }
            const uint64_t wallValue = wallStart.as<uint64_t>();
            if (wallValue > 9007199254740991ull) {
                return ArchiveError::INVALID_REQUEST;  // JS safe integer
            }
            out.hasWallStartUtcMs = true;
            out.wallStartUtcMs = wallValue;
        }
        return ArchiveError::NONE;
    }

    if (typeLength == 8 && memcmp(type, "run.stop", 8) == 0) {
        out.command = CommandKind::Stop;
        JsonVariant runId = document["runId"];
        if (!runId.is<uint32_t>()) return ArchiveError::INVALID_REQUEST;
        const uint32_t value = runId.as<uint32_t>();
        if (value == 0) return ArchiveError::INVALID_REQUEST;
        out.targetRunId = value;
        return ArchiveError::NONE;
    }

    if (typeLength == 6 && memcmp(type, "preset", 6) == 0) {
        out.command = CommandKind::Preset;
        const char* value = nullptr;
        size_t valueLength = 0;
        if (!jsonString(document["value"], value, valueLength)) {
            return ArchiveError::INVALID_REQUEST;
        }
        if (valueLength == 0 || valueLength > 32 ||
            !validUtf8NoNul(value, valueLength)) {
            return ArchiveError::INVALID_REQUEST;
        }
        memcpy(out.value, value, valueLength);
        out.value[valueLength] = '\0';
        return ArchiveError::NONE;
    }

    return ArchiveError::INVALID_REQUEST;
}

ArchiveError parseMarkerBody(const uint8_t* body, size_t length,
                             BoundedArena& arena, MarkerMutationKind kind,
                             const char* pathMarkerId,
                             MarkerMutation& out) {
    out = MarkerMutation{};
    out.kind = kind;
    arena.reset();
    JsonDocument document(&arena);
    DeserializationError parseError =
        deserializeJson(document, body, length);
    if (parseError || document.overflowed() || !document.is<JsonObject>()) {
        return ArchiveError::INVALID_REQUEST;
    }

    const char* operationId = nullptr;
    size_t operationIdLength = 0;
    if (!jsonString(document["operationId"], operationId,
                    operationIdLength) ||
        !validToken(operationId, operationIdLength, 80)) {
        return ArchiveError::INVALID_REQUEST;
    }
    memcpy(out.operationId, operationId, operationIdLength);
    out.operationId[operationIdLength] = '\0';

    JsonVariant baseRevision = document["baseRevision"];
    if (kind == MarkerMutationKind::Create) {
        out.baseRevision = 0;
        if (!baseRevision.isNull()) {
            if (!baseRevision.is<uint32_t>()) {
                return ArchiveError::INVALID_REQUEST;
            }
            out.baseRevision = baseRevision.as<uint32_t>();
        }
    } else {
        if (!baseRevision.is<uint32_t>()) {
            return ArchiveError::INVALID_REQUEST;
        }
        out.baseRevision = baseRevision.as<uint32_t>();
    }

    if (kind == MarkerMutationKind::Create) {
        const char* markerId = nullptr;
        size_t markerLength = 0;
        if (!jsonString(document["id"], markerId, markerLength) ||
            !validToken(markerId, markerLength, 80)) {
            return ArchiveError::INVALID_REQUEST;
        }
        memcpy(out.id, markerId, markerLength);
        out.id[markerLength] = '\0';
    } else {
        const size_t pathLength =
            pathMarkerId != nullptr ? strlen(pathMarkerId) : 0;
        if (!validToken(pathMarkerId, pathLength, 80)) {
            return ArchiveError::INVALID_REQUEST;
        }
        JsonVariant bodyId = document["id"];
        if (!bodyId.isNull()) {
            const char* markerId = nullptr;
            size_t markerLength = 0;
            if (!jsonString(bodyId, markerId, markerLength) ||
                markerLength != pathLength ||
                memcmp(markerId, pathMarkerId, pathLength) != 0) {
                return ArchiveError::INVALID_REQUEST;
            }
        }
        memcpy(out.id, pathMarkerId, pathLength);
        out.id[pathLength] = '\0';
    }

    if (kind == MarkerMutationKind::Delete) {
        out.timeBasis = TimeBasis::Untimed;
        out.hasMs = false;
        return ArchiveError::NONE;
    }

    const char* note = nullptr;
    size_t noteLength = 0;
    if (!jsonString(document["note"], note, noteLength)) {
        return ArchiveError::INVALID_REQUEST;
    }
    const ArchiveError noteError =
        prepareNote(note, noteLength, out.note, sizeof(out.note));
    if (noteError != ArchiveError::NONE) return noteError;

    const char* basis = nullptr;
    size_t basisLength = 0;
    if (!jsonString(document["timeBasis"], basis, basisLength)) {
        return ArchiveError::INVALID_REQUEST;
    }
    if (basisLength == 9 && memcmp(basis, "estimated", 9) == 0) {
        out.timeBasis = TimeBasis::Estimated;
    } else if (basisLength == 6 && memcmp(basis, "manual", 6) == 0) {
        out.timeBasis = TimeBasis::Manual;
    } else if (basisLength == 7 && memcmp(basis, "untimed", 7) == 0) {
        out.timeBasis = TimeBasis::Untimed;
    } else {
        return ArchiveError::INVALID_REQUEST;
    }

    JsonVariant ms = document["ms"];
    out.hasMs = false;
    if (!ms.isNull()) {
        if (!ms.is<uint32_t>()) return ArchiveError::INVALID_REQUEST;
        out.hasMs = true;
        out.ms = ms.as<uint32_t>();
    }
    if ((out.timeBasis == TimeBasis::Untimed) == out.hasMs) {
        return ArchiveError::INVALID_REQUEST;
    }
    return ArchiveError::NONE;
}

// ---------------------------------------------------------------------------
// Immediate (non-slot) response
// ---------------------------------------------------------------------------

void sendImmediateError(AsyncWebServerRequest* request, int status,
                        ArchiveError error, const char* requestId) {
    if (!request || request->getResponse()) return;
    char buffer[512];
    JsonOut out(reinterpret_cast<uint8_t*>(buffer), sizeof(buffer) - 1);
    if (!writeErrorJson(out, error, requestId) || !out.ok) return;
    buffer[out.len] = '\0';
    AsyncWebServerResponse* response =
        request->beginResponse(status, "application/json", buffer);
    if (response == nullptr) return;
    response->addHeader("Cache-Control", "no-store", true);
    request->send(response);
}

}  // namespace

// ---------------------------------------------------------------------------
// Transport state
// ---------------------------------------------------------------------------

namespace {

struct ResponseSlot {
    uint8_t* buffer = nullptr;
    size_t capacity = 0;
    std::atomic<ResponseSlotState> state{ResponseSlotState::Free};
    std::atomic<uint32_t> generation{0};
    std::atomic<bool> cancelled{false};
    std::weak_ptr<AsyncWebServerRequest> request;
    size_t bodyLength = 0;
    size_t bodyLimit = 0;
    bool invalid = false;
    bool oversize = false;
    size_t responseLength = 0;
    int statusCode = 200;
};

enum class WorkerJobKind : uint8_t {
    Command,
    ListRuns,
    GetMeta,
    Snapshot,
    Samples,
    Events,
    MutateMarker,
    DeleteRun
};

struct WorkerJob {
    WorkerJobKind kind = WorkerJobKind::Command;
    uint8_t slotIndex = 0;
    uint32_t slotGeneration = 0;
    std::weak_ptr<AsyncWebServerRequest> request;
    std::atomic<bool> cancelled{false};
    char expectedDeviceId[13]{};
    CommandRequest command{};
    uint32_t runId = 0;
    uint32_t beforeRunId = 0;
    uint32_t after = 0;
    uint32_t through = 0;
    uint8_t limit = 0;
    uint32_t listIds[kMaxListLimit]{};
    uint8_t listCount = 0;
    uint8_t listIndex = 0;
    uint8_t listPhase = 0;
    uint32_t listNextBefore = 0;
#if defined(PYROVUE_RUN_ARCHIVE)
    MarkerMutation marker{};
#endif
};

void* psramAllocate(size_t bytes) {
#if defined(ARDUINO_ARCH_ESP32) && defined(BOARD_HAS_PSRAM)
    void* memory =
        heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return memory;
#else
    (void)bytes;
    return nullptr;
#endif
}

void psramRelease(void* memory) {
#if defined(ARDUINO_ARCH_ESP32) && defined(BOARD_HAS_PSRAM)
    if (memory != nullptr) heap_caps_free(memory);
#else
    (void)memory;
#endif
}

}  // namespace

struct ArchiveRoutes::Impl {
    explicit Impl(ArchiveRoutes* owner) : owner_(owner) {}

    ArchiveRoutes* owner_;
    DeviceIdentity identity_{};
    std::function<void()> stateChanged_;
    std::function<CommandResult(const CommandRequest&)>* commandHandler_ =
        nullptr;
    RunLogService* logger_ = nullptr;
    AsyncWebServer* server_ = nullptr;

    ResponseSlot slots_[kMaxSlotCount];
    WorkerJob* jobs_ = nullptr;
    bool jobsInPsram_ = false;
    bool slotsInPsram_ = false;
    uint8_t slotCount_ = 0;
    uint8_t jobCapacity_ = 0;
    uint8_t jobHead_ = 0;
    uint8_t jobTail_ = 0;
    uint8_t jobCount_ = 0;
    bool archiveReady_ = false;
    bool started_ = false;

    RunMeta* metaScratch_ = nullptr;
    LoggedSample* sampleScratch_ = nullptr;
    uint8_t* frameScratch_ = nullptr;
    uint8_t* arenaBuffer_ = nullptr;
    BoundedArena arena_;
    SlotLock lock_;

    void begin(AsyncWebServer& server,
               std::function<CommandResult(const CommandRequest&)>& handler,
               RunLogService* logger);
    void releaseResources();
    bool allocateArchiveResources();
    void allocateMonitorResources();
    void registerRoutes();

    void onRequest(AsyncWebServerRequest* request);
    void onBody(AsyncWebServerRequest* request, uint8_t* data, size_t len,
                size_t index, size_t total);
    void handleCommandRequest(AsyncWebServerRequest* request,
                              const RouteMatch& route);
    void handleMarkerRequest(AsyncWebServerRequest* request,
                             const RouteMatch& route);
    void handleBodylessRequest(AsyncWebServerRequest* request,
                               const RouteMatch& route);

    bool deviceHeaderOk(AsyncWebServerRequest* request) const;
    bool archiveAvailable() const;
    int findAssembly(AsyncWebServerRequest* request);
    bool reserveSlot(uint8_t& slotIndex, uint32_t& generation);
    void releaseSlot(uint8_t slotIndex, uint32_t generation);
    void releaseSlotLocked(uint8_t slotIndex, uint32_t generation);
    void installDisconnect(AsyncWebServerRequest* request, uint8_t slotIndex,
                           uint32_t generation);
    void onDisconnected(uint8_t slotIndex, uint32_t generation);

    bool enqueueCommand(uint8_t slotIndex, uint32_t generation,
                        const CommandRequest& command);
    bool enqueueRead(WorkerJobKind kind, uint8_t slotIndex, uint32_t generation,
                     uint32_t runId, uint32_t beforeRunId, uint32_t after,
                     uint32_t through, uint8_t limit);
#if defined(PYROVUE_RUN_ARCHIVE)
    bool enqueueMutation(uint8_t slotIndex, uint32_t generation,
                         uint32_t runId, const MarkerMutation& mutation);
#endif

    void poll(uint32_t nowMs);
    bool executeRead(WorkerJob& job, bool& sent);
    bool executeMutation(WorkerJob& job, uint32_t nowMs, bool& sent);
    bool executeDelete(WorkerJob& job);
    void finishJob(const WorkerJob& job, bool sent);

    bool markReady(const WorkerJob& job, int status, size_t length);
    bool sendSlot(const WorkerJob& job);
    bool writeErrorResponse(const WorkerJob& job, ArchiveError error,
                            const char* requestId);
    bool writeCommandResponse(const WorkerJob& job,
                              const CommandResult& result);
    bool writeListResponse(const WorkerJob& job, RunMeta* runs, uint32_t count,
                           uint32_t nextBeforeRunId);
    bool writeMetaResponse(const WorkerJob& job, const RunMeta& meta);
    bool writeSnapshotResponse(const WorkerJob& job, const RunSnapshot& snap);
    bool writeSamplesResponse(const WorkerJob& job, const LoggedSample* samples,
                              const PageInfo& page, uint32_t after);
    bool writeEventsResponse(const WorkerJob& job, const PageInfo& page,
                             uint32_t after);
    bool writeMutationResponse(const WorkerJob& job,
                               const MutationResult& result);
    bool writeDeleteResponse(const WorkerJob& job, bool deleted);
};

// ---------------------------------------------------------------------------
// Lifecycle and registration
// ---------------------------------------------------------------------------

void ArchiveRoutes::Impl::releaseResources() {
    if (jobs_ != nullptr) {
        if (jobsInPsram_) {
            for (uint8_t i = 0; i < jobCapacity_; ++i) jobs_[i].~WorkerJob();
            psramRelease(jobs_);
        } else {
            delete[] jobs_;
        }
    }
    jobs_ = nullptr;
    jobsInPsram_ = false;

    for (uint8_t i = 0; i < kMaxSlotCount; ++i) {
        if (slots_[i].buffer != nullptr) {
            if (slotsInPsram_) {
                psramRelease(slots_[i].buffer);
            } else {
                free(slots_[i].buffer);
            }
        }
        slots_[i].buffer = nullptr;
        slots_[i].capacity = 0;
        slots_[i].state.store(ResponseSlotState::Free);
        slots_[i].cancelled.store(false);
        slots_[i].bodyLength = 0;
        slots_[i].bodyLimit = 0;
        slots_[i].invalid = false;
        slots_[i].oversize = false;
        slots_[i].responseLength = 0;
        slots_[i].statusCode = 200;
    }
    slotsInPsram_ = false;

    if (metaScratch_ != nullptr) psramRelease(metaScratch_);
    if (sampleScratch_ != nullptr) psramRelease(sampleScratch_);
    if (frameScratch_ != nullptr) psramRelease(frameScratch_);
    if (arenaBuffer_ != nullptr) psramRelease(arenaBuffer_);
    metaScratch_ = nullptr;
    sampleScratch_ = nullptr;
    frameScratch_ = nullptr;
    arenaBuffer_ = nullptr;
    arena_.configure(nullptr, 0);

    slotCount_ = 0;
    jobCapacity_ = 0;
    jobHead_ = 0;
    jobTail_ = 0;
    jobCount_ = 0;
    archiveReady_ = false;
}

bool ArchiveRoutes::Impl::allocateArchiveResources() {
#if defined(PYROVUE_RUN_ARCHIVE)
    void* rawJobs = psramAllocate(sizeof(WorkerJob) * kArchiveJobCapacity);
    if (rawJobs == nullptr) return false;
    WorkerJob* jobs = static_cast<WorkerJob*>(rawJobs);
    for (uint8_t i = 0; i < kArchiveJobCapacity; ++i) {
        new (&jobs[i]) WorkerJob();
    }

    uint8_t* buffers[kArchiveSlotCount] = {};
    for (uint8_t i = 0; i < kArchiveSlotCount; ++i) {
        buffers[i] = static_cast<uint8_t*>(psramAllocate(kArchiveSlotBytes));
        if (buffers[i] == nullptr) {
            for (uint8_t j = 0; j < i; ++j) psramRelease(buffers[j]);
            for (uint8_t j = 0; j < kArchiveJobCapacity; ++j) {
                jobs[j].~WorkerJob();
            }
            psramRelease(rawJobs);
            return false;
        }
    }

    RunMeta* metas =
        static_cast<RunMeta*>(psramAllocate(sizeof(RunMeta) * kMaxListLimit));
    LoggedSample* samples = static_cast<LoggedSample*>(
        psramAllocate(sizeof(LoggedSample) * kMaxSampleLimit));
    uint8_t* frame = static_cast<uint8_t*>(psramAllocate(kFrameScratchBytes));
    uint8_t* arena = static_cast<uint8_t*>(psramAllocate(kJsonArenaBytes));
    if (metas == nullptr || samples == nullptr || frame == nullptr ||
        arena == nullptr) {
        psramRelease(metas);
        psramRelease(samples);
        psramRelease(frame);
        psramRelease(arena);
        for (uint8_t i = 0; i < kArchiveSlotCount; ++i) {
            psramRelease(buffers[i]);
        }
        for (uint8_t i = 0; i < kArchiveJobCapacity; ++i) {
            jobs[i].~WorkerJob();
        }
        psramRelease(rawJobs);
        return false;
    }

    jobs_ = jobs;
    jobsInPsram_ = true;
    for (uint8_t i = 0; i < kArchiveSlotCount; ++i) {
        slots_[i].buffer = buffers[i];
        slots_[i].capacity = kArchiveSlotBytes;
        slots_[i].state.store(ResponseSlotState::Free);
    }
    slotsInPsram_ = true;
    metaScratch_ = metas;
    sampleScratch_ = samples;
    frameScratch_ = frame;
    arenaBuffer_ = arena;
    arena_.configure(arenaBuffer_, kJsonArenaBytes);
    slotCount_ = kArchiveSlotCount;
    jobCapacity_ = kArchiveJobCapacity;
    archiveReady_ = true;
    return true;
#else
    return false;
#endif
}

void ArchiveRoutes::Impl::allocateMonitorResources() {
    jobs_ = new (std::nothrow) WorkerJob[1]();
    if (jobs_ == nullptr) {
        slotCount_ = 0;
        jobCapacity_ = 0;
        return;
    }
    jobsInPsram_ = false;
    jobCapacity_ = 1;
    slotCount_ = 1;
    slots_[0].buffer = static_cast<uint8_t*>(malloc(kMonitorSlotBytes));
    slots_[0].capacity = slots_[0].buffer != nullptr ? kMonitorSlotBytes : 0;
    slots_[0].state.store(ResponseSlotState::Free);
    if (slots_[0].buffer == nullptr) {
        slotCount_ = 0;
        jobCapacity_ = 0;
        delete[] jobs_;
        jobs_ = nullptr;
    }
    slotsInPsram_ = false;
    arena_.configure(nullptr, 0);
}

void ArchiveRoutes::Impl::registerRoutes() {
#if defined(PYROVUE_RUN_ARCHIVE)
    server_->on(
        AsyncURIMatcher::exact("/api/cmd"),
        AsyncWebRequestMethod::HTTP_POST,
        [this](AsyncWebServerRequest* request) { onRequest(request); },
        nullptr,
        [this](AsyncWebServerRequest* request, uint8_t* data, size_t length,
               size_t index, size_t total) {
            onBody(request, data, length, index, total);
        });
    server_->on(
        AsyncURIMatcher::prefix("/api/runs"),
        AsyncWebRequestMethod::HTTP_GET | AsyncWebRequestMethod::HTTP_POST |
            AsyncWebRequestMethod::HTTP_PUT |
            AsyncWebRequestMethod::HTTP_DELETE,
        [this](AsyncWebServerRequest* request) { onRequest(request); },
        nullptr,
        [this](AsyncWebServerRequest* request, uint8_t* data, size_t length,
               size_t index, size_t total) {
            onBody(request, data, length, index, total);
        });
#else
    server_->on(
        AsyncURIMatcher::exact("/api/cmd"),
        AsyncWebRequestMethod::HTTP_POST,
        [this](AsyncWebServerRequest* request) { onRequest(request); },
        nullptr,
        [this](AsyncWebServerRequest* request, uint8_t* data, size_t length,
               size_t index, size_t total) {
            onBody(request, data, length, index, total);
        });
    server_->on(
        AsyncURIMatcher::prefix("/api/runs"),
        AsyncWebRequestMethod::HTTP_ALL,
        [](AsyncWebServerRequest* request) {
            sendImmediateError(request, 404,
                               ArchiveError::ARCHIVE_UNAVAILABLE, nullptr);
        });
#endif
}

void ArchiveRoutes::Impl::begin(
    AsyncWebServer& server,
    std::function<CommandResult(const CommandRequest&)>& handler,
    RunLogService* logger) {
    if (started_) return;
    started_ = true;
    server_ = &server;
    commandHandler_ = &handler;
    logger_ = logger;

    releaseResources();

#if defined(PYROVUE_RUN_ARCHIVE)
    if (logger_ != nullptr && allocateArchiveResources()) {
        // Archive profile is ready.
    } else {
        allocateMonitorResources();
    }
#else
    allocateMonitorResources();
#endif

    registerRoutes();
}

void ArchiveRoutes::Impl::onDisconnected(uint8_t slotIndex,
                                         uint32_t generation) {
    LockGuard guard(lock_);
    ResponseSlot& slot = slots_[slotIndex];
    if (slot.generation.load() != generation) return;
    if (slot.state.load() == ResponseSlotState::Assembling) {
        slot.state.store(ResponseSlotState::Free);
        slot.bodyLength = 0;
        slot.invalid = false;
        slot.oversize = false;
        return;
    }
    if (slot.state.load() == ResponseSlotState::Queued ||
        slot.state.load() == ResponseSlotState::Ready) {
        slot.state.store(ResponseSlotState::Cancelled);
        slot.cancelled.store(true);
        for (uint8_t i = 0; i < jobCapacity_; ++i) {
            WorkerJob& job = jobs_[i];
            if (job.slotIndex == slotIndex &&
                job.slotGeneration == generation) {
                job.cancelled.store(true);
            }
        }
    }
}

void ArchiveRoutes::Impl::installDisconnect(AsyncWebServerRequest* request,
                                            uint8_t slotIndex,
                                            uint32_t generation) {
    request->onDisconnect([this, slotIndex, generation]() {
        onDisconnected(slotIndex, generation);
    });
}

// ---------------------------------------------------------------------------
// Request callbacks
// ---------------------------------------------------------------------------

bool ArchiveRoutes::Impl::deviceHeaderOk(
    AsyncWebServerRequest* request) const {
    if (request == nullptr || identity_.deviceId[0] == '\0') return false;
    return request->header("X-PyroVue-Device-Id") == identity_.deviceId;
}

bool ArchiveRoutes::Impl::archiveAvailable() const {
    return logger_ != nullptr && archiveReady_;
}

int ArchiveRoutes::Impl::findAssembly(AsyncWebServerRequest* request) {
    for (uint8_t i = 0; i < slotCount_; ++i) {
        if (slots_[i].state.load() == ResponseSlotState::Assembling) {
            std::shared_ptr<AsyncWebServerRequest> held =
                slots_[i].request.lock();
            if (held.get() == request) return static_cast<int>(i);
        }
    }
    return -1;
}

bool ArchiveRoutes::Impl::reserveSlot(uint8_t& slotIndex,
                                      uint32_t& generation) {
    LockGuard guard(lock_);
    for (uint8_t i = 0; i < slotCount_; ++i) {
        ResponseSlot& slot = slots_[i];
        if (slot.state.load() != ResponseSlotState::Free) continue;
        const uint32_t next = slot.generation.load() + 1u;
        slot.generation.store(next);
        slot.state.store(ResponseSlotState::Assembling);
        slot.cancelled.store(false);
        slot.bodyLength = 0;
        slot.bodyLimit = 0;
        slot.invalid = false;
        slot.oversize = false;
        slot.responseLength = 0;
        slot.statusCode = 200;
        slotIndex = i;
        generation = next;
        return true;
    }
    return false;
}

void ArchiveRoutes::Impl::releaseSlotLocked(uint8_t slotIndex,
                                            uint32_t generation) {
    if (slotIndex >= kMaxSlotCount) return;
    ResponseSlot& slot = slots_[slotIndex];
    if (slot.generation.load() != generation) return;
    slot.state.store(ResponseSlotState::Free);
    slot.cancelled.store(false);
    slot.bodyLength = 0;
    slot.invalid = false;
    slot.oversize = false;
    slot.responseLength = 0;
}

void ArchiveRoutes::Impl::releaseSlot(uint8_t slotIndex, uint32_t generation) {
    LockGuard guard(lock_);
    releaseSlotLocked(slotIndex, generation);
}

void ArchiveRoutes::Impl::onBody(AsyncWebServerRequest* request,
                                 uint8_t* data, size_t len, size_t index,
                                 size_t total) {
    if (!request || request->getResponse()) return;

    RouteMatch route;
    if (!parseRoute(request->url(), request->method(), route)) {
        sendImmediateError(request, 400, ArchiveError::INVALID_REQUEST,
                           nullptr);
        return;
    }
    if (!deviceHeaderOk(request)) {
        sendImmediateError(request, 409, ArchiveError::DEVICE_ID_MISMATCH,
                           nullptr);
        return;
    }
    if (isArchiveRoute(route.kind) && !archiveAvailable()) {
        sendImmediateError(request, 404, ArchiveError::ARCHIVE_UNAVAILABLE,
                           nullptr);
        return;
    }

    const size_t limit = bodyLimitFor(route.kind);
    if (limit == 0) {
        if (len != 0) {
            sendImmediateError(request, 400, ArchiveError::INVALID_REQUEST,
                               nullptr);
        }
        return;
    }

    int slotIndex = findAssembly(request);
    if (slotIndex < 0) {
        if (total > 0 && total > limit) {
            sendImmediateError(request, 413, ArchiveError::BODY_TOO_LARGE,
                               nullptr);
            return;
        }
        uint8_t reserved = 0;
        uint32_t generation = 0;
        if (!reserveSlot(reserved, generation)) {
            sendImmediateError(request, 503, ArchiveError::BUSY, nullptr);
            return;
        }
        slots_[reserved].request = request->pause();
        installDisconnect(request, reserved, generation);
        slots_[reserved].bodyLimit = limit;
        slotIndex = static_cast<int>(reserved);
    }

    ResponseSlot& slot = slots_[slotIndex];
    if (slot.cancelled.load() ||
        slot.state.load() != ResponseSlotState::Assembling) {
        return;
    }
    if (slot.oversize) return;
    if (index != slot.bodyLength) {
        slot.invalid = true;
        return;
    }
    if (len != 0 && slot.bodyLength + len > slot.bodyLimit) {
        slot.oversize = true;
        releaseSlot(static_cast<uint8_t>(slotIndex), slot.generation.load());
        sendImmediateError(request, 413, ArchiveError::BODY_TOO_LARGE,
                           nullptr);
        return;
    }
    if (len != 0) {
        memcpy(slot.buffer + slot.bodyLength, data, len);
        slot.bodyLength += len;
    }
}

void ArchiveRoutes::Impl::onRequest(AsyncWebServerRequest* request) {
    if (!request || request->getResponse()) return;

    RouteMatch route;
    if (!parseRoute(request->url(), request->method(), route)) {
        sendImmediateError(request, 400, ArchiveError::INVALID_REQUEST,
                           nullptr);
        return;
    }
    if (!deviceHeaderOk(request)) {
        sendImmediateError(request, 409, ArchiveError::DEVICE_ID_MISMATCH,
                           nullptr);
        return;
    }
    if (isArchiveRoute(route.kind) && !archiveAvailable()) {
        sendImmediateError(request, 404, ArchiveError::ARCHIVE_UNAVAILABLE,
                           nullptr);
        return;
    }

    if (route.kind == RouteKind::Command) {
        handleCommandRequest(request, route);
        return;
    }
    if (route.kind == RouteKind::MarkerCreate ||
        route.kind == RouteKind::MarkerUpdate ||
        route.kind == RouteKind::MarkerDelete) {
        handleMarkerRequest(request, route);
        return;
    }
    handleBodylessRequest(request, route);
}

void ArchiveRoutes::Impl::handleCommandRequest(
    AsyncWebServerRequest* request, const RouteMatch& /*route*/) {
    const int slotIndex = findAssembly(request);
    if (slotIndex < 0) {
        sendImmediateError(request, 400, ArchiveError::INVALID_REQUEST,
                           nullptr);
        return;
    }
    ResponseSlot& slot = slots_[slotIndex];
    if (slot.cancelled.load() ||
        slot.state.load() != ResponseSlotState::Assembling) {
        return;
    }
    if (slot.oversize) {
        const uint32_t generation = slot.generation.load();
        releaseSlot(static_cast<uint8_t>(slotIndex), generation);
        sendImmediateError(request, 413, ArchiveError::BODY_TOO_LARGE,
                           nullptr);
        return;
    }
    if (slot.invalid || slot.bodyLength == 0 ||
        (request->contentLength() != 0 &&
         slot.bodyLength != request->contentLength())) {
        const uint32_t generation = slot.generation.load();
        releaseSlot(static_cast<uint8_t>(slotIndex), generation);
        sendImmediateError(request, 400, ArchiveError::INVALID_REQUEST,
                           nullptr);
        return;
    }

    CommandRequest command;
    const ArchiveError error =
        parseCommandBody(slot.buffer, slot.bodyLength, arena_, command);
    if (error != ArchiveError::NONE) {
        const uint32_t generation = slot.generation.load();
        releaseSlot(static_cast<uint8_t>(slotIndex), generation);
        sendImmediateError(request, 400, error, nullptr);
        return;
    }

    const uint8_t reserved = static_cast<uint8_t>(slotIndex);
    const uint32_t generation = slot.generation.load();
    if (!enqueueCommand(reserved, generation, command)) {
        releaseSlot(reserved, generation);
        sendImmediateError(request, 503, ArchiveError::BUSY, nullptr);
    }
}

void ArchiveRoutes::Impl::handleMarkerRequest(
    AsyncWebServerRequest* request, const RouteMatch& route) {
    const int slotIndex = findAssembly(request);
    if (slotIndex < 0) {
        sendImmediateError(request, 400, ArchiveError::INVALID_REQUEST,
                           nullptr);
        return;
    }
    ResponseSlot& slot = slots_[slotIndex];
    if (slot.cancelled.load() ||
        slot.state.load() != ResponseSlotState::Assembling) {
        return;
    }
    if (slot.oversize) {
        const uint32_t generation = slot.generation.load();
        releaseSlot(static_cast<uint8_t>(slotIndex), generation);
        sendImmediateError(request, 413, ArchiveError::BODY_TOO_LARGE,
                           nullptr);
        return;
    }
    if (slot.invalid || slot.bodyLength == 0 ||
        (request->contentLength() != 0 &&
         slot.bodyLength != request->contentLength())) {
        const uint32_t generation = slot.generation.load();
        releaseSlot(static_cast<uint8_t>(slotIndex), generation);
        sendImmediateError(request, 400, ArchiveError::INVALID_REQUEST,
                           nullptr);
        return;
    }

#if defined(PYROVUE_RUN_ARCHIVE)
    MarkerMutationKind kind = MarkerMutationKind::Create;
    if (route.kind == RouteKind::MarkerUpdate) {
        kind = MarkerMutationKind::Update;
    } else if (route.kind == RouteKind::MarkerDelete) {
        kind = MarkerMutationKind::Delete;
    }
    MarkerMutation mutation;
    const ArchiveError error =
        parseMarkerBody(slot.buffer, slot.bodyLength, arena_, kind,
                        route.markerId, mutation);
    if (error != ArchiveError::NONE) {
        const uint32_t generation = slot.generation.load();
        releaseSlot(static_cast<uint8_t>(slotIndex), generation);
        sendImmediateError(request, 400, error, nullptr);
        return;
    }

    const uint8_t reserved = static_cast<uint8_t>(slotIndex);
    const uint32_t generation = slot.generation.load();
    if (!enqueueMutation(reserved, generation, route.runId, mutation)) {
        releaseSlot(reserved, generation);
        sendImmediateError(request, 503, ArchiveError::BUSY, nullptr);
    }
#else
    const uint32_t generation = slot.generation.load();
    releaseSlot(static_cast<uint8_t>(slotIndex), generation);
    sendImmediateError(request, 404, ArchiveError::ARCHIVE_UNAVAILABLE,
                       nullptr);
#endif
}

void ArchiveRoutes::Impl::handleBodylessRequest(
    AsyncWebServerRequest* request, const RouteMatch& route) {
    if (request->contentLength() != 0) {
        sendImmediateError(request, 400, ArchiveError::INVALID_REQUEST,
                           nullptr);
        return;
    }

    WorkerJobKind kind = WorkerJobKind::GetMeta;
    uint32_t beforeRunId = 0;
    uint32_t after = 0;
    uint32_t through = 0;
    uint8_t limit = 1;

    switch (route.kind) {
        case RouteKind::ListRuns: {
            kind = WorkerJobKind::ListRuns;
            if (request->hasArg("beforeRunId") &&
                !parseUint32(request->arg("beforeRunId"), beforeRunId)) {
                sendImmediateError(request, 400,
                                   ArchiveError::INVALID_REQUEST, nullptr);
                return;
            }
            limit = static_cast<uint8_t>(kMaxListLimit);
            if (request->hasArg("limit")) {
                uint32_t value = 0;
                if (!parseUint32(request->arg("limit"), value) ||
                    value < 1 || value > kMaxListLimit) {
                    sendImmediateError(request, 400,
                                       ArchiveError::INVALID_REQUEST, nullptr);
                    return;
                }
                limit = static_cast<uint8_t>(value);
            }
            break;
        }
        case RouteKind::Samples: {
            kind = WorkerJobKind::Samples;
            if (request->hasArg("afterLogSeq") &&
                !parseUint32(request->arg("afterLogSeq"), after)) {
                sendImmediateError(request, 400,
                                   ArchiveError::INVALID_REQUEST, nullptr);
                return;
            }
            if (!request->hasArg("throughLogSeq") ||
                !parseUint32(request->arg("throughLogSeq"), through)) {
                sendImmediateError(request, 400,
                                   ArchiveError::INVALID_REQUEST, nullptr);
                return;
            }
            limit = static_cast<uint8_t>(kMaxSampleLimit);
            if (request->hasArg("limit")) {
                uint32_t value = 0;
                if (!parseUint32(request->arg("limit"), value) ||
                    value < 1 || value > kMaxSampleLimit) {
                    sendImmediateError(request, 400,
                                       ArchiveError::INVALID_REQUEST, nullptr);
                    return;
                }
                limit = static_cast<uint8_t>(value);
            }
            break;
        }
        case RouteKind::Events: {
            kind = WorkerJobKind::Events;
            if (request->hasArg("afterRevision") &&
                !parseUint32(request->arg("afterRevision"), after)) {
                sendImmediateError(request, 400,
                                   ArchiveError::INVALID_REQUEST, nullptr);
                return;
            }
            if (!request->hasArg("throughRevision") ||
                !parseUint32(request->arg("throughRevision"), through)) {
                sendImmediateError(request, 400,
                                   ArchiveError::INVALID_REQUEST, nullptr);
                return;
            }
            limit = static_cast<uint8_t>(kMaxEventLimit);
            if (request->hasArg("limit")) {
                uint32_t value = 0;
                if (!parseUint32(request->arg("limit"), value) ||
                    value < 1 || value > kMaxEventLimit) {
                    sendImmediateError(request, 400,
                                       ArchiveError::INVALID_REQUEST, nullptr);
                    return;
                }
                limit = static_cast<uint8_t>(value);
            }
            break;
        }
        case RouteKind::GetMeta:
            kind = WorkerJobKind::GetMeta;
            break;
        case RouteKind::Snapshot:
            kind = WorkerJobKind::Snapshot;
            break;
        case RouteKind::DeleteRun:
            kind = WorkerJobKind::DeleteRun;
            break;
        default:
            sendImmediateError(request, 400, ArchiveError::INVALID_REQUEST,
                               nullptr);
            return;
    }

    uint8_t reserved = 0;
    uint32_t generation = 0;
    if (!reserveSlot(reserved, generation)) {
        sendImmediateError(request, 503, ArchiveError::BUSY, nullptr);
        return;
    }
    slots_[reserved].request = request->pause();
    installDisconnect(request, reserved, generation);
    if (!enqueueRead(kind, reserved, generation, route.runId, beforeRunId,
                     after, through, limit)) {
        releaseSlot(reserved, generation);
        sendImmediateError(request, 503, ArchiveError::BUSY, nullptr);
    }
}

// ---------------------------------------------------------------------------
// Queueing
// ---------------------------------------------------------------------------

bool ArchiveRoutes::Impl::enqueueCommand(uint8_t slotIndex,
                                         uint32_t generation,
                                         const CommandRequest& command) {
    LockGuard guard(lock_);
    if (jobCapacity_ == 0 || jobCount_ >= jobCapacity_) return false;
    WorkerJob& job = jobs_[jobTail_];
    job.kind = WorkerJobKind::Command;
    job.slotIndex = slotIndex;
    job.slotGeneration = generation;
    job.request = slots_[slotIndex].request;
    job.cancelled.store(false);
    memcpy(job.expectedDeviceId, identity_.deviceId,
           sizeof(job.expectedDeviceId));
    job.expectedDeviceId[sizeof(job.expectedDeviceId) - 1] = '\0';
    job.command = command;
    job.runId = 0;
    job.beforeRunId = 0;
    job.after = 0;
    job.through = 0;
    job.limit = 0;
#if defined(PYROVUE_RUN_ARCHIVE)
    job.marker = MarkerMutation{};
#endif
    jobTail_ = static_cast<uint8_t>((jobTail_ + 1u) % jobCapacity_);
    ++jobCount_;
    slots_[slotIndex].state.store(ResponseSlotState::Queued);
    return true;
}

bool ArchiveRoutes::Impl::enqueueRead(WorkerJobKind kind, uint8_t slotIndex,
                                      uint32_t generation, uint32_t runId,
                                      uint32_t beforeRunId, uint32_t after,
                                      uint32_t through, uint8_t limit) {
    LockGuard guard(lock_);
    if (jobCapacity_ == 0 || jobCount_ >= jobCapacity_) return false;
    WorkerJob& job = jobs_[jobTail_];
    job.kind = kind;
    job.slotIndex = slotIndex;
    job.slotGeneration = generation;
    job.request = slots_[slotIndex].request;
    job.cancelled.store(false);
    memcpy(job.expectedDeviceId, identity_.deviceId,
           sizeof(job.expectedDeviceId));
    job.expectedDeviceId[sizeof(job.expectedDeviceId) - 1] = '\0';
    job.command = CommandRequest{};
    job.runId = runId;
    job.beforeRunId = beforeRunId;
    job.after = after;
    job.through = through;
    job.limit = limit;
    job.listCount = 0;
    job.listIndex = 0;
    job.listPhase = 0;
    job.listNextBefore = 0;
    for (uint8_t i = 0; i < kMaxListLimit; ++i) job.listIds[i] = 0;
#if defined(PYROVUE_RUN_ARCHIVE)
    job.marker = MarkerMutation{};
#endif
    jobTail_ = static_cast<uint8_t>((jobTail_ + 1u) % jobCapacity_);
    ++jobCount_;
    slots_[slotIndex].state.store(ResponseSlotState::Queued);
    return true;
}

#if defined(PYROVUE_RUN_ARCHIVE)
bool ArchiveRoutes::Impl::enqueueMutation(
    uint8_t slotIndex, uint32_t generation, uint32_t runId,
    const MarkerMutation& mutation) {
    LockGuard guard(lock_);
    if (jobCapacity_ == 0 || jobCount_ >= jobCapacity_) return false;
    WorkerJob& job = jobs_[jobTail_];
    job.kind = WorkerJobKind::MutateMarker;
    job.slotIndex = slotIndex;
    job.slotGeneration = generation;
    job.request = slots_[slotIndex].request;
    job.cancelled.store(false);
    memcpy(job.expectedDeviceId, identity_.deviceId,
           sizeof(job.expectedDeviceId));
    job.expectedDeviceId[sizeof(job.expectedDeviceId) - 1] = '\0';
    job.command = CommandRequest{};
    job.runId = runId;
    job.beforeRunId = 0;
    job.after = 0;
    job.through = 0;
    job.limit = 0;
    job.marker = mutation;
    jobTail_ = static_cast<uint8_t>((jobTail_ + 1u) % jobCapacity_);
    ++jobCount_;
    slots_[slotIndex].state.store(ResponseSlotState::Queued);
    return true;
}
#endif

// ---------------------------------------------------------------------------
// Owner-loop job execution
// ---------------------------------------------------------------------------

void ArchiveRoutes::Impl::poll(uint32_t nowMs) {
    WorkerJob* job = nullptr;
    {
        LockGuard guard(lock_);
        if (jobCapacity_ == 0 || jobCount_ == 0) return;
        job = &jobs_[jobHead_];
    }

    bool sent = false;
    switch (job->kind) {
        case WorkerJobKind::Command: {
#if defined(PYROVUE_RUN_ARCHIVE)
            // A stop retry may target a historical run whose metadata is not
            // cached.  Warm it in bounded steps instead of letting the command
            // handler perform a full synchronous scan.  The active run is
            // returned immediately by prepareReadStep.
            if (job->command.command == CommandKind::Stop &&
                job->command.targetRunId != 0 && logger_ != nullptr) {
                ArchiveError prepareError = ArchiveError::NONE;
                if (!logger_->archive().prepareReadStep(
                        job->command.targetRunId, prepareError)) {
                    return;
                }
            }
#endif
            CommandResult result{};
            if (commandHandler_ != nullptr && *commandHandler_) {
                result = (*commandHandler_)(job->command);
            } else {
                result.error = ArchiveError::BUSY;
            }
            if (commandHandler_ != nullptr && *commandHandler_ &&
                stateChanged_) {
                stateChanged_();
            }
            if (!job->cancelled.load()) {
                if (result.ok) {
                    sent = writeCommandResponse(*job, result);
                } else {
                    sent = writeErrorResponse(
                        *job,
                        result.error == ArchiveError::NONE
                            ? ArchiveError::INVALID_REQUEST
                            : result.error,
                        job->command.requestId);
                }
            }
            break;
        }
        case WorkerJobKind::DeleteRun:
            sent = executeDelete(*job);
            break;
        case WorkerJobKind::MutateMarker: {
            bool mutationSent = false;
            if (!executeMutation(*job, nowMs, mutationSent)) {
                // The bounded marker cache was still loading; the job stays
                // queued at the head without blocking the sensor tick.
                return;
            }
            sent = mutationSent;
            break;
        }
        default: {
            if (job->cancelled.load()) {
                finishJob(*job, false);
                return;
            }
            bool readSent = false;
            if (!executeRead(*job, readSent)) return;
            sent = readSent;
            break;
        }
    }

    finishJob(*job, sent);
}

bool ArchiveRoutes::Impl::executeDelete(WorkerJob& job) {
#if defined(PYROVUE_RUN_ARCHIVE)
    if (logger_ == nullptr) {
        return writeErrorResponse(job, ArchiveError::ARCHIVE_UNAVAILABLE,
                                  nullptr);
    }
    bool deleted = false;
    const ArchiveError error = logger_->archive().deleteRun(job.runId, deleted);
    if (job.cancelled.load()) return false;
    if (error != ArchiveError::NONE) {
        return writeErrorResponse(job, error, nullptr);
    }
    return writeDeleteResponse(job, deleted);
#else
    (void)job;
    return false;
#endif
}

bool ArchiveRoutes::Impl::executeMutation(WorkerJob& job, uint32_t nowMs,
                                            bool& sent) {
    sent = false;
#if defined(PYROVUE_RUN_ARCHIVE)
    if (logger_ == nullptr) {
        if (!job.cancelled.load()) {
            sent = writeErrorResponse(job, ArchiveError::ARCHIVE_UNAVAILABLE,
                                      job.marker.operationId);
        }
        return true;
    }
    RunArchiveStore& store = logger_->archive();
    ArchiveError prepareError = ArchiveError::NONE;
    if (!store.prepareReadStep(job.runId, prepareError)) {
        return false;  // bounded metadata preparation is still in progress
    }
    if (prepareError != ArchiveError::NONE) {
        if (!job.cancelled.load()) {
            sent = writeErrorResponse(job, prepareError,
                                      job.marker.operationId);
        }
        return true;
    }
    RunMeta meta;
    const ArchiveError metaError = store.getMeta(job.runId, meta);
    if (metaError != ArchiveError::NONE) {
        if (!job.cancelled.load()) {
            sent = writeErrorResponse(job, metaError, job.marker.operationId);
        }
        return true;
    }
    if (meta.corrupt) {
        if (!job.cancelled.load()) {
            sent = writeErrorResponse(job, ArchiveError::CORRUPT_RUN,
                                      job.marker.operationId);
        }
        return true;
    }
    MutationResult result{};
    ArchiveError error = ArchiveError::NONE;
    if (!logger_->mutateMarkerStep(job.runId, job.marker, nowMs, result,
                                   error)) {
        return false;
    }
    if (job.cancelled.load()) return true;
    if (error != ArchiveError::NONE) {
        sent = writeErrorResponse(job, error, job.marker.operationId);
        return true;
    }
    sent = writeMutationResponse(job, result);
    return true;
#else
    (void)job;
    (void)nowMs;
    return true;
#endif
}

bool ArchiveRoutes::Impl::executeRead(WorkerJob& job, bool& sent) {
    sent = false;
#if defined(PYROVUE_RUN_ARCHIVE)
    if (logger_ == nullptr) {
        sent = writeErrorResponse(job, ArchiveError::ARCHIVE_UNAVAILABLE,
                                  nullptr);
        return true;
    }
    RunArchiveStore& store = logger_->archive();

    // prepareReadStep is bounded to at most 64 raw records or 8 event frames
    // per call.  A false return means the job must yield and be retried on a
    // later owner-loop poll; it must not block the sensor tick.
    auto prepare = [&](uint32_t runId, bool& pending) -> ArchiveError {
        ArchiveError error = ArchiveError::NONE;
        if (!store.prepareReadStep(runId, error)) {
            pending = true;
            return ArchiveError::NONE;
        }
        pending = false;
        return error;
    };

    switch (job.kind) {
        case WorkerJobKind::ListRuns: {
            if (job.listPhase == 0) {
                job.listCount = 0;
                job.listIndex = 0;
                job.listNextBefore = 0;
                for (uint8_t i = 0; i < kMaxListLimit; ++i) {
                    job.listIds[i] = 0;
                }
                const ArchiveError idsError = store.listRunIds(
                    job.beforeRunId, job.limit, job.listIds, kMaxListLimit,
                    job.listNextBefore);
                if (idsError != ArchiveError::NONE) {
                    sent = writeErrorResponse(job, idsError, nullptr);
                    return true;
                }
                while (job.listCount < job.limit &&
                       job.listIds[job.listCount] != 0) {
                    ++job.listCount;
                }
                job.listPhase = 1;
            }
            while (job.listIndex < job.listCount) {
                bool pending = false;
                (void)prepare(job.listIds[job.listIndex], pending);
                if (pending) return false;
                // A per-run metadata problem must not hide healthy catalog
                // entries; listRuns below emits the cached corrupt variant.
                ++job.listIndex;
            }
            memset(metaScratch_, 0, sizeof(RunMeta) * kMaxListLimit);
            uint32_t nextBeforeRunId = 0;
            const ArchiveError error =
                store.listRuns(job.beforeRunId, job.limit, metaScratch_,
                               job.limit, nextBeforeRunId);
            if (error != ArchiveError::NONE) {
                sent = writeErrorResponse(job, error, nullptr);
                return true;
            }
            uint32_t count = job.listCount;
            if (count > job.limit) count = job.limit;
            sent = writeListResponse(job, metaScratch_, count,
                                     nextBeforeRunId);
            return true;
        }
        case WorkerJobKind::GetMeta: {
            bool pending = false;
            (void)prepare(job.runId, pending);
            if (pending) return false;
            // A corrupt-but-listed run still has a valid corrupt meta
            // variant, so getMeta decides the final result here.
            RunMeta meta;
            const ArchiveError error = store.getMeta(job.runId, meta);
            if (error != ArchiveError::NONE) {
                sent = writeErrorResponse(job, error, nullptr);
                return true;
            }
            sent = writeMetaResponse(job, meta);
            return true;
        }
        case WorkerJobKind::Snapshot:
        case WorkerJobKind::Samples:
        case WorkerJobKind::Events: {
            bool pending = false;
            const ArchiveError prepareError = prepare(job.runId, pending);
            if (pending) return false;
            if (prepareError != ArchiveError::NONE) {
                sent = writeErrorResponse(job, prepareError, nullptr);
                return true;
            }
            if (job.kind == WorkerJobKind::Snapshot) {
                RunSnapshot snapshot;
                const ArchiveError error =
                    store.captureSnapshot(job.runId, snapshot);
                if (error != ArchiveError::NONE) {
                    sent = writeErrorResponse(job, error, nullptr);
                    return true;
                }
                sent = writeSnapshotResponse(job, snapshot);
                return true;
            }
            if (job.kind == WorkerJobKind::Samples) {
                PageInfo page;
                const ArchiveError error = store.readSamplesPage(
                    job.runId, job.after, job.through, job.limit,
                    sampleScratch_, kMaxSampleLimit, page);
                if (error != ArchiveError::NONE) {
                    sent = writeErrorResponse(job, error, nullptr);
                    return true;
                }
                sent = writeSamplesResponse(job, sampleScratch_, page,
                                            job.after);
                return true;
            }
            ResponseSlot& slot = slots_[job.slotIndex];
            if (slot.capacity <= kEventArrayOffset + kEventSuffixReserve) {
                sent = writeErrorResponse(job, ArchiveError::WRITE_FAILED,
                                          nullptr);
                return true;
            }
            ArchiveError warmError = ArchiveError::NONE;
            if (!store.warmMarkerCacheStep(job.runId, warmError)) {
                return false;  // one bounded event-index step per poll
            }
            if (warmError != ArchiveError::NONE) {
                sent = writeErrorResponse(job, warmError, nullptr);
                return true;
            }
            const size_t arrayCapacity =
                slot.capacity - kEventArrayOffset - kEventSuffixReserve;
            PageInfo page;
            const ArchiveError error = store.readEventsPage(
                job.runId, job.after, job.through, job.limit,
                slot.buffer + kEventArrayOffset, arrayCapacity, page);
            if (error != ArchiveError::NONE) {
                sent = writeErrorResponse(job, error, nullptr);
                return true;
            }
            sent = writeEventsResponse(job, page, job.after);
            return true;
        }
        default:
            sent = writeErrorResponse(job, ArchiveError::INVALID_REQUEST,
                                      nullptr);
            return true;
    }
#else
    (void)job;
    sent = false;
    return true;
#endif
}

void ArchiveRoutes::Impl::finishJob(const WorkerJob& job, bool sent) {
    LockGuard guard(lock_);
    if (!sent) releaseSlotLocked(job.slotIndex, job.slotGeneration);
    if (jobCapacity_ != 0) {
        jobHead_ = static_cast<uint8_t>((jobHead_ + 1u) % jobCapacity_);
        if (jobCount_ != 0) --jobCount_;
    }
}

// ---------------------------------------------------------------------------
// Response construction
// ---------------------------------------------------------------------------

bool ArchiveRoutes::Impl::markReady(const WorkerJob& job, int status,
                                    size_t length) {
    LockGuard guard(lock_);
    ResponseSlot& slot = slots_[job.slotIndex];
    if (slot.generation.load() != job.slotGeneration) return false;
    if (strcmp(job.expectedDeviceId, identity_.deviceId) != 0) return false;
    if (slot.state.load() == ResponseSlotState::Cancelled ||
        slot.cancelled.load()) {
        return false;
    }
    if (slot.state.load() != ResponseSlotState::Queued) return false;
    slot.statusCode = status;
    slot.responseLength = length;
    slot.state.store(ResponseSlotState::Ready);
    return true;
}

bool ArchiveRoutes::Impl::sendSlot(const WorkerJob& job) {
    std::weak_ptr<AsyncWebServerRequest> request;
    const uint8_t* data = nullptr;
    size_t length = 0;
    int status = 200;
    {
        LockGuard guard(lock_);
        ResponseSlot& slot = slots_[job.slotIndex];
        if (slot.generation.load() != job.slotGeneration) return false;
        if (slot.state.load() != ResponseSlotState::Ready ||
            slot.cancelled.load()) {
            return false;
        }
        slot.state.store(ResponseSlotState::Sending);
        request = slot.request;
        data = slot.buffer;
        length = slot.responseLength;
        status = slot.statusCode;
    }

    std::shared_ptr<AsyncWebServerRequest> held = request.lock();
    if (!held || held->isSent()) {
        releaseSlot(job.slotIndex, job.slotGeneration);
        return false;
    }
    if (held->client() == nullptr || !held->client()->connected()) {
        releaseSlot(job.slotIndex, job.slotGeneration);
        return false;
    }

    ArchiveResponse* response = new (std::nothrow)
        ArchiveResponse(*owner_, job.slotIndex, job.slotGeneration, status,
                        data, length);
    if (response == nullptr) {
        releaseSlot(job.slotIndex, job.slotGeneration);
        return false;
    }
    held->send(response);
    return true;
}

bool ArchiveRoutes::Impl::writeErrorResponse(const WorkerJob& job,
                                             ArchiveError error,
                                             const char* requestId) {
    ResponseSlot& slot = slots_[job.slotIndex];
    JsonOut out(slot.buffer, slot.capacity);
    if (!writeErrorJson(out, error, requestId)) return false;
    if (error == ArchiveError::NONE) error = ArchiveError::INVALID_REQUEST;
    return markReady(job, httpStatusFor(error), out.len) && sendSlot(job);
}

bool ArchiveRoutes::Impl::writeCommandResponse(
    const WorkerJob& job, const CommandResult& result) {
    ResponseSlot& slot = slots_[job.slotIndex];
    JsonOut out(slot.buffer, slot.capacity);
    if (!writeCommandResultJson(out, result, job.command.requestId)) {
        return false;
    }
    return markReady(job, 200, out.len) && sendSlot(job);
}

bool ArchiveRoutes::Impl::writeListResponse(const WorkerJob& job,
                                            RunMeta* runs, uint32_t count,
                                            uint32_t nextBeforeRunId) {
    ResponseSlot& slot = slots_[job.slotIndex];
    JsonOut out(slot.buffer, slot.capacity);
    out.str("{\"schemaVersion\":");
    out.u32(runlog::kSchemaVersion);
    out.str(",\"runs\":[");
    for (uint32_t i = 0; i < count; ++i) {
        if (i != 0) out.ch(',');
        if (!writeRunMetaJson(out, runs[i])) return false;
    }
    out.str("],\"nextBeforeRunId\":");
    if (nextBeforeRunId == 0) {
        out.str("null");
    } else {
        out.u32(nextBeforeRunId);
    }
    out.ch('}');
    if (!out.ok) return false;
    return markReady(job, 200, out.len) && sendSlot(job);
}

bool ArchiveRoutes::Impl::writeMetaResponse(const WorkerJob& job,
                                            const RunMeta& meta) {
    ResponseSlot& slot = slots_[job.slotIndex];
    JsonOut out(slot.buffer, slot.capacity);
    if (!writeRunMetaJson(out, meta)) return false;
    return markReady(job, 200, out.len) && sendSlot(job);
}

bool ArchiveRoutes::Impl::writeSnapshotResponse(
    const WorkerJob& job, const RunSnapshot& snapshot) {
    ResponseSlot& slot = slots_[job.slotIndex];
    JsonOut out(slot.buffer, slot.capacity);
    out.str("{\"schemaVersion\":");
    out.u32(runlog::kSchemaVersion);
    out.str(",\"meta\":");
    if (!writeRunMetaJson(out, snapshot.meta)) return false;
    out.str(",\"throughLogSeq\":");
    out.u32(snapshot.throughLogSeq);
    out.str(",\"throughEventRevision\":");
    out.u32(snapshot.throughEventRevision);
    out.ch('}');
    if (!out.ok) return false;
    return markReady(job, 200, out.len) && sendSlot(job);
}

bool ArchiveRoutes::Impl::writeSamplesResponse(
    const WorkerJob& job, const LoggedSample* samples, const PageInfo& page,
    uint32_t after) {
    ResponseSlot& slot = slots_[job.slotIndex];
    JsonOut out(slot.buffer, slot.capacity);
    out.str("{\"schemaVersion\":");
    out.u32(runlog::kSchemaVersion);
    out.str(",\"deviceId\":");
    out.quoted(page.deviceId);
    out.str(",\"runBootId\":");
    out.quoted(page.runBootId);
    out.str(",\"runId\":");
    out.u32(page.runId);
    out.str(",\"throughLogSeq\":");
    out.u32(page.through);
    out.str(",\"afterLogSeq\":");
    out.u32(after);
    out.str(",\"samples\":[");
    for (uint32_t i = 0; i < page.count; ++i) {
        if (i != 0) out.ch(',');
        const LoggedSample& sample = samples[i];
        out.str("{\"logSeq\":");
        out.u32(sample.logSeq);
        out.str(",\"acqSeq\":");
        out.u32(sample.acqSeq);
        out.str(",\"ms\":");
        out.u32(sample.ms);
        out.str(",\"temp\":");
        if (isfinite(sample.tempC)) {
            char number[40];
            const int written = snprintf(number, sizeof(number), "%.9g",
                                         static_cast<double>(sample.tempC));
            if (written <= 0 || written >= static_cast<int>(sizeof(number))) {
                return false;
            }
            out.str(number);
        } else {
            out.str("null");
        }
        out.str(",\"fault\":");
        out.u32(sample.fault);
        out.str(",\"intervalFault\":");
        out.u32(sample.intervalFault);
        out.str(",\"flags\":");
        out.u32(sample.flags);
        out.ch('}');
    }
    out.str("],\"nextAfterLogSeq\":");
    out.u32(page.nextCursor);
    out.str(",\"done\":");
    out.boolean(page.done);
    out.ch('}');
    if (!out.ok) return false;
    return markReady(job, 200, out.len) && sendSlot(job);
}

bool ArchiveRoutes::Impl::writeEventsResponse(const WorkerJob& job,
                                              const PageInfo& page,
                                              uint32_t after) {
    ResponseSlot& slot = slots_[job.slotIndex];
    if (frameScratch_ == nullptr ||
        slot.capacity <= kEventArrayOffset + kEventSuffixReserve ||
        slot.buffer == nullptr) {
        return false;
    }

    char* prefix = reinterpret_cast<char*>(frameScratch_);
    const int prefixLength = snprintf(
        prefix, kFrameScratchBytes,
        "{\"schemaVersion\":%lu,\"deviceId\":\"%s\",\"runBootId\":\"%s\","
        "\"runId\":%lu,\"throughRevision\":%lu,\"afterRevision\":%lu,"
        "\"events\":",
        static_cast<unsigned long>(runlog::kSchemaVersion), page.deviceId,
        page.runBootId, static_cast<unsigned long>(page.runId),
        static_cast<unsigned long>(page.through),
        static_cast<unsigned long>(after));
    if (prefixLength <= 0 ||
        static_cast<size_t>(prefixLength) >= kFrameScratchBytes ||
        static_cast<size_t>(prefixLength) > kEventArrayOffset) {
        return false;
    }

    const size_t arrayLength =
        strlen(reinterpret_cast<const char*>(slot.buffer + kEventArrayOffset));
    if (kEventArrayOffset + arrayLength + 1 > slot.capacity) return false;

    memmove(slot.buffer + prefixLength, slot.buffer + kEventArrayOffset,
            arrayLength + 1);
    memcpy(slot.buffer, prefix, static_cast<size_t>(prefixLength));

    const size_t suffixOffset = static_cast<size_t>(prefixLength) + arrayLength;
    const int suffixLength = snprintf(
        reinterpret_cast<char*>(slot.buffer + suffixOffset),
        slot.capacity - suffixOffset,
        ",\"nextAfterRevision\":%lu,\"done\":%s}",
        static_cast<unsigned long>(page.nextCursor),
        page.done ? "true" : "false");
    if (suffixLength <= 0 ||
        suffixOffset + static_cast<size_t>(suffixLength) >= slot.capacity) {
        return false;
    }
    return markReady(job, 200, suffixOffset + static_cast<size_t>(suffixLength)) &&
           sendSlot(job);
}

bool ArchiveRoutes::Impl::writeMutationResponse(
    const WorkerJob& job, const MutationResult& result) {
    ResponseSlot& slot = slots_[job.slotIndex];
    JsonOut out(slot.buffer, slot.capacity);
    if (!writeMutationResultJson(out, result)) return false;
    return markReady(job, 200, out.len) && sendSlot(job);
}

bool ArchiveRoutes::Impl::writeDeleteResponse(const WorkerJob& job,
                                              bool deleted) {
    ResponseSlot& slot = slots_[job.slotIndex];
    JsonOut out(slot.buffer, slot.capacity);
    if (!writeDeleteResultJson(out, job.runId, deleted)) return false;
    return markReady(job, 200, out.len) && sendSlot(job);
}

// ---------------------------------------------------------------------------
// Public facade
// ---------------------------------------------------------------------------

ArchiveRoutes::ArchiveRoutes() : impl_(new (std::nothrow) Impl(this)) {}

ArchiveRoutes::~ArchiveRoutes() {
    Impl* impl = impl_;
    impl_ = nullptr;
    if (impl != nullptr) {
        impl->releaseResources();
        delete impl;
    }
}

void ArchiveRoutes::setIdentity(const DeviceIdentity& identity) {
    if (impl_ != nullptr) impl_->identity_ = identity;
}

void ArchiveRoutes::onStateChanged(std::function<void()> stateChanged) {
    if (impl_ != nullptr) impl_->stateChanged_ = std::move(stateChanged);
}

void ArchiveRoutes::begin(
    AsyncWebServer& server,
    std::function<CommandResult(const CommandRequest&)>& commandHandler,
    RunLogService* logger) {
    if (impl_ != nullptr) impl_->begin(server, commandHandler, logger);
}

void ArchiveRoutes::poll(uint32_t nowMs) {
    if (impl_ != nullptr) impl_->poll(nowMs);
}

void ArchiveRoutes::releaseResponseSlot(uint8_t slotIndex,
                                        uint32_t generation) {
    if (impl_ != nullptr) impl_->releaseSlot(slotIndex, generation);
}
