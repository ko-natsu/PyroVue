// RunArchiveStore: the single owner of /littlefs archive IO on the N16R8.
// All calls happen on the Arduino loop task. The store mounts the filesystem,
// validates bounded file prefixes, keeps the active run writer, and owns the
// one PSRAM journal cache + MarkerReducer used for annotation lookups.
//
// On-disk layout (see docs/n16r8-migration-plan.md):
//   /runs/<runId>/meta.json      immutable start metadata (schema 1)
//   /runs/<runId>/samples.bin    rawlogcodec PVR1 header + 24-byte records
//   /runs/<runId>/events.bin     eventjournalcodec PVE1 header + JSON frames
//   /runs/.creating-<runId>/     prepared but not renamed
//   /runs/.deleting-<runId>/     explicitly deleted, waiting for unlink
//
// Recovery never resumes a writer and never truncates samples.bin. A single
// invalid event suffix is preserved as events-tail-<firstBadOffset>.bin before
// the validated prefix is truncated and repaired.

#include "RunArchiveStore.h"

#if defined(PYROVUE_RUN_ARCHIVE) && defined(ARDUINO_ARCH_ESP32)

#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "EventJournalCodec.h"
#include "MarkerReducer.h"
#include "RawLogCodec.h"

#if defined(ARDUINO_ARCH_ESP32)
#include <esp_heap_caps.h>
#include <esp32s3/spiram.h>
#endif

namespace {

// Checked reservation arithmetic from the approved plan. These are logical
// figures; every actual write additionally checks real free space.
constexpr size_t kRawLogicalMaxBytes = 32u + 43202u * 24u;  // 1,036,880
constexpr size_t kRawReservationBytes = 1040384u;           // 254 sectors
constexpr size_t kEventBudgetBytes = 262144u;
constexpr size_t kLifecycleReserveBytes = 8192u;
constexpr size_t kMetaAllowanceBytes = 65536u;
constexpr size_t kFsHeadroomBytes = 1048576u;
constexpr uint32_t kStartRequiredBytes = 2416640u;
constexpr size_t kSectorBytes = 4096u;

constexpr size_t kJournalCacheBytes = 262144u;
constexpr size_t kFrameScratchBytes =
    eventjournalcodec::kMaxFrameBytes + 8u;  // encode/decode headroom
constexpr size_t kWarmReadBudgetBytes = 32768u;
constexpr size_t kWarmMaxFramesPerStep = 8u;
constexpr size_t kPendingCapacity = 32u;
constexpr size_t kRecordBytes = rawlogcodec::kRecordSize;
constexpr size_t kMetaReadLimit = 4096u;
constexpr uint8_t kMaxSamplePage = 64u;
constexpr uint8_t kMaxEventPage = 8u;
constexpr size_t kDirScanOverflowVariants = 100u;

static_assert(kRawLogicalMaxBytes == 1036880u, "run record budget changed");
static_assert(kRecordBytes == 24u, "raw record size changed");
static_assert(kPendingCapacity == 32u, "pending record capacity changed");

// --------------------------------------------------------------- helpers ---

uint32_t roundUpSector(size_t value) {
    const size_t rounded = (value + kSectorBytes - 1u) & ~(kSectorBytes - 1u);
    return rounded > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(rounded);
}

uint32_t clampU32(size_t value) {
    return value > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(value);
}

uint32_t freeFsBytes() {
    const size_t total = LittleFS.totalBytes();
    const size_t used = LittleFS.usedBytes();
    if (total <= used) return 0;
    return clampU32(total - used);
}

bool boundedCopy(char* dst, size_t capacity, const char* src) {
    if (dst == nullptr || capacity == 0 || src == nullptr) return false;
    const size_t length = strnlen(src, capacity);
    if (length >= capacity) return false;
    memcpy(dst, src, length);
    dst[length] = '\0';
    return true;
}

size_t boundedLength(const char* s, size_t maximum) {
    if (s == nullptr) return 0;
    return strnlen(s, maximum);
}

bool isLowerHex(const char* s, size_t length) {
    if (s == nullptr) return false;
    for (size_t i = 0; i < length; ++i) {
        const char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return s[length] == '\0';
}

bool validDeviceId(const char* s) { return isLowerHex(s, 12); }
bool validBootId(const char* s) { return isLowerHex(s, 16); }

bool validRequestId(const char* s) {
    if (s == nullptr) return false;
    const size_t length = strnlen(s, 81);
    if (length == 0 || length > 80) return false;
    if (s[length] != '\0') return false;
    for (size_t i = 0; i < length; ++i) {
        const char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
              (c >= 'a' && c <= 'z') || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

bool parseDecimalId(const char* s, size_t length, uint32_t& out) {
    if (s == nullptr || length == 0 || length > 10) return false;
    uint64_t value = 0;
    for (size_t i = 0; i < length; ++i) {
        const char c = s[i];
        if (c < '0' || c > '9') return false;
        value = value * 10u + static_cast<uint64_t>(c - '0');
        if (value > UINT32_MAX) return false;
    }
    if (value == 0) return false;
    out = static_cast<uint32_t>(value);
    return true;
}

void makeRunPaths(uint32_t runId, char* full, size_t fullCap,
                  char* rel, size_t relCap) {
    snprintf(full, fullCap, "/littlefs/runs/%lu",
             static_cast<unsigned long>(runId));
    snprintf(rel, relCap, "/runs/%lu", static_cast<unsigned long>(runId));
}

void makeStagingPaths(uint32_t runId, char* full, size_t fullCap,
                      char* rel, size_t relCap) {
    snprintf(full, fullCap, "/littlefs/runs/.creating-%lu",
             static_cast<unsigned long>(runId));
    snprintf(rel, relCap, "/runs/.creating-%lu",
             static_cast<unsigned long>(runId));
}

void makeDeletingPaths(uint32_t runId, char* full, size_t fullCap,
                       char* rel, size_t relCap) {
    snprintf(full, fullCap, "/littlefs/runs/.deleting-%lu",
             static_cast<unsigned long>(runId));
    snprintf(rel, relCap, "/runs/.deleting-%lu",
             static_cast<unsigned long>(runId));
}

void joinFile(const char* dir, const char* name, char* out, size_t cap) {
    snprintf(out, cap, "%s/%s", dir, name);
}

bool isDirectory(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

bool readAtExact(int fd, size_t offset, void* out, size_t bytes) {
    if (fd < 0) return false;
    if (lseek(fd, static_cast<off_t>(offset), SEEK_SET) < 0) return false;
    uint8_t* p = static_cast<uint8_t*>(out);
    size_t done = 0;
    while (done < bytes) {
        const ssize_t n = read(fd, p + done, bytes - done);
        if (n <= 0) return false;
        done += static_cast<size_t>(n);
    }
    return true;
}

bool writeAll(int fd, const void* data, size_t bytes) {
    if (fd < 0) return false;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    size_t done = 0;
    while (done < bytes) {
        const ssize_t n = write(fd, p + done, bytes - done);
        if (n <= 0) return false;
        done += static_cast<size_t>(n);
    }
    return true;
}

bool writeAtExact(int fd, size_t offset, const void* data, size_t bytes) {
    if (fd < 0) return false;
    if (lseek(fd, static_cast<off_t>(offset), SEEK_SET) < 0) return false;
    return writeAll(fd, data, bytes);
}

bool fileSizeOf(int fd, size_t& out) {
    if (fd < 0) return false;
    const off_t end = lseek(fd, 0, SEEK_END);
    if (end < 0) return false;
    out = static_cast<size_t>(end);
    return true;
}

bool pathSize(const char* path, size_t& out) {
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return false;
    out = static_cast<size_t>(st.st_size);
    return true;
}

bool readWholeFile(const char* path, uint8_t* out, size_t capacity,
                   size_t& readBytes) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    size_t size = 0;
    bool ok = fileSizeOf(fd, size) && size <= capacity &&
              readAtExact(fd, 0, out, size);
    if (ok) readBytes = size;
    close(fd);
    return ok;
}

bool writeNewFile(const char* path, const void* data, size_t bytes) {
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (fd < 0) return false;
    bool ok = writeAll(fd, data, bytes);
    if (ok && fsync(fd) != 0) ok = false;
    if (close(fd) != 0) ok = false;
    return ok;
}

bool writeEventsInitialFile(const char* path, const uint8_t* header,
                            size_t headerBytes, const uint8_t* frame,
                            size_t frameBytes) {
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (fd < 0) return false;
    bool ok = writeAll(fd, header, headerBytes) &&
              writeAll(fd, frame, frameBytes);
    if (ok && fsync(fd) != 0) ok = false;
    if (close(fd) != 0) ok = false;
    return ok;
}

bool syncDirectory(const char* path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    errno = 0;
    const int rc = fsync(fd);
    const int saved = errno;
    close(fd);
    if (rc == 0) return true;
    // Some VFS implementations reject fsync on a directory descriptor.
    return saved == EINVAL || saved == ENOTSUP;
}

bool fileEqualsRange(int srcFd, size_t srcOffset, const char* path,
                     size_t bytes) {
    size_t candidateSize = 0;
    if (!pathSize(path, candidateSize) || candidateSize != bytes) return false;
    int other = open(path, O_RDONLY);
    if (other < 0) return false;
    uint8_t a[256];
    uint8_t b[256];
    bool equal = true;
    size_t done = 0;
    while (done < bytes) {
        const size_t chunk = (bytes - done) < sizeof(a) ? (bytes - done)
                                                       : sizeof(a);
        if (!readAtExact(srcFd, srcOffset + done, a, chunk) ||
            !readAtExact(other, done, b, chunk) ||
            memcmp(a, b, chunk) != 0) {
            equal = false;
            break;
        }
        done += chunk;
    }
    close(other);
    return equal;
}

bool removeDirRecursive(const char* path) {
    DIR* dir = opendir(path);
    if (dir == nullptr) return errno == ENOENT;
    bool ok = true;
    struct dirent* entry = nullptr;
    while ((entry = readdir(dir)) != nullptr) {
        const char* name = entry->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        char child[160];
        snprintf(child, sizeof(child), "%s/%s", path, name);
        struct stat st;
        if (stat(child, &st) != 0) {
            ok = false;
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (!removeDirRecursive(child)) ok = false;
        } else if (unlink(child) != 0) {
            ok = false;
        }
    }
    closedir(dir);
    if (rmdir(path) != 0) ok = false;
    return ok;
}

bool directoryTotalSize(const char* path, uint32_t& total) {
    DIR* dir = opendir(path);
    if (dir == nullptr) return false;
    uint64_t sum = 0;
    struct dirent* entry = nullptr;
    while ((entry = readdir(dir)) != nullptr) {
        const char* name = entry->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        char child[160];
        snprintf(child, sizeof(child), "%s/%s", path, name);
        struct stat st;
        if (stat(child, &st) == 0 && S_ISREG(st.st_mode)) {
            sum += static_cast<uint64_t>(st.st_size);
        }
    }
    closedir(dir);
    total = sum > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(sum);
    return true;
}

bool isTailBackupName(const char* name) {
    static const char kPrefix[] = "events-tail-";
    static const char kSuffix[] = ".bin";
    const size_t nameLen = strlen(name);
    const size_t prefixLen = sizeof(kPrefix) - 1;
    const size_t suffixLen = sizeof(kSuffix) - 1;
    if (nameLen <= prefixLen + suffixLen) return false;
    if (strncmp(name, kPrefix, prefixLen) != 0) return false;
    if (strcmp(name + nameLen - suffixLen, kSuffix) != 0) return false;
    return true;
}

uint32_t tailBackupsSize(const char* path) {
    DIR* dir = opendir(path);
    if (dir == nullptr) return 0;
    uint64_t sum = 0;
    struct dirent* entry = nullptr;
    while ((entry = readdir(dir)) != nullptr) {
        if (!isTailBackupName(entry->d_name)) continue;
        char child[160];
        snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        struct stat st;
        if (stat(child, &st) == 0 && S_ISREG(st.st_mode)) {
            sum += static_cast<uint64_t>(st.st_size);
        }
    }
    closedir(dir);
    return sum > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(sum);
}

template <typename Fn>
bool forEachRunDir(Fn fn) {
    DIR* dir = opendir("/littlefs/runs");
    if (dir == nullptr) return false;
    struct dirent* entry = nullptr;
    while ((entry = readdir(dir)) != nullptr) {
        const char* name = entry->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        uint32_t id = 0;
        bool isFinal = false;
        bool isDeleting = false;
        if (strncmp(name, ".creating-", 10) == 0) {
            if (!parseDecimalId(name + 10, strlen(name + 10), id)) continue;
        } else if (strncmp(name, ".deleting-", 10) == 0) {
            if (!parseDecimalId(name + 10, strlen(name + 10), id)) continue;
            isDeleting = true;
        } else if (parseDecimalId(name, strlen(name), id)) {
            isFinal = true;
        } else {
            continue;
        }
        char full[128];
        char rel[96];
        snprintf(full, sizeof(full), "/littlefs/runs/%s", name);
        snprintf(rel, sizeof(rel), "/runs/%s", name);
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        fn(id, full, rel, isFinal, isDeleting);
    }
    closedir(dir);
    return true;
}

bool resolveRunDir(uint32_t runId, char* full, size_t fullCap, char* rel,
                   size_t relCap) {
    makeRunPaths(runId, full, fullCap, rel, relCap);
    if (isDirectory(full)) return true;
    makeStagingPaths(runId, full, fullCap, rel, relCap);
    if (isDirectory(full)) return true;
    return false;
}

enum class EventKind : uint8_t {
    Started,
    Ended,
    MarkerCreated,
    MarkerUpdated,
    MarkerDeleted,
    Unknown
};

const char* statusLiteral(RunStatus status) {
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

bool parseStatus(const char* value, RunStatus& out) {
    if (value == nullptr) return false;
    if (strcmp(value, "active") == 0) out = RunStatus::Active;
    else if (strcmp(value, "complete") == 0) out = RunStatus::Complete;
    else if (strcmp(value, "interrupted") == 0) out = RunStatus::Interrupted;
    else if (strcmp(value, "duration_limit") == 0) out = RunStatus::DurationLimit;
    else if (strcmp(value, "storage_error") == 0) out = RunStatus::StorageError;
    else if (strcmp(value, "corrupt") == 0) out = RunStatus::Corrupt;
    else return false;
    return true;
}

bool parseErrorName(const char* value, ArchiveError& out) {
    if (value == nullptr) {
        out = ArchiveError::NONE;
        return true;
    }
    for (uint8_t i = 0; i <= static_cast<uint8_t>(ArchiveError::RUN_ID_EXHAUSTED);
         ++i) {
        const ArchiveError candidate = static_cast<ArchiveError>(i);
        const char* name = archiveErrorName(candidate);
        if (name != nullptr && strcmp(name, value) == 0) {
            out = candidate;
            return true;
        }
    }
    return false;
}

bool parseTimeBasis(const char* value, TimeBasis& out) {
    if (value == nullptr) return false;
    if (strcmp(value, "estimated") == 0) out = TimeBasis::Estimated;
    else if (strcmp(value, "manual") == 0) out = TimeBasis::Manual;
    else if (strcmp(value, "untimed") == 0) out = TimeBasis::Untimed;
    else return false;
    return true;
}

bool eventIdentity(const uint8_t* json, size_t bytes, uint32_t& revision,
                   EventKind& kind) {
    if (json == nullptr || bytes == 0 || memchr(json, 0, bytes) != nullptr) {
        return false;
    }
    JsonDocument doc;
    const DeserializationError error =
        deserializeJson(doc, reinterpret_cast<const char*>(json), bytes);
    if (error != DeserializationError::Ok || !doc.is<JsonObject>()) {
        return false;
    }
    if (!doc["revision"].is<uint32_t>()) return false;
    revision = doc["revision"].as<uint32_t>();
    if (revision == 0) return false;
    const char* type = doc["type"].as<const char*>();
    if (type == nullptr) return false;
    if (strcmp(type, "run.started") == 0) kind = EventKind::Started;
    else if (strcmp(type, "run.ended") == 0) kind = EventKind::Ended;
    else if (strcmp(type, "marker.created") == 0) kind = EventKind::MarkerCreated;
    else if (strcmp(type, "marker.updated") == 0) kind = EventKind::MarkerUpdated;
    else if (strcmp(type, "marker.deleted") == 0) kind = EventKind::MarkerDeleted;
    else return false;
    return true;
}

bool validateEventForAppend(const uint8_t* json, size_t bytes,
                            uint32_t& revision, EventKind& kind) {
    if (!eventIdentity(json, bytes, revision, kind)) return false;
    JsonDocument doc;
    if (deserializeJson(doc, reinterpret_cast<const char*>(json), bytes) !=
        DeserializationError::Ok) {
        return false;
    }
    if (kind == EventKind::Started || kind == EventKind::Ended) {
        if (!doc["requestId"].is<const char*>()) return false;
        if (!doc["ms"].is<uint32_t>()) return false;
        if (!doc["droppedAcquired"].is<uint32_t>()) return false;
        const char* status = doc["status"].as<const char*>();
        RunStatus parsed = RunStatus::Interrupted;
        if (!parseStatus(status, parsed)) return false;
        if (kind == EventKind::Started) {
            if (revision != 1 || parsed != RunStatus::Active) return false;
            if (!doc["errorCode"].isNull()) return false;
            if (doc["droppedAcquired"].as<uint32_t>() != 0) return false;
        } else if (parsed == RunStatus::Active || parsed == RunStatus::Corrupt) {
            return false;
        }
        if (doc["errorCode"].isNull()) {
            // null is the healthy success case
        } else if (doc["errorCode"].is<const char*>()) {
            ArchiveError ignored = ArchiveError::NONE;
            if (!parseErrorName(doc["errorCode"].as<const char*>(), ignored)) {
                return false;
            }
        } else {
            return false;
        }
        return true;
    }
    // Marker entries.
    const char* operationId = doc["operationId"].as<const char*>();
    const char* id = doc["id"].as<const char*>();
    if (operationId == nullptr || id == nullptr) return false;
    if (strlen(operationId) == 0 || strlen(operationId) > 80) return false;
    if (strlen(id) == 0 || strlen(id) > 80) return false;
    if (!doc["baseRevision"].is<uint32_t>()) return false;
    if (kind == EventKind::MarkerCreated &&
        doc["baseRevision"].as<uint32_t>() != 0) {
        return false;
    }
    if (kind != EventKind::MarkerDeleted) {
        if (!doc["note"].is<const char*>()) return false;
        TimeBasis basis = TimeBasis::Untimed;
        if (!parseTimeBasis(doc["timeBasis"].as<const char*>(), basis)) {
            return false;
        }
        if (!doc["ms"].isNull() && !doc["ms"].is<uint32_t>()) return false;
        if (basis == TimeBasis::Untimed && !doc["ms"].isNull()) return false;
    }
    return true;
}

struct RawInfo {
    bool headerOk{};
    uint32_t count{};
    uint32_t lastAcqSeq{};
    uint32_t lastMs{};
    uint32_t acquisitionGaps{};
    bool terminalSeen{};
    size_t validBytes{};
    size_t fileBytes{};
    rawlogcodec::ScanCursor cursor{};
    bool done{};
};

bool scanRawFile(const char* dirFull, uint32_t expectedRunId, RawInfo& out,
                 size_t maxRecords = SIZE_MAX, bool resume = false) {
    if (!resume) out = RawInfo{};
    if (out.done) return true;
    char path[160];
    joinFile(dirFull, "samples.bin", path, sizeof(path));
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    size_t fileBytes = 0;
    if (!fileSizeOf(fd, fileBytes) || fileBytes < rawlogcodec::kHeaderSize) {
        close(fd);
        return false;
    }
    uint8_t headerBytes[rawlogcodec::kHeaderSize];
    rawlogcodec::Header header;
    if (!readAtExact(fd, 0, headerBytes, sizeof(headerBytes)) ||
        !rawlogcodec::decodeHeader(headerBytes, sizeof(headerBytes), header) ||
        header.runId != expectedRunId) {
        close(fd);
        return false;
    }
    out.headerOk = true;
    out.fileBytes = fileBytes;

    size_t offset = out.validBytes ? out.validBytes : rawlogcodec::kHeaderSize;
    uint8_t buffer[kRecordBytes * 64u];
    size_t remaining = maxRecords;
    while (remaining && offset + kRecordBytes <= fileBytes) {
        size_t count = (fileBytes - offset) / kRecordBytes;
        if (count > 64u) count = 64u;
        if (count > remaining) count = remaining;
        const size_t bytes = count * kRecordBytes;
        if (!readAtExact(fd, offset, buffer, bytes)) {
            close(fd);
            return false;
        }
        const auto validated = rawlogcodec::scanRecords(buffer, bytes, out.cursor);
        for (size_t i = 0; i < validated.validCount; ++i)
            if (buffer[i * kRecordBytes + rawlogcodec::kRecordFlagsOffset] & runlog::kAcquisitionGap)
                ++out.acquisitionGaps;
        out.count += validated.validCount;
        out.lastAcqSeq = out.cursor.lastAcqSeq;
        out.lastMs = out.cursor.lastMs;
        out.terminalSeen = out.cursor.terminalSeen;
        offset += validated.validBytes;
        remaining -= count;
        if (validated.validCount != count || out.terminalSeen) {
            out.done = true;
            break;
        }
    }
    out.done = out.done || offset + kRecordBytes > fileBytes;
    out.validBytes = offset;
    close(fd);
    return true;
}

struct EventInfo {
    bool headerOk{};
    uint32_t revision{};
    uint32_t annotationCount{};
    bool started{};
    bool ended{};
    RunStatus terminalStatus{RunStatus::Interrupted};
    bool hasEndMs{};
    uint32_t endMs{};
    uint32_t droppedAcquired{};
    ArchiveError terminalError{ArchiveError::NONE};
    size_t validBytes{};
    size_t fileBytes{};
    size_t lastFrameOffset{};
    size_t lastPayloadOffset{};
    size_t lastPayloadBytes{};
    bool done{};
};

ArchiveError scanEventsFile(const char* dirFull, uint32_t expectedRunId,
                            uint8_t* scratch, size_t scratchBytes,
                            EventInfo& out, uint32_t maxFrames = UINT32_MAX,
                            bool resume = false) {
    if (!resume) out = EventInfo{};
    if (out.done) return ArchiveError::NONE;
    char path[160];
    joinFile(dirFull, "events.bin", path, sizeof(path));
    int fd = open(path, O_RDONLY);
    if (fd < 0) return ArchiveError::CORRUPT_RUN;
    size_t fileBytes = 0;
    if (!fileSizeOf(fd, fileBytes) || fileBytes < eventjournalcodec::kHeaderSize) {
        close(fd);
        return ArchiveError::CORRUPT_RUN;
    }
    uint8_t headerBytes[eventjournalcodec::kHeaderSize];
    eventjournalcodec::Header header;
    if (!readAtExact(fd, 0, headerBytes, sizeof(headerBytes)) ||
        !eventjournalcodec::decodeHeader(headerBytes, sizeof(headerBytes),
                                         header) ||
        header.runId != expectedRunId) {
        close(fd);
        return ArchiveError::CORRUPT_RUN;
    }
    out.headerOk = true;
    out.fileBytes = fileBytes;

    size_t offset = out.validBytes ? out.validBytes : eventjournalcodec::kHeaderSize;
    uint32_t expectedRevision = out.revision + 1;
    uint32_t frames = 0;
    while (frames < maxFrames && offset + eventjournalcodec::kFrameOverheadBytes <= fileBytes) {
        uint8_t lengthBytes[4];
        if (!readAtExact(fd, offset, lengthBytes, sizeof(lengthBytes))) break;
        const uint32_t payloadBytes = static_cast<uint32_t>(lengthBytes[0]) |
                                      (static_cast<uint32_t>(lengthBytes[1]) << 8) |
                                      (static_cast<uint32_t>(lengthBytes[2]) << 16) |
                                      (static_cast<uint32_t>(lengthBytes[3]) << 24);
        if (payloadBytes < eventjournalcodec::kMinPayloadBytes ||
            payloadBytes > eventjournalcodec::kMaxPayloadBytes) {
            break;
        }
        const size_t frameBytes =
            eventjournalcodec::kFrameOverheadBytes + payloadBytes;
        if (offset + frameBytes > fileBytes || frameBytes > scratchBytes) break;
        if (!readAtExact(fd, offset, scratch, frameBytes)) break;
        eventjournalcodec::FrameView view;
        if (!eventjournalcodec::decodeFrame(scratch, frameBytes, view)) break;

        if (view.payloadBytes == 0 || memchr(view.payload, 0, view.payloadBytes)) {
            break;
        }
        JsonDocument doc;
        if (deserializeJson(doc,
                            reinterpret_cast<const char*>(view.payload),
                            view.payloadBytes) != DeserializationError::Ok ||
            !doc.is<JsonObject>()) {
            break;
        }
        if (!doc["revision"].is<uint32_t>()) break;
        const uint32_t revision = doc["revision"].as<uint32_t>();
        if (revision != expectedRevision) break;
        const char* type = doc["type"].as<const char*>();
        if (type == nullptr) break;

        bool valid = true;
        if (strcmp(type, "run.started") == 0) {
            if (out.started || revision != 1) valid = false;
            if (!doc["requestId"].is<const char*>()) valid = false;
            if (!doc["ms"].is<uint32_t>()) valid = false;
            if (!doc["status"].is<const char*>()) valid = false;
            RunStatus started = RunStatus::Interrupted;
            if (valid && (!parseStatus(doc["status"].as<const char*>(), started) ||
                          started != RunStatus::Active)) {
                valid = false;
            }
            if (valid && !doc["errorCode"].isNull()) valid = false;
            if (valid && (!doc["droppedAcquired"].is<uint32_t>() ||
                          doc["droppedAcquired"].as<uint32_t>() != 0)) {
                valid = false;
            }
            if (valid) out.started = true;
        } else if (strcmp(type, "run.ended") == 0) {
            if (out.ended) valid = false;
            if (!doc["requestId"].is<const char*>()) valid = false;
            RunStatus ended = RunStatus::Interrupted;
            if (valid && (!doc["status"].is<const char*>() ||
                          !parseStatus(doc["status"].as<const char*>(), ended) ||
                          ended == RunStatus::Active ||
                          ended == RunStatus::Corrupt)) {
                valid = false;
            }
            if (valid && !doc["ms"].is<uint32_t>()) valid = false;
            if (valid && !doc["droppedAcquired"].is<uint32_t>()) valid = false;
            ArchiveError terminalError = ArchiveError::NONE;
            if (valid && !doc["errorCode"].isNull()) {
                if (!doc["errorCode"].is<const char*>()) {
                    valid = false;
                } else if (!parseErrorName(
                               doc["errorCode"].as<const char*>(),
                               terminalError)) {
                    valid = false;
                }
            }
            if (valid) {
                out.ended = true;
                out.terminalStatus = ended;
                out.hasEndMs = true;
                out.endMs = doc["ms"].as<uint32_t>();
                out.droppedAcquired = doc["droppedAcquired"].as<uint32_t>();
                out.terminalError = terminalError;
            }
        } else if (strcmp(type, "marker.created") == 0 ||
                   strcmp(type, "marker.updated") == 0 ||
                   strcmp(type, "marker.deleted") == 0) {
            if (!doc["operationId"].is<const char*>() ||
                !doc["id"].is<const char*>()) {
                valid = false;
            }
            if (valid) {
                if (strcmp(type, "marker.created") == 0) {
                    ++out.annotationCount;
                } else if (strcmp(type, "marker.deleted") == 0) {
                    if (out.annotationCount > 0) --out.annotationCount;
                }
            }
        } else {
            valid = false;
        }
        if (!valid) break;

        out.revision = revision;
        out.validBytes = offset + frameBytes;
        out.lastFrameOffset = offset;
        out.lastPayloadOffset = offset + eventjournalcodec::kFramePayloadOffset;
        out.lastPayloadBytes = view.payloadBytes;
        offset += frameBytes;
        ++expectedRevision;
        ++frames;
    }
    out.validBytes = offset;
    out.done = frames < maxFrames ||
        offset + eventjournalcodec::kFrameOverheadBytes > fileBytes;
    close(fd);
    if (out.revision > 0 && !out.started) return ArchiveError::CORRUPT_RUN;
    return ArchiveError::NONE;
}

void makeCorruptMeta(RunMeta& out, uint32_t runId, const char* relPath,
                     uint32_t sizeBytes, uint32_t recoveredTailBytes,
                     const char* deviceId) {
    out = RunMeta{};
    out.corrupt = true;
    out.schemaVersion = runlog::kSchemaVersion;
    if (deviceId != nullptr) boundedCopy(out.deviceId, sizeof(out.deviceId), deviceId);
    out.runId = runId;
    out.status = RunStatus::Corrupt;
    out.rawComplete = false;
    out.error = ArchiveError::CORRUPT_RUN;
    boundedCopy(out.path, sizeof(out.path), relPath != nullptr ? relPath : "");
    out.sizeBytes = sizeBytes;
    out.recoveredTailBytes = recoveredTailBytes;
}

bool parseMetaFile(const char* dirFull, const char* relPath, uint32_t runId,
                   RunMeta& out, char* startRequestId,
                   size_t startRequestIdCapacity, uint8_t* buffer,
                   size_t bufferCapacity) {
    char path[160];
    joinFile(dirFull, "meta.json", path, sizeof(path));
    if (!buffer || bufferCapacity < kMetaReadLimit) return false;
    size_t length = 0;
    if (!readWholeFile(path, buffer, kMetaReadLimit, length)) return false;
    JsonDocument doc;
    if (deserializeJson(doc, reinterpret_cast<const char*>(buffer), length) !=
            DeserializationError::Ok ||
        !doc.is<JsonObject>()) {
        return false;
    }
    if (!doc["schemaVersion"].is<uint32_t>() ||
        doc["schemaVersion"].as<uint32_t>() != runlog::kSchemaVersion) {
        return false;
    }
    if (!doc["runId"].is<uint32_t>() || doc["runId"].as<uint32_t>() != runId) {
        return false;
    }
    const char* deviceId = doc["deviceId"].as<const char*>();
    const char* bootId = doc["bootId"].as<const char*>();
    if (deviceId == nullptr || !validDeviceId(deviceId)) return false;
    if (bootId == nullptr || !validBootId(bootId)) return false;

    RunMeta meta;
    meta.schemaVersion = runlog::kSchemaVersion;
    boundedCopy(meta.deviceId, sizeof(meta.deviceId), deviceId);
    boundedCopy(meta.bootId, sizeof(meta.bootId), bootId);
    meta.runId = runId;
    const char* firmware = doc["firmware"].as<const char*>();
    if (firmware != nullptr) {
        if (!boundedCopy(meta.firmware, sizeof(meta.firmware), firmware)) {
            return false;
        }
    }
    if (!doc["protocolVersion"].is<uint32_t>() ||
        doc["protocolVersion"].as<uint32_t>() == 0) {
        return false;
    }
    meta.protocolVersion = doc["protocolVersion"].as<uint32_t>();
    meta.acquireIntervalMs = doc["acquireIntervalMs"].is<uint32_t>()
                                 ? doc["acquireIntervalMs"].as<uint32_t>()
                                 : runlog::kAcquireIntervalMs;
    meta.logIntervalMs = doc["logIntervalMs"].is<uint32_t>()
                             ? doc["logIntervalMs"].as<uint32_t>()
                             : runlog::kLogIntervalMs;
    meta.graphBucketMs = doc["graphBucketMs"].is<uint32_t>()
                             ? doc["graphBucketMs"].as<uint32_t>()
                             : 5000u;
    meta.maxRunMs = doc["maxRunMs"].is<uint32_t>()
                        ? doc["maxRunMs"].as<uint32_t>()
                        : runlog::kMaxDurationMs;
    meta.startUptimeMs = doc["startUptimeMs"].is<uint32_t>()
                             ? doc["startUptimeMs"].as<uint32_t>()
                             : 0u;
    meta.status = RunStatus::Interrupted;
    meta.wallTimeSource = WallTimeSource::Unavailable;
    const char* wallSource = doc["wallTimeSource"].as<const char*>();
    if (wallSource != nullptr &&
        strcmp(wallSource, "client-estimated") == 0) {
        if (!doc["wallStartUtcMs"].is<uint64_t>()) return false;
        meta.hasWallStartUtcMs = true;
        meta.wallStartUtcMs = doc["wallStartUtcMs"].as<uint64_t>();
        meta.wallTimeSource = WallTimeSource::ClientEstimated;
    } else if (wallSource != nullptr && strcmp(wallSource, "unavailable") == 0) {
        meta.wallTimeSource = WallTimeSource::Unavailable;
    } else if (wallSource != nullptr) {
        return false;
    }
    boundedCopy(meta.path, sizeof(meta.path), relPath != nullptr ? relPath : "");

    if (startRequestId != nullptr && startRequestIdCapacity > 0) {
        const char* request = doc["startRequestId"].as<const char*>();
        if (request == nullptr ||
            !boundedCopy(startRequestId, startRequestIdCapacity, request)) {
            return false;
        }
    }
    out = meta;
    return true;
}

bool probeRun(const char* dirFull, const char* relPath, uint32_t runId,
              bool& corrupt, uint8_t* scratch, size_t scratchBytes) {
    RunMeta base;
    corrupt = !parseMetaFile(dirFull, relPath, runId, base, nullptr, 0,
                            scratch, scratchBytes);
    char path[160];
    if (!corrupt) {
        joinFile(dirFull, "samples.bin", path, sizeof(path));
        int fd = open(path, O_RDONLY);
        rawlogcodec::Header header;
        uint8_t bytes[rawlogcodec::kHeaderSize];
        if (fd < 0 || !readAtExact(fd, 0, bytes, sizeof(bytes)) ||
            !rawlogcodec::decodeHeader(bytes, sizeof(bytes), header) ||
            header.runId != runId) {
            corrupt = true;
        }
        if (fd >= 0) close(fd);
    }
    if (!corrupt) {
        joinFile(dirFull, "events.bin", path, sizeof(path));
        int fd = open(path, O_RDONLY);
        eventjournalcodec::Header header;
        uint8_t bytes[eventjournalcodec::kHeaderSize];
        if (fd < 0 || !readAtExact(fd, 0, bytes, sizeof(bytes)) ||
            !eventjournalcodec::decodeHeader(bytes, sizeof(bytes), header) ||
            header.runId != runId) {
            corrupt = true;
        }
        if (fd >= 0) close(fd);
    }
    return true;
}

void applyPrefixMeta(RunMeta& meta, const RawInfo& raw, const EventInfo& events,
                     uint32_t sizeBytes, uint32_t tailBytes) {
    meta.sampleCount = raw.count;
    meta.durableLogSeq = raw.count;
    meta.latestAcqSeq = raw.lastAcqSeq;
    meta.acquisitionGaps = raw.acquisitionGaps;
    meta.eventRevision = events.revision;
    meta.annotationCount = events.annotationCount;
    meta.status = events.ended ? events.terminalStatus : RunStatus::Interrupted;
    meta.hasEndElapsedMs = events.hasEndMs;
    meta.endElapsedMs = events.endMs;
    meta.droppedAcquired = events.droppedAcquired;
    meta.error = events.terminalError;
    const size_t invalidBytes = raw.fileBytes - raw.validBytes +
                                events.fileBytes - events.validBytes;
    meta.rawComplete = events.ended && events.terminalStatus == RunStatus::Complete &&
        events.terminalError == ArchiveError::NONE && !raw.acquisitionGaps &&
        !events.droppedAcquired && !invalidBytes && !tailBytes;
    meta.sizeBytes = sizeBytes;
    meta.recoveredTailBytes = clampU32(tailBytes + invalidBytes);
}

ArchiveError loadMetaFromDir(const char* dirFull, const char* relPath,
                             uint32_t runId, uint8_t* frameScratch,
                             size_t frameScratchBytes, const char* fallbackDevice,
                             RunMeta& out) {
    RunMeta meta;
    if (!parseMetaFile(dirFull, relPath, runId, meta, nullptr, 0,
                        frameScratch, frameScratchBytes)) {
        uint32_t size = 0;
        uint32_t tail = 0;
        directoryTotalSize(dirFull, size);
        tail = tailBackupsSize(dirFull);
        makeCorruptMeta(out, runId, relPath, size, tail, fallbackDevice);
        return ArchiveError::NONE;
    }

    uint32_t sizeBytes = 0;
    directoryTotalSize(dirFull, sizeBytes);
    const uint32_t tailBytes = tailBackupsSize(dirFull);
    RawInfo raw;
    if (!scanRawFile(dirFull, runId, raw)) {
        makeCorruptMeta(out, runId, relPath, sizeBytes, tailBytes,
                        meta.deviceId);
        return ArchiveError::NONE;
    }
    EventInfo events;
    const ArchiveError eventError =
        scanEventsFile(dirFull, runId, frameScratch, frameScratchBytes, events);
    if (eventError != ArchiveError::NONE) {
        makeCorruptMeta(out, runId, relPath, sizeBytes, tailBytes,
                        meta.deviceId);
        return ArchiveError::NONE;
    }

    applyPrefixMeta(meta, raw, events, sizeBytes, tailBytes);
    out = meta;
    return ArchiveError::NONE;
}

bool buildStartedJson(const RunStart& start, uint8_t* out, size_t capacity,
                      size_t& bytes) {
    JsonDocument doc;
    doc["revision"] = 1u;
    doc["type"] = "run.started";
    doc["requestId"] = start.startRequestId;
    // Lifecycle ms is run-relative and bounded by maxRunMs; the device uptime
    // at start is immutable metadata, not a run timestamp.
    doc["ms"] = 0u;
    doc["status"] = "active";
    doc["errorCode"] = nullptr;
    doc["droppedAcquired"] = 0u;
    const size_t measured = measureJson(doc);
    if (measured + 1u > capacity) return false;
    const size_t written =
        serializeJson(doc, reinterpret_cast<char*>(out), capacity);
    if (written == 0 || written != measured) return false;
    bytes = written;
    return true;
}

bool buildEndedJson(uint32_t revision, RunStatus status, uint32_t endMs,
                    const char* requestId, uint32_t droppedAcquired,
                    ArchiveError cause, uint8_t* out, size_t capacity,
                    size_t& bytes) {
    JsonDocument doc;
    doc["revision"] = revision;
    doc["type"] = "run.ended";
    doc["requestId"] = requestId != nullptr ? requestId : "";
    doc["ms"] = endMs;
    doc["status"] = statusLiteral(status);
    const char* errorName = archiveErrorName(cause);
    if (errorName == nullptr) {
        doc["errorCode"] = nullptr;
    } else {
        doc["errorCode"] = errorName;
    }
    doc["droppedAcquired"] = droppedAcquired;
    const size_t measured = measureJson(doc);
    if (measured + 1u > capacity) return false;
    const size_t written =
        serializeJson(doc, reinterpret_cast<char*>(out), capacity);
    if (written == 0 || written != measured) return false;
    bytes = written;
    return true;
}

}  // namespace

struct RunArchiveStore::Impl {
    bool mounted{};
    bool archiveReady{};
    char deviceId[13]{};
    char bootId[17]{};

    uint32_t activeRunId{};
    int samplesFd{-1};
    char activeDir[128]{};
    char activeRel[96]{};
    size_t samplesBytes{};
    size_t eventsBytes{};
    size_t metaBytes{};
    uint32_t tailBytes{};
    uint32_t durableLogSeq{};
    uint32_t latestAcqSeq{};
    uint32_t acquisitionGaps{};
    bool terminalSeen{};
    size_t pendingCount{};
    LoggedSample pending[kPendingCapacity]{};
    RunMeta activeMeta{};
    ArchiveError activeError{ArchiveError::NONE};

    uint8_t* journalCache{};
    uint8_t* frameScratch{};
    uint32_t* eventOffsets{};
    RunMeta* metaCache{};
    size_t nextMetaCache{};
    struct ReadPreparation {
        uint32_t runId{};
        char dir[128]{};
        char rel[96]{};
        RunMeta meta{};
        RawInfo raw{};
        EventInfo events{};
    } read;
    MarkerReducer reducer;

    enum class WarmPhase : uint8_t { Idle, Reading, Reducing, Ready, Error };
    WarmPhase warmPhase{WarmPhase::Idle};
    uint32_t warmRunId{};
    ArchiveError warmError{ArchiveError::NONE};
    size_t warmCacheBytes{};
    size_t warmReadOffset{};
    size_t warmFileSize{};
    size_t warmReducerBytes{};
    uint32_t warmExpectedRevision{1};
    bool warmStarted{};
    bool warmEnded{};
    bool warmTail{};

    ~Impl() {
        if (samplesFd >= 0) close(samplesFd);
        if (journalCache != nullptr) heap_caps_free(journalCache);
        if (frameScratch != nullptr) heap_caps_free(frameScratch);
        if (eventOffsets) heap_caps_free(eventOffsets);
        if (metaCache) heap_caps_free(metaCache);
    }
};

namespace {

RunMeta* cachedMeta(RunArchiveStore::Impl& st, uint32_t runId) {
    if (!st.metaCache) return nullptr;
    for (size_t i = 0; i < 16; ++i)
        if (st.metaCache[i].runId == runId) return &st.metaCache[i];
    return nullptr;
}

void cacheMeta(RunArchiveStore::Impl& st, const RunMeta& meta) {
    if (!st.metaCache) return;
    RunMeta* entry = cachedMeta(st, meta.runId);
    if (!entry) {
        entry = st.metaCache + st.nextMetaCache;
        st.nextMetaCache = (st.nextMetaCache + 1) % 16;
    }
    *entry = meta;
}

void invalidateMeta(RunArchiveStore::Impl& st, uint32_t runId) {
    if (RunMeta* entry = cachedMeta(st, runId)) entry->runId = 0;
    if (st.read.runId == runId) st.read.runId = 0;
}

uint32_t remainingReservationInternal(const RunArchiveStore::Impl& st) {
    if (!st.archiveReady || st.activeRunId == 0) return 0;
    const uint32_t rawAllocated = roundUpSector(
        st.samplesBytes + st.pendingCount * kRecordBytes);
    const uint32_t eventAllocated = roundUpSector(st.eventsBytes);
    const uint32_t metaAllocated = roundUpSector(
        st.metaBytes + rawlogcodec::kHeaderSize +
        eventjournalcodec::kHeaderSize);
    const uint64_t rawRemaining =
        rawAllocated < kRawReservationBytes ? kRawReservationBytes - rawAllocated
                                            : 0u;
    const uint64_t eventRemaining =
        eventAllocated < kEventBudgetBytes ? kEventBudgetBytes - eventAllocated
                                           : 0u;
    const uint64_t metaRemaining =
        metaAllocated < kMetaAllowanceBytes ? kMetaAllowanceBytes - metaAllocated
                                            : 0u;
    const uint64_t total = rawRemaining + eventRemaining + metaRemaining;
    return total > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(total);
}

void resetWarm(RunArchiveStore::Impl& st) {
    st.warmPhase = RunArchiveStore::Impl::WarmPhase::Idle;
    st.warmRunId = 0;
    st.warmError = ArchiveError::NONE;
    st.warmCacheBytes = 0;
    st.warmReadOffset = 0;
    st.warmFileSize = 0;
    st.warmReducerBytes = 0;
    st.warmExpectedRevision = 1;
    st.warmStarted = false;
    st.warmEnded = false;
    st.warmTail = false;
}

ArchiveError startWarm(RunArchiveStore::Impl& st, uint32_t runId) {
    char dir[128];
    char rel[96];
    if (!resolveRunDir(runId, dir, sizeof(dir), rel, sizeof(rel))) {
        return ArchiveError::RUN_NOT_FOUND;
    }
    char path[160];
    joinFile(dir, "events.bin", path, sizeof(path));
    int fd = open(path, O_RDONLY);
    if (fd < 0) return ArchiveError::CORRUPT_RUN;
    size_t size = 0;
    if (!fileSizeOf(fd, size) || size < eventjournalcodec::kHeaderSize) {
        close(fd);
        return ArchiveError::CORRUPT_RUN;
    }
    uint8_t headerBytes[eventjournalcodec::kHeaderSize];
    eventjournalcodec::Header header;
    if (!readAtExact(fd, 0, headerBytes, sizeof(headerBytes)) ||
        !eventjournalcodec::decodeHeader(headerBytes, sizeof(headerBytes),
                                         header) ||
        header.runId != runId) {
        close(fd);
        return ArchiveError::CORRUPT_RUN;
    }
    close(fd);
    memcpy(st.journalCache, headerBytes, sizeof(headerBytes));
    st.warmRunId = runId;
    st.warmPhase = RunArchiveStore::Impl::WarmPhase::Reading;
    st.warmCacheBytes = eventjournalcodec::kHeaderSize;
    st.warmReadOffset = eventjournalcodec::kHeaderSize;
    st.warmFileSize = size;
    st.warmReducerBytes = 0;
    st.warmExpectedRevision = 1;
    st.warmStarted = false;
    st.warmEnded = false;
    st.warmTail = false;
    st.warmError = ArchiveError::NONE;
    return ArchiveError::NONE;
}

ArchiveError preserveInvalidTail(RunArchiveStore::Impl& st, int fd,
                                 uint32_t runId, const char* dirFull,
                                 size_t validBytes, size_t fileBytes);

bool warmReduceStep(RunArchiveStore::Impl& st, ArchiveError& error) {
    if (st.warmReducerBytes == 0) {
        st.reducer.reset(st.journalCache, st.warmCacheBytes);
        st.warmReducerBytes = st.warmCacheBytes;
    } else if (st.warmReducerBytes != st.warmCacheBytes) {
        const ArchiveError extendError =
            st.reducer.extendValidatedPrefix(st.warmCacheBytes);
        if (extendError != ArchiveError::NONE) {
            st.warmPhase = RunArchiveStore::Impl::WarmPhase::Error;
            st.warmError = extendError;
            error = extendError;
            return true;
        }
        st.warmReducerBytes = st.warmCacheBytes;
    }
    const ArchiveError ingestError =
        st.reducer.ingestNext(kWarmMaxFramesPerStep);
    if (ingestError != ArchiveError::NONE) {
        st.warmPhase = RunArchiveStore::Impl::WarmPhase::Error;
        st.warmError = ingestError;
        error = ingestError;
        return true;
    }
    if (!st.reducer.ready()) return false;

    char dir[128];
    char rel[96];
    if (!resolveRunDir(st.warmRunId, dir, sizeof(dir), rel, sizeof(rel))) {
        st.warmPhase = RunArchiveStore::Impl::WarmPhase::Error;
        st.warmError = ArchiveError::RUN_NOT_FOUND;
        error = st.warmError;
        return true;
    }
    char path[160];
    joinFile(dir, "events.bin", path, sizeof(path));
    size_t size = 0;
    if (!pathSize(path, size)) {
        st.warmPhase = RunArchiveStore::Impl::WarmPhase::Error;
        st.warmError = ArchiveError::STORAGE_UNAVAILABLE;
        error = st.warmError;
        return true;
    }
    if (size < st.warmFileSize) {
        resetWarm(st);
        return false;
    }
    if (!st.warmTail && size > st.warmFileSize) {
        st.warmFileSize = size;
        st.warmPhase = RunArchiveStore::Impl::WarmPhase::Reading;
        return false;
    }
    int fd = open(path, O_RDWR);
    if (fd < 0) {
        resetWarm(st);
        error = ArchiveError::WRITE_FAILED;
        return true;
    }
    if (st.warmTail) {
        const ArchiveError repair = preserveInvalidTail(
            st, fd, st.warmRunId, dir, st.warmCacheBytes, size);
        if (repair != ArchiveError::NONE) {
            close(fd);
            resetWarm(st);
            error = repair;
            return true;
        }
        st.warmFileSize = st.warmCacheBytes;
        st.warmTail = false;
    }
    const bool synced = fsync(fd) == 0;
    close(fd);
    if (!synced) {
        resetWarm(st);
        error = ArchiveError::WRITE_FAILED;
        return true;
    }
    if (st.activeRunId == st.warmRunId) {
        st.activeMeta.eventRevision = st.reducer.revision();
        st.activeMeta.annotationCount = st.reducer.annotationCount();
        st.eventsBytes = st.warmCacheBytes;
    }
    st.warmPhase = RunArchiveStore::Impl::WarmPhase::Ready;
    st.warmError = ArchiveError::NONE;
    error = ArchiveError::NONE;
    return true;
}

ArchiveError preserveInvalidTail(RunArchiveStore::Impl& st, int fd,
                                 uint32_t runId, const char* dirFull,
                                 size_t validBytes, size_t fileBytes) {
    if (fileBytes <= validBytes) return ArchiveError::NONE;
    const size_t suffixBytes = fileBytes - validBytes;

    char name[64];
    char candidate[160];
    bool found = false;
    bool reused = false;
    for (size_t variant = 0; variant < kDirScanOverflowVariants; ++variant) {
        if (variant == 0) {
            snprintf(name, sizeof(name), "events-tail-%lu.bin",
                     static_cast<unsigned long>(validBytes));
        } else {
            snprintf(name, sizeof(name), "events-tail-%lu-%lu.bin",
                     static_cast<unsigned long>(validBytes),
                     static_cast<unsigned long>(variant + 1u));
        }
        joinFile(dirFull, name, candidate, sizeof(candidate));
        struct stat stBuf;
        if (stat(candidate, &stBuf) == 0) {
            if (fileEqualsRange(fd, validBytes, candidate, suffixBytes)) {
                found = true;
                reused = true;
                break;
            }
            continue;
        }
        if (errno != ENOENT) return ArchiveError::STORAGE_UNAVAILABLE;
        found = true;
        reused = false;
        break;
    }
    if (!found) return ArchiveError::WRITE_FAILED;

    if (!reused) {
        const uint32_t activeReserve =
            (st.activeRunId != 0) ? remainingReservationInternal(st) : 0;
        const uint64_t required = static_cast<uint64_t>(suffixBytes) +
                                  kFsHeadroomBytes + activeReserve;
        if (static_cast<uint64_t>(freeFsBytes()) < required) {
            return ArchiveError::INSUFFICIENT_STORAGE;
        }
        int out = open(candidate, O_WRONLY | O_CREAT | O_EXCL, 0666);
        if (out < 0) return ArchiveError::WRITE_FAILED;
        uint8_t buffer[512];
        bool ok = true;
        size_t done = 0;
        while (done < suffixBytes) {
            const size_t chunk = (suffixBytes - done) < sizeof(buffer)
                                     ? (suffixBytes - done)
                                     : sizeof(buffer);
            if (!readAtExact(fd, validBytes + done, buffer, chunk) ||
                !writeAll(out, buffer, chunk)) {
                ok = false;
                break;
            }
            done += chunk;
        }
        if (ok && fsync(out) != 0) ok = false;
        if (close(out) != 0) ok = false;
        if (!ok) {
            unlink(candidate);
            return ArchiveError::WRITE_FAILED;
        }
    }

    if (ftruncate(fd, static_cast<off_t>(validBytes)) != 0) {
        return ArchiveError::WRITE_FAILED;
    }
    if (fsync(fd) != 0) return ArchiveError::WRITE_FAILED;
    if (st.activeRunId == runId) {
        st.eventsBytes = validBytes;
        st.tailBytes = tailBackupsSize(dirFull);
        st.activeMeta.recoveredTailBytes = st.tailBytes;
        st.activeMeta.sizeBytes = static_cast<uint32_t>(
            st.metaBytes + st.samplesBytes + st.eventsBytes + st.tailBytes);
    }
    return ArchiveError::NONE;
}

ArchiveError appendEventInternal(RunArchiveStore::Impl& st, uint32_t runId,
                                 const uint8_t* json, size_t bytes,
                                 uint32_t& durableRevision) {
    durableRevision = 0;
    if (json == nullptr || bytes == 0) return ArchiveError::INVALID_REQUEST;
    if (bytes > eventjournalcodec::kMaxPayloadBytes) {
        return ArchiveError::BODY_TOO_LARGE;
    }
    uint32_t revision = 0;
    EventKind kind = EventKind::Unknown;
    if (!validateEventForAppend(json, bytes, revision, kind)) {
        return ArchiveError::INVALID_REQUEST;
    }
    char dir[128];
    char rel[96];
    if (!resolveRunDir(runId, dir, sizeof(dir), rel, sizeof(rel))) {
        return ArchiveError::RUN_NOT_FOUND;
    }
    char path[160];
    joinFile(dir, "events.bin", path, sizeof(path));
    int fd = open(path, O_RDWR);
    if (fd < 0) return ArchiveError::CORRUPT_RUN;

    EventInfo scan;
    const ArchiveError scanError =
        scanEventsFile(dir, runId, st.frameScratch, kFrameScratchBytes, scan);
    if (scanError != ArchiveError::NONE) {
        close(fd);
        return scanError;
    }
    if (scan.validBytes < scan.fileBytes) {
        const ArchiveError tailError = preserveInvalidTail(
            st, fd, runId, dir, scan.validBytes, scan.fileBytes);
        if (tailError != ArchiveError::NONE) {
            close(fd);
            return tailError;
        }
        scan.fileBytes = scan.validBytes;
    }
    // Recover a fully written but previously unacknowledged frame before
    // another append or any replay can claim persistence.
    if (fsync(fd) != 0) {
        close(fd);
        resetWarm(st);
        return ArchiveError::WRITE_FAILED;
    }

    if (revision == scan.revision) {
        bool identical = false;
        if (scan.lastPayloadBytes == bytes && scan.lastPayloadBytes > 0 &&
            bytes <= kFrameScratchBytes &&
            readAtExact(fd, scan.lastPayloadOffset, st.frameScratch, bytes)) {
            identical = memcmp(st.frameScratch, json, bytes) == 0;
        }
        const bool synced = fsync(fd) == 0;
        close(fd);
        if (!identical) return ArchiveError::REVISION_CONFLICT;
        if (!synced) return ArchiveError::WRITE_FAILED;
        durableRevision = revision;
        invalidateMeta(st, runId);
        if (st.activeRunId == runId) {
            st.activeMeta.eventRevision = revision;
            st.activeMeta.annotationCount = scan.annotationCount;
        }
        return ArchiveError::NONE;
    }
    if (revision < scan.revision) {
        close(fd);
        return ArchiveError::REVISION_CONFLICT;
    }
    if (revision != scan.revision + 1u) {
        close(fd);
        return ArchiveError::INVALID_REQUEST;
    }
    if (kind == EventKind::Started) {
        if (revision != 1 || scan.started) {
            close(fd);
            return ArchiveError::INVALID_REQUEST;
        }
    } else {
        if (!scan.started) {
            close(fd);
            return ArchiveError::CORRUPT_RUN;
        }
        if (kind == EventKind::Ended && scan.ended) {
            close(fd);
            return ArchiveError::INVALID_REQUEST;
        }
    }

    const size_t frameBytes = eventjournalcodec::kFrameOverheadBytes + bytes;
    const uint64_t newSize = static_cast<uint64_t>(scan.validBytes) + frameBytes;
    const bool lifecycle =
        kind == EventKind::Started || kind == EventKind::Ended;
    const uint64_t eventLimit =
        lifecycle ? kEventBudgetBytes : kEventBudgetBytes - kLifecycleReserveBytes;
    if (newSize > eventLimit) {
        close(fd);
        return ArchiveError::EVENT_BUDGET_EXHAUSTED;
    }

    const uint32_t otherReserve =
        (st.activeRunId != 0 && st.activeRunId != runId)
            ? remainingReservationInternal(st)
            : 0;
    const uint32_t freeBytes = freeFsBytes();
    const uint64_t required = static_cast<uint64_t>(frameBytes) +
                              kFsHeadroomBytes + otherReserve;
    if (static_cast<uint64_t>(freeBytes) < required) {
        close(fd);
        return ArchiveError::INSUFFICIENT_STORAGE;
    }

    const size_t encoded = eventjournalcodec::encodeFrame(
        st.frameScratch, kFrameScratchBytes, json, bytes);
    if (encoded == 0 || encoded != frameBytes) {
        close(fd);
        return ArchiveError::INVALID_REQUEST;
    }
    if (!writeAtExact(fd, scan.validBytes, st.frameScratch, encoded) ||
        fsync(fd) != 0) {
        close(fd);
        resetWarm(st);
        return ArchiveError::WRITE_FAILED;
    }
    close(fd);

    durableRevision = revision;
    invalidateMeta(st, runId);
    if (st.activeRunId == runId) {
        st.activeMeta.eventRevision = revision;
        int64_t annotations = static_cast<int64_t>(scan.annotationCount);
        if (kind == EventKind::MarkerCreated) ++annotations;
        else if (kind == EventKind::MarkerDeleted && annotations > 0) --annotations;
        if (annotations < 0) annotations = 0;
        st.activeMeta.annotationCount = static_cast<uint32_t>(annotations);
        st.eventsBytes = scan.validBytes + frameBytes;
        st.activeMeta.sizeBytes = static_cast<uint32_t>(
            st.metaBytes + st.samplesBytes + st.eventsBytes + st.tailBytes);
    }
    if (st.warmRunId == runId) resetWarm(st);
    return ArchiveError::NONE;
}

ArchiveError syncInternal(RunArchiveStore::Impl& st, uint32_t runId) {
    if (st.activeRunId != runId) {
        char dir[128];
        char rel[96];
        return resolveRunDir(runId, dir, sizeof(dir), rel, sizeof(rel))
                   ? ArchiveError::NONE
                   : ArchiveError::RUN_NOT_FOUND;
    }
    if (st.activeError != ArchiveError::NONE) return st.activeError;
    if (st.pendingCount == 0) return ArchiveError::NONE;
    if (st.samplesFd < 0) {
        st.activeError = ArchiveError::WRITE_FAILED;
        st.activeMeta.status = RunStatus::StorageError;
        st.activeMeta.error = st.activeError;
        st.activeMeta.rawComplete = false;
        return st.activeError;
    }
    const uint64_t required =
        static_cast<uint64_t>(st.samplesBytes) +
        static_cast<uint64_t>(st.pendingCount) * kRecordBytes;
    if (required > kRawLogicalMaxBytes) {
        return ArchiveError::INSUFFICIENT_STORAGE;
    }
    uint8_t buffer[kPendingCapacity * kRecordBytes];
    for (size_t i = 0; i < st.pendingCount; ++i) {
        const size_t written = rawlogcodec::encodeRecord(
            buffer + i * kRecordBytes, kRecordBytes, st.pending[i]);
        if (written != kRecordBytes) return ArchiveError::INVALID_REQUEST;
    }
    const size_t bytes = st.pendingCount * kRecordBytes;
    if (!writeAtExact(st.samplesFd, st.samplesBytes, buffer, bytes) ||
        fsync(st.samplesFd) != 0) {
        if (st.samplesFd >= 0) close(st.samplesFd);
        st.samplesFd = -1;
        st.activeError = ArchiveError::WRITE_FAILED;
        st.activeMeta.status = RunStatus::StorageError;
        st.activeMeta.error = st.activeError;
        st.activeMeta.rawComplete = false;
        return st.activeError;
    }
    for (size_t i = 0; i < st.pendingCount; ++i) {
        if ((st.pending[i].flags & runlog::kAcquisitionGap) != 0) {
            ++st.acquisitionGaps;
        }
        if ((st.pending[i].flags & runlog::kTerminal) != 0) {
            st.terminalSeen = true;
        }
    }
    const LoggedSample& last = st.pending[st.pendingCount - 1u];
    st.durableLogSeq = last.logSeq;
    st.latestAcqSeq = last.acqSeq;
    st.samplesBytes += bytes;
    st.pendingCount = 0;
    st.activeMeta.sampleCount = st.durableLogSeq;
    st.activeMeta.durableLogSeq = st.durableLogSeq;
    st.activeMeta.latestAcqSeq = st.latestAcqSeq;
    st.activeMeta.acquisitionGaps = st.acquisitionGaps;
    st.activeMeta.sizeBytes = static_cast<uint32_t>(
        st.metaBytes + st.samplesBytes + st.eventsBytes + st.tailBytes);
    return ArchiveError::NONE;
}

void closeActiveWriter(RunArchiveStore::Impl& st) {
    if (st.samplesFd >= 0) close(st.samplesFd);
    st.samplesFd = -1;
    st.activeRunId = 0;
    st.pendingCount = 0;
}

}  // namespace

RunArchiveStore::RunArchiveStore() : impl_(nullptr) {}

RunArchiveStore::~RunArchiveStore() { delete impl_; }

ArchiveError RunArchiveStore::begin(const char* deviceId,
                                    const char* currentBootId) {
    if (impl_ != nullptr && impl_->archiveReady && impl_->mounted &&
        impl_->activeRunId == 0 && validDeviceId(deviceId) &&
        validBootId(currentBootId) &&
        strcmp(impl_->deviceId, deviceId) == 0 &&
        strcmp(impl_->bootId, currentBootId) == 0) {
        // Main and the service initialization path may both call begin once;
        // an already-mounted, idle store is already fully recovered.
        return ArchiveError::NONE;
    }
    delete impl_;
    impl_ = new (std::nothrow) Impl;
    if (impl_ == nullptr) return ArchiveError::STORAGE_UNAVAILABLE;
    Impl& st = *impl_;
    if (!validDeviceId(deviceId) || !validBootId(currentBootId)) {
        return ArchiveError::INVALID_REQUEST;
    }
    boundedCopy(st.deviceId, sizeof(st.deviceId), deviceId);
    boundedCopy(st.bootId, sizeof(st.bootId), currentBootId);

    st.mounted = LittleFS.begin(false, "/littlefs", 10, "spiffs");
    if (!st.mounted) return ArchiveError::ARCHIVE_UNAVAILABLE;

    if (!isDirectory("/littlefs/runs")) {
        if (mkdir("/littlefs/runs", 0777) != 0 && errno != EEXIST) {
            return ArchiveError::STORAGE_UNAVAILABLE;
        }
    }

    st.journalCache = static_cast<uint8_t*>(heap_caps_malloc(
        kJournalCacheBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    st.frameScratch = static_cast<uint8_t*>(heap_caps_malloc(
        kFrameScratchBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    st.eventOffsets = static_cast<uint32_t*>(heap_caps_malloc(
        4096u * sizeof(uint32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    st.metaCache = static_cast<RunMeta*>(heap_caps_malloc(
        16u * sizeof(RunMeta), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (st.metaCache)
        for (size_t i = 0; i < 16; ++i) new (st.metaCache + i) RunMeta{};
    if (!st.journalCache || !st.frameScratch || !st.eventOffsets || !st.metaCache) {
        return ArchiveError::ARCHIVE_UNAVAILABLE;
    }

    bool enumerated = forEachRunDir(
        [&st](uint32_t id, const char* full, const char* rel, bool isFinal,
              bool isDeleting) {
            (void)st;
            if (isDeleting) {
                removeDirRecursive(full);
                return;
            }
            if (isFinal) return;
            bool corrupt = false;
            probeRun(full, rel, id, corrupt, st.frameScratch, kFrameScratchBytes);
            if (!corrupt) {
                char finalFull[128];
                char finalRel[96];
                makeRunPaths(id, finalFull, sizeof(finalFull), finalRel,
                             sizeof(finalRel));
                if (!isDirectory(finalFull)) rename(full, finalFull);
            }
        });
    if (!enumerated) return ArchiveError::STORAGE_UNAVAILABLE;

    st.archiveReady = true;
    return ArchiveError::NONE;
}

ArchiveError RunArchiveStore::checkStart() const {
    const Impl* st = impl_;
    if (st == nullptr || !st->mounted) return ArchiveError::ARCHIVE_UNAVAILABLE;
    if (!st->archiveReady || st->journalCache == nullptr ||
        st->frameScratch == nullptr) {
        return ArchiveError::ARCHIVE_UNAVAILABLE;
    }
    if (ESP.getFlashChipSize() != 16u * 1024u * 1024u ||
        !esp_spiram_is_initialized() ||
        esp_spiram_get_size() != 8u * 1024u * 1024u) {
        return ArchiveError::ARCHIVE_UNAVAILABLE;
    }
    if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < kJournalCacheBytes) {
        return ArchiveError::ARCHIVE_UNAVAILABLE;
    }
    if (st->activeRunId != 0) return ArchiveError::BUSY;
    if (freeFsBytes() < kStartRequiredBytes) {
        return ArchiveError::INSUFFICIENT_STORAGE;
    }
    return ArchiveError::NONE;
}

StorageStatus RunArchiveStore::storageStatus() const {
    StorageStatus status;
    status.startRequiredBytes = kStartRequiredBytes;
    const Impl* st = impl_;
    if (st == nullptr || !st->mounted) return status;
    status.mounted = true;
    status.totalBytes = clampU32(LittleFS.totalBytes());
    status.usedBytes = clampU32(LittleFS.usedBytes());
    status.freeBytes = freeFsBytes();
    status.reservedRemainingBytes = remainingReservationInternal(*st);
    return status;
}

ArchiveError RunArchiveStore::scanCatalog(CatalogSummary& out) {
    if (impl_ == nullptr || !impl_->archiveReady) {
        return ArchiveError::ARCHIVE_UNAVAILABLE;
    }
    CatalogSummary summary;
    const bool enumerated = forEachRunDir(
        [&summary, this](uint32_t id, const char* full, const char* rel,
                   bool isFinal, bool isDeleting) {
            (void)isFinal;
            if (isDeleting) {
                // A pending explicit deletion still holds an allocated identity.
                if (id > summary.maxRunId) summary.maxRunId = id;
                return;
            }
            ++summary.runCount;
            if (id > summary.maxRunId) summary.maxRunId = id;
            bool corrupt = false;
            probeRun(full, rel, id, corrupt, impl_->frameScratch, kFrameScratchBytes);
            if (corrupt) ++summary.corruptCount;
        });
    if (!enumerated) return ArchiveError::STORAGE_UNAVAILABLE;
    out = summary;
    return ArchiveError::NONE;
}

ArchiveError RunArchiveStore::listRunIds(uint32_t beforeRunId, uint8_t limit,
                                       uint32_t* out, size_t capacity,
                                       uint32_t& nextBeforeRunId) {
    nextBeforeRunId = 0;
    if (!impl_ || !impl_->archiveReady) return ArchiveError::ARCHIVE_UNAVAILABLE;
    if (!limit || limit > 16 || !out || capacity < limit)
        return ArchiveError::INVALID_REQUEST;
    for (size_t i = 0; i < limit; ++i) out[i] = 0;
    size_t selected = 0;
    uint32_t total = 0;
    const bool enumerated = forEachRunDir(
        [&](uint32_t id, const char*, const char*, bool, bool deleting) {
            if (deleting || (beforeRunId && id >= beforeRunId)) return;
            ++total;
            size_t pos = 0;
            while (pos < selected && out[pos] > id) ++pos;
            if (pos >= limit) return;
            if (selected < limit) ++selected;
            for (size_t i = selected - 1; i > pos; --i) out[i] = out[i - 1];
            out[pos] = id;
        });
    if (!enumerated) return ArchiveError::STORAGE_UNAVAILABLE;
    if (total > selected && selected) nextBeforeRunId = out[selected - 1];
    return ArchiveError::NONE;
}

ArchiveError RunArchiveStore::listRuns(uint32_t beforeRunId, uint8_t limit,
                                      RunMeta* out, size_t capacity,
                                      uint32_t& nextBeforeRunId) {
    if (!out || capacity < limit) return ArchiveError::INVALID_REQUEST;
    uint32_t ids[16]{};
    const auto error = listRunIds(beforeRunId, limit, ids, 16, nextBeforeRunId);
    if (error != ArchiveError::NONE) return error;
    for (size_t i = 0; i < limit; ++i) {
        out[i] = RunMeta{};
        if (!ids[i]) continue;
        const auto readError = getMeta(ids[i], out[i]);
        if (readError != ArchiveError::NONE) return readError;
    }
    return ArchiveError::NONE;
}

bool RunArchiveStore::prepareReadStep(uint32_t runId, ArchiveError& error) {
    error = ArchiveError::NONE;
    if (!impl_ || !impl_->archiveReady) {
        error = ArchiveError::ARCHIVE_UNAVAILABLE;
        return true;
    }
    if (!runId) {
        error = ArchiveError::INVALID_REQUEST;
        return true;
    }
    Impl& st = *impl_;
    if (runId == st.activeRunId || cachedMeta(st, runId)) return true;
    auto& read = st.read;
    if (read.runId != runId) {
        read = Impl::ReadPreparation{};
        if (!resolveRunDir(runId, read.dir, sizeof(read.dir), read.rel, sizeof(read.rel))) {
            error = ArchiveError::RUN_NOT_FOUND;
            return true;
        }
        read.runId = runId;
        if (!parseMetaFile(read.dir, read.rel, runId, read.meta, nullptr, 0,
                            st.frameScratch, kFrameScratchBytes)) {
            uint32_t bytes = 0;
            directoryTotalSize(read.dir, bytes);
            makeCorruptMeta(read.meta, runId, read.rel, bytes,
                            tailBackupsSize(read.dir), st.deviceId);
            cacheMeta(st, read.meta);
            return true;
        }
    }
    bool corrupt = false;
    if (!read.raw.done) {
        corrupt = !scanRawFile(read.dir, runId, read.raw, 64, true);
        if (!corrupt) return false;
    } else {
        error = scanEventsFile(read.dir, runId, st.frameScratch,
                              kFrameScratchBytes, read.events, 8, true);
        corrupt = error == ArchiveError::CORRUPT_RUN;
        if (error != ArchiveError::NONE && !corrupt) return true;
        if (!corrupt && !read.events.done) return false;
    }
    uint32_t bytes = 0;
    directoryTotalSize(read.dir, bytes);
    const uint32_t tails = tailBackupsSize(read.dir);
    if (corrupt) makeCorruptMeta(read.meta, runId, read.rel, bytes, tails, st.deviceId);
    else applyPrefixMeta(read.meta, read.raw, read.events, bytes, tails);
    cacheMeta(st, read.meta);
    error = ArchiveError::NONE;
    return true;
}

ArchiveError RunArchiveStore::getMeta(uint32_t runId, RunMeta& out) {
    if (impl_ == nullptr || !impl_->archiveReady) {
        return ArchiveError::ARCHIVE_UNAVAILABLE;
    }
    if (runId == 0) return ArchiveError::INVALID_REQUEST;
    Impl& st = *impl_;
    if (st.activeRunId == runId) {
        out = st.activeMeta;
        return ArchiveError::NONE;
    }
    if (RunMeta* cached = cachedMeta(st, runId)) {
        out = *cached;
        return ArchiveError::NONE;
    }
    char dir[128];
    char rel[96];
    if (!resolveRunDir(runId, dir, sizeof(dir), rel, sizeof(rel))) {
        return ArchiveError::RUN_NOT_FOUND;
    }
    const auto error = loadMetaFromDir(dir, rel, runId, st.frameScratch,
                                      kFrameScratchBytes, st.deviceId, out);
    if (error == ArchiveError::NONE) cacheMeta(st, out);
    return error;
}

ArchiveError RunArchiveStore::findStartRequest(const char* requestId,
                                               bool& found, RunMeta& out) {
    found = false;
    if (impl_ == nullptr || !impl_->archiveReady) {
        return ArchiveError::ARCHIVE_UNAVAILABLE;
    }
    if (!validRequestId(requestId)) return ArchiveError::INVALID_REQUEST;
    Impl& st = *impl_;
    bool matched = false;
    const bool enumerated = forEachRunDir(
        [&](uint32_t id, const char* full, const char* rel, bool isFinal,
            bool isDeleting) {
            (void)isFinal;
            if (matched || isDeleting) return;
            char stored[81];
            RunMeta ignored;
            if (!parseMetaFile(full, rel, id, ignored, stored, sizeof(stored),
                                st.frameScratch, kFrameScratchBytes)) {
                return;
            }
            if (strcmp(stored, requestId) != 0) return;
            matched = true;
            if (id == st.activeRunId) {
                out = st.activeMeta;
            } else {
                loadMetaFromDir(full, rel, id, st.frameScratch,
                                kFrameScratchBytes, st.deviceId, out);
            }
        });
    if (!enumerated) return ArchiveError::STORAGE_UNAVAILABLE;
    found = matched;
    return ArchiveError::NONE;
}

ArchiveError RunArchiveStore::prepareRun(const RunStart& start, RunMeta& out) {
    if (impl_ == nullptr || !impl_->archiveReady) {
        return ArchiveError::ARCHIVE_UNAVAILABLE;
    }
    Impl& st = *impl_;
    const ArchiveError preflight = checkStart();
    if (preflight != ArchiveError::NONE) return preflight;
    if (start.runId == 0) return ArchiveError::INVALID_REQUEST;
    if (!validDeviceId(start.identity.deviceId) ||
        !validBootId(start.identity.bootId)) {
        return ArchiveError::INVALID_REQUEST;
    }
    if (strcmp(start.identity.deviceId, st.deviceId) != 0 ||
        strcmp(start.identity.bootId, st.bootId) != 0) {
        return ArchiveError::DEVICE_ID_MISMATCH;
    }
    if (!validRequestId(start.startRequestId)) {
        return ArchiveError::INVALID_REQUEST;
    }
    if (start.acquireIntervalMs != runlog::kAcquireIntervalMs ||
        start.logIntervalMs != runlog::kLogIntervalMs ||
        start.graphBucketMs != 5000u ||
        start.maxRunMs != runlog::kMaxDurationMs ||
        start.protocolVersion == 0) {
        return ArchiveError::INVALID_REQUEST;
    }
    if (start.hasWallStartUtcMs &&
        start.wallTimeSource != WallTimeSource::ClientEstimated) {
        return ArchiveError::INVALID_REQUEST;
    }
    if (!start.hasWallStartUtcMs &&
        start.wallTimeSource != WallTimeSource::Unavailable) {
        return ArchiveError::INVALID_REQUEST;
    }
    const size_t firmwareLength = boundedLength(start.firmware,
                                                sizeof(start.firmware));
    if (firmwareLength >= sizeof(start.firmware)) {
        return ArchiveError::INVALID_REQUEST;
    }

    char stageFull[128];
    char stageRel[96];
    char finalFull[128];
    char finalRel[96];
    makeStagingPaths(start.runId, stageFull, sizeof(stageFull), stageRel,
                     sizeof(stageRel));
    makeRunPaths(start.runId, finalFull, sizeof(finalFull), finalRel,
                 sizeof(finalRel));
    struct stat stBuf;
    if (stat(stageFull, &stBuf) == 0) return ArchiveError::RUN_ID_CONFLICT;
    if (stat(finalFull, &stBuf) == 0) return ArchiveError::RUN_ID_CONFLICT;
    if (mkdir(stageFull, 0777) != 0) return ArchiveError::STORAGE_UNAVAILABLE;

    JsonDocument metaDoc;
    metaDoc["schemaVersion"] = runlog::kSchemaVersion;
    metaDoc["deviceId"] = start.identity.deviceId;
    metaDoc["bootId"] = start.identity.bootId;
    metaDoc["runId"] = start.runId;
    metaDoc["firmware"] = start.firmware;
    metaDoc["protocolVersion"] = start.protocolVersion;
    metaDoc["acquireIntervalMs"] = start.acquireIntervalMs;
    metaDoc["logIntervalMs"] = start.logIntervalMs;
    metaDoc["graphBucketMs"] = start.graphBucketMs;
    metaDoc["maxRunMs"] = start.maxRunMs;
    metaDoc["startRequestId"] = start.startRequestId;
    metaDoc["startUptimeMs"] = start.startUptimeMs;
    if (start.hasWallStartUtcMs) {
        metaDoc["wallStartUtcMs"] = start.wallStartUtcMs;
        metaDoc["wallTimeSource"] = "client-estimated";
    } else {
        metaDoc["wallStartUtcMs"] = nullptr;
        metaDoc["wallTimeSource"] = "unavailable";
    }
    resetWarm(st);
    uint8_t* metaBuffer = st.journalCache;
    constexpr size_t metaCapacity = 2048;
    const size_t metaMeasured = measureJson(metaDoc);
    if (metaMeasured + 1u > metaCapacity) return ArchiveError::INVALID_REQUEST;
    const size_t metaLength =
        serializeJson(metaDoc, reinterpret_cast<char*>(metaBuffer),
                      metaCapacity);
    if (metaLength == 0 || metaLength != metaMeasured) {
        return ArchiveError::INVALID_REQUEST;
    }

    rawlogcodec::Header rawHeader;
    rawHeader.runId = start.runId;
    uint8_t rawHeaderBytes[rawlogcodec::kHeaderSize];
    const size_t rawHeaderLength = rawlogcodec::encodeHeader(
        rawHeaderBytes, sizeof(rawHeaderBytes), rawHeader);
    if (rawHeaderLength != rawlogcodec::kHeaderSize) {
        return ArchiveError::INVALID_REQUEST;
    }

    uint8_t startedJson[512];
    size_t startedLength = 0;
    if (!buildStartedJson(start, startedJson, sizeof(startedJson),
                          startedLength)) {
        return ArchiveError::INVALID_REQUEST;
    }
    eventjournalcodec::Header eventHeader;
    eventHeader.runId = start.runId;
    uint8_t eventHeaderBytes[eventjournalcodec::kHeaderSize];
    const size_t eventHeaderLength = eventjournalcodec::encodeHeader(
        eventHeaderBytes, sizeof(eventHeaderBytes), eventHeader);
    if (eventHeaderLength != eventjournalcodec::kHeaderSize) {
        return ArchiveError::INVALID_REQUEST;
    }
    const size_t startedFrameLength = eventjournalcodec::encodeFrame(
        st.frameScratch, kFrameScratchBytes, startedJson, startedLength);
    if (startedFrameLength == 0) return ArchiveError::INVALID_REQUEST;
    const size_t eventsInitialLength = eventHeaderLength + startedFrameLength;

    char metaPath[160];
    char rawPath[160];
    char eventsPath[160];
    joinFile(stageFull, "meta.json", metaPath, sizeof(metaPath));
    joinFile(stageFull, "samples.bin", rawPath, sizeof(rawPath));
    joinFile(stageFull, "events.bin", eventsPath, sizeof(eventsPath));
    if (!writeNewFile(metaPath, metaBuffer, metaLength) ||
        !writeNewFile(rawPath, rawHeaderBytes, rawHeaderLength) ||
        !writeEventsInitialFile(eventsPath, eventHeaderBytes,
                                eventHeaderLength, st.frameScratch,
                                startedFrameLength)) {
        return ArchiveError::WRITE_FAILED;
    }
    syncDirectory(stageFull);
    if (isDirectory(finalFull)) return ArchiveError::RUN_ID_CONFLICT;
    if (rename(stageFull, finalFull) != 0) return ArchiveError::WRITE_FAILED;
    syncDirectory("/littlefs/runs");

    char finalRaw[160];
    joinFile(finalFull, "samples.bin", finalRaw, sizeof(finalRaw));
    const int samplesFd = open(finalRaw, O_WRONLY);
    if (samplesFd < 0) {
        return ArchiveError::WRITE_FAILED;
    }

    st.activeRunId = start.runId;
    st.samplesFd = samplesFd;
    st.activeDir[0] = '\0';
    boundedCopy(st.activeDir, sizeof(st.activeDir), finalFull);
    boundedCopy(st.activeRel, sizeof(st.activeRel), finalRel);
    st.samplesBytes = rawHeaderLength;
    st.eventsBytes = eventsInitialLength;
    st.metaBytes = metaLength;
    st.tailBytes = 0;
    st.durableLogSeq = 0;
    st.latestAcqSeq = 0;
    st.acquisitionGaps = 0;
    st.terminalSeen = false;
    st.pendingCount = 0;
    st.activeError = ArchiveError::NONE;
    resetWarm(st);

    RunMeta meta;
    meta.schemaVersion = runlog::kSchemaVersion;
    boundedCopy(meta.deviceId, sizeof(meta.deviceId), start.identity.deviceId);
    boundedCopy(meta.bootId, sizeof(meta.bootId), start.identity.bootId);
    meta.runId = start.runId;
    boundedCopy(meta.firmware, sizeof(meta.firmware), start.firmware);
    meta.protocolVersion = start.protocolVersion;
    meta.status = RunStatus::Active;
    meta.rawComplete = false;
    meta.acquireIntervalMs = start.acquireIntervalMs;
    meta.logIntervalMs = start.logIntervalMs;
    meta.graphBucketMs = start.graphBucketMs;
    meta.maxRunMs = start.maxRunMs;
    meta.startUptimeMs = start.startUptimeMs;
    meta.hasWallStartUtcMs = start.hasWallStartUtcMs;
    meta.wallStartUtcMs = start.wallStartUtcMs;
    meta.wallTimeSource = start.wallTimeSource;
    meta.sampleCount = 0;
    meta.durableLogSeq = 0;
    meta.latestAcqSeq = 0;
    meta.eventRevision = 1;
    meta.annotationCount = 0;
    meta.acquisitionGaps = 0;
    meta.droppedAcquired = 0;
    meta.sizeBytes = static_cast<uint32_t>(st.metaBytes + st.samplesBytes +
                                           st.eventsBytes);
    meta.recoveredTailBytes = 0;
    meta.error = ArchiveError::NONE;
    boundedCopy(meta.path, sizeof(meta.path), finalRel);
    st.activeMeta = meta;
    out = meta;
    return ArchiveError::NONE;
}

ArchiveError RunArchiveStore::appendSelected(uint32_t runId,
                                             const LoggedSample& sample) {
    if (impl_ == nullptr || !impl_->archiveReady) {
        return ArchiveError::ARCHIVE_UNAVAILABLE;
    }
    Impl& st = *impl_;
    if (st.activeRunId != runId) return ArchiveError::INVALID_REQUEST;
    if (st.activeError != ArchiveError::NONE) return st.activeError;
    if (sample.logSeq == 0 || sample.acqSeq == 0 ||
        sample.ms > runlog::kMaxDurationMs ||
        (sample.flags & ~rawlogcodec::kFlagsMask) != 0) {
        return ArchiveError::INVALID_REQUEST;
    }
    if (st.samplesFd < 0) return ArchiveError::WRITE_FAILED;
    const uint64_t expected =
        static_cast<uint64_t>(st.durableLogSeq) + st.pendingCount + 1u;
    if (sample.logSeq != expected) return ArchiveError::INVALID_REQUEST;
    if (st.pendingCount == kPendingCapacity) {
        const ArchiveError syncError = syncInternal(st, runId);
        if (syncError != ArchiveError::NONE) return syncError;
    }
    if (st.pendingCount >= kPendingCapacity) {
        return ArchiveError::BUSY;
    }
    const uint64_t bytes = static_cast<uint64_t>(st.samplesBytes) +
                           (st.pendingCount + 1u) * kRecordBytes;
    if (bytes > kRawLogicalMaxBytes) {
        return ArchiveError::INSUFFICIENT_STORAGE;
    }
    st.pending[st.pendingCount++] = sample;
    return ArchiveError::NONE;
}

ArchiveError RunArchiveStore::sync(uint32_t runId) {
    if (impl_ == nullptr || !impl_->archiveReady) {
        return ArchiveError::ARCHIVE_UNAVAILABLE;
    }
    if (runId == 0) return ArchiveError::INVALID_REQUEST;
    return syncInternal(*impl_, runId);
}

ArchiveError RunArchiveStore::finalize(uint32_t runId, RunStatus status,
                                       uint32_t endMs, const char* requestId,
                                       uint32_t droppedAcquired,
                                       ArchiveError cause) {
    if (impl_ == nullptr || !impl_->archiveReady) {
        return ArchiveError::ARCHIVE_UNAVAILABLE;
    }
    Impl& st = *impl_;
    if (st.activeRunId != runId) return ArchiveError::INVALID_REQUEST;
    if (status == RunStatus::Active || status == RunStatus::Corrupt) {
        return ArchiveError::INVALID_REQUEST;
    }
    if (endMs > runlog::kMaxDurationMs) return ArchiveError::INVALID_REQUEST;
    char internalRequestId[81];
    if (requestId == nullptr || requestId[0] == '\0') {
        // Automatic limit/storage-error stops have no client request id. The
        // journal reducer requires a nonempty ASCII token, so derive a stable
        // one from the run identity instead of inventing a revision.
        snprintf(internalRequestId, sizeof(internalRequestId), "end-%lu",
                 static_cast<unsigned long>(runId));
        requestId = internalRequestId;
    } else if (!validRequestId(requestId)) {
        return ArchiveError::INVALID_REQUEST;
    }

    ArchiveError syncError = syncInternal(st, runId);
    RunStatus effectiveStatus = status;
    ArchiveError effectiveCause = cause;
    if (syncError != ArchiveError::NONE) {
        effectiveStatus = RunStatus::StorageError;
        effectiveCause = syncError;
        st.activeMeta.status = RunStatus::StorageError;
        st.activeMeta.error = syncError;
        st.activeMeta.rawComplete = false;
    }
    if (effectiveStatus == RunStatus::StorageError &&
        effectiveCause == ArchiveError::NONE) {
        effectiveCause = ArchiveError::WRITE_FAILED;
    }

    uint8_t endedJson[512];
    size_t endedLength = 0;
    if (!buildEndedJson(st.activeMeta.eventRevision + 1u, effectiveStatus, endMs,
                        requestId, droppedAcquired, effectiveCause, endedJson,
                        sizeof(endedJson), endedLength)) {
        closeActiveWriter(st);
        st.activeError = ArchiveError::WRITE_FAILED;
        st.activeMeta.status = RunStatus::StorageError;
        st.activeMeta.error = st.activeError;
        st.activeMeta.rawComplete = false;
        return ArchiveError::WRITE_FAILED;
    }
    uint32_t durableRevision = 0;
    const ArchiveError eventError =
        appendEventInternal(st, runId, endedJson, endedLength, durableRevision);
    if (eventError != ArchiveError::NONE) {
        closeActiveWriter(st);
        st.activeError = eventError;
        st.activeMeta.status = RunStatus::StorageError;
        st.activeMeta.error = eventError;
        st.activeMeta.rawComplete = false;
        return eventError;
    }

    st.activeMeta.status = effectiveStatus;
    st.activeMeta.hasEndElapsedMs = true;
    st.activeMeta.endElapsedMs = endMs;
    st.activeMeta.droppedAcquired = droppedAcquired;
    st.activeMeta.error = effectiveCause;
    st.activeMeta.rawComplete =
        effectiveStatus == RunStatus::Complete &&
        effectiveCause == ArchiveError::NONE && st.acquisitionGaps == 0 &&
        droppedAcquired == 0 && st.tailBytes == 0;
    cacheMeta(st, st.activeMeta);
    closeActiveWriter(st);
    return syncError;
}

ArchiveError RunArchiveStore::captureSnapshot(uint32_t runId,
                                              RunSnapshot& out) {
    if (impl_ == nullptr || !impl_->archiveReady) {
        return ArchiveError::ARCHIVE_UNAVAILABLE;
    }
    if (runId == 0) return ArchiveError::INVALID_REQUEST;
    Impl& st = *impl_;
    if (st.activeRunId == runId) {
        const ArchiveError syncError = syncInternal(st, runId);
        if (syncError != ArchiveError::NONE) return syncError;
        out.meta = st.activeMeta;
        out.throughLogSeq = st.durableLogSeq;
        out.throughEventRevision = st.activeMeta.eventRevision;
        return ArchiveError::NONE;
    }
    RunSnapshot snapshot;
    const ArchiveError error = getMeta(runId, snapshot.meta);
    if (error != ArchiveError::NONE) return error;
    if (snapshot.meta.corrupt) return ArchiveError::CORRUPT_RUN;
    snapshot.throughLogSeq = snapshot.meta.durableLogSeq;
    snapshot.throughEventRevision = snapshot.meta.eventRevision;
    out = snapshot;
    return ArchiveError::NONE;
}

ArchiveError RunArchiveStore::readSamplesPage(
    uint32_t runId, uint32_t after, uint32_t through, uint8_t limit,
    LoggedSample* out, size_t capacity, PageInfo& page) {
    page = PageInfo{};
    if (impl_ == nullptr || !impl_->archiveReady) {
        return ArchiveError::ARCHIVE_UNAVAILABLE;
    }
    if (limit == 0 || limit > kMaxSamplePage || out == nullptr ||
        capacity < limit) {
        return ArchiveError::INVALID_REQUEST;
    }
    RunMeta meta;
    const ArchiveError metaError = getMeta(runId, meta);
    if (metaError != ArchiveError::NONE) return metaError;
    if (meta.corrupt) return ArchiveError::CORRUPT_RUN;
    if (through > meta.durableLogSeq || after > through ||
        after > meta.durableLogSeq) {
        return ArchiveError::INVALID_CURSOR;
    }
    boundedCopy(page.deviceId, sizeof(page.deviceId), meta.deviceId);
    boundedCopy(page.runBootId, sizeof(page.runBootId), meta.bootId);
    page.runId = runId;
    page.through = through;
    if (after == through) {
        page.nextCursor = after;
        page.done = true;
        return ArchiveError::NONE;
    }
    char dir[128];
    char rel[96];
    if (!resolveRunDir(runId, dir, sizeof(dir), rel, sizeof(rel))) {
        return ArchiveError::RUN_NOT_FOUND;
    }
    char path[160];
    joinFile(dir, "samples.bin", path, sizeof(path));
    int fd = open(path, O_RDONLY);
    if (fd < 0) return ArchiveError::CORRUPT_RUN;
    size_t fileBytes = 0;
    uint8_t headerBytes[rawlogcodec::kHeaderSize];
    rawlogcodec::Header header;
    if (!fileSizeOf(fd, fileBytes) ||
        !readAtExact(fd, 0, headerBytes, sizeof(headerBytes)) ||
        !rawlogcodec::decodeHeader(headerBytes, sizeof(headerBytes), header) ||
        header.runId != runId) {
        close(fd);
        return ArchiveError::CORRUPT_RUN;
    }
    const uint32_t available = through - after;
    uint32_t want = available < limit ? available : limit;
    const size_t offset = rawlogcodec::kHeaderSize +
                          static_cast<size_t>(after) * kRecordBytes;
    const size_t bytes = static_cast<size_t>(want) * kRecordBytes;
    if (offset + bytes > fileBytes) {
        close(fd);
        return ArchiveError::CORRUPT_RUN;
    }
    uint8_t buffer[kMaxSamplePage * kRecordBytes];
    if (!readAtExact(fd, offset, buffer, bytes)) {
        close(fd);
        return ArchiveError::STORAGE_UNAVAILABLE;
    }
    close(fd);
    for (uint32_t i = 0; i < want; ++i) {
        LoggedSample sample;
        if (!rawlogcodec::decodeRecord(buffer + i * kRecordBytes, kRecordBytes,
                                       sample) ||
            sample.logSeq != after + i + 1u) {
            return ArchiveError::CORRUPT_RUN;
        }
        out[i] = sample;
    }
    page.count = want;
    page.nextCursor = after + want;
    page.done = page.nextCursor == through;
    return ArchiveError::NONE;
}

ArchiveError RunArchiveStore::readEventsPage(
    uint32_t runId, uint32_t after, uint32_t through, uint8_t limit,
    uint8_t* jsonArrayOut, size_t capacity, PageInfo& page) {
    page = PageInfo{};
    if (impl_ == nullptr || !impl_->archiveReady) {
        return ArchiveError::ARCHIVE_UNAVAILABLE;
    }
    if (limit == 0 || limit > kMaxEventPage || jsonArrayOut == nullptr ||
        capacity < 3u) {
        return ArchiveError::INVALID_REQUEST;
    }
    RunMeta meta;
    const ArchiveError metaError = getMeta(runId, meta);
    if (metaError != ArchiveError::NONE) return metaError;
    if (meta.corrupt) return ArchiveError::CORRUPT_RUN;
    if (through > meta.eventRevision || after > through ||
        after > meta.eventRevision) {
        return ArchiveError::INVALID_CURSOR;
    }
    boundedCopy(page.deviceId, sizeof(page.deviceId), meta.deviceId);
    boundedCopy(page.runBootId, sizeof(page.runBootId), meta.bootId);
    page.runId = runId;
    page.through = through;
    if (after == through) {
        jsonArrayOut[0] = '[';
        jsonArrayOut[1] = ']';
        jsonArrayOut[2] = '\0';
        page.nextCursor = after;
        page.done = true;
        return ArchiveError::NONE;
    }
    Impl& st = *impl_;
    if (st.warmRunId != runId || st.warmPhase != Impl::WarmPhase::Ready)
        return ArchiveError::BUSY;
    if (through > st.reducer.revision()) return ArchiveError::INVALID_CURSOR;
    size_t position = 1;
    uint32_t next = after;
    uint32_t count = 0;
    jsonArrayOut[0] = '[';
    for (uint32_t revision = after + 1; revision <= through && count < limit; ++revision) {
        if (revision > 4096) return ArchiveError::CORRUPT_RUN;
        const size_t offset = st.eventOffsets[revision - 1];
        if (offset >= st.warmCacheBytes) return ArchiveError::CORRUPT_RUN;
        eventjournalcodec::FrameView frame{};
        if (!eventjournalcodec::decodeFrame(st.journalCache + offset,
                st.warmCacheBytes - offset, frame)) return ArchiveError::CORRUPT_RUN;
        if (position + (count ? 1 : 0) + frame.payloadBytes + 2 > capacity) break;
        if (count) jsonArrayOut[position++] = ',';
        memcpy(jsonArrayOut + position, frame.payload, frame.payloadBytes);
        position += frame.payloadBytes;
        next = revision;
        ++count;
    }
    if (!count) return ArchiveError::INVALID_REQUEST;
    jsonArrayOut[position++] = ']';
    jsonArrayOut[position] = '\0';
    page.count = count;
    page.nextCursor = next;
    page.done = next == through;
    return ArchiveError::NONE;
}

ArchiveError RunArchiveStore::readJournalBytes(uint32_t runId, size_t offset,
                                               uint8_t* out, size_t capacity,
                                               size_t& bytesRead) {
    bytesRead = 0;
    if (impl_ == nullptr || !impl_->archiveReady) {
        return ArchiveError::ARCHIVE_UNAVAILABLE;
    }
    if (runId == 0 || (out == nullptr && capacity != 0)) {
        return ArchiveError::INVALID_REQUEST;
    }
    char dir[128];
    char rel[96];
    if (!resolveRunDir(runId, dir, sizeof(dir), rel, sizeof(rel))) {
        return ArchiveError::RUN_NOT_FOUND;
    }
    char path[160];
    joinFile(dir, "events.bin", path, sizeof(path));
    int fd = open(path, O_RDONLY);
    if (fd < 0) return ArchiveError::CORRUPT_RUN;
    size_t fileBytes = 0;
    if (!fileSizeOf(fd, fileBytes)) {
        close(fd);
        return ArchiveError::STORAGE_UNAVAILABLE;
    }
    if (offset > fileBytes) {
        close(fd);
        return ArchiveError::INVALID_CURSOR;
    }
    size_t want = fileBytes - offset;
    if (want > capacity) want = capacity;
    if (want > 0 && !readAtExact(fd, offset, out, want)) {
        close(fd);
        return ArchiveError::STORAGE_UNAVAILABLE;
    }
    close(fd);
    bytesRead = want;
    return ArchiveError::NONE;
}

ArchiveError RunArchiveStore::appendEventAndSync(uint32_t runId,
                                                 const uint8_t* json,
                                                 size_t bytes,
                                                 uint32_t& durableRevision) {
    if (impl_ == nullptr || !impl_->archiveReady) {
        return ArchiveError::ARCHIVE_UNAVAILABLE;
    }
    if (runId == 0) return ArchiveError::INVALID_REQUEST;
    return appendEventInternal(*impl_, runId, json, bytes, durableRevision);
}

ArchiveError RunArchiveStore::deleteRun(uint32_t runId, bool& deleted) {
    deleted = false;
    if (impl_ == nullptr || !impl_->archiveReady) {
        return ArchiveError::ARCHIVE_UNAVAILABLE;
    }
    if (runId == 0) return ArchiveError::INVALID_REQUEST;
    Impl& st = *impl_;
    if (st.activeRunId == runId) return ArchiveError::RUN_ACTIVE;
    char full[128];
    char rel[96];
    if (!resolveRunDir(runId, full, sizeof(full), rel, sizeof(rel))) {
        return ArchiveError::NONE;
    }
    char deletingFull[128];
    char deletingRel[96];
    makeDeletingPaths(runId, deletingFull, sizeof(deletingFull), deletingRel,
                      sizeof(deletingRel));
    if (isDirectory(deletingFull)) {
        if (!removeDirRecursive(deletingFull)) {
            return ArchiveError::WRITE_FAILED;
        }
    }
    if (rename(full, deletingFull) != 0) return ArchiveError::WRITE_FAILED;
    invalidateMeta(st, runId);
    syncDirectory("/littlefs/runs");
    if (!removeDirRecursive(deletingFull)) return ArchiveError::WRITE_FAILED;
    if (st.warmRunId == runId) resetWarm(st);
    deleted = true;
    return ArchiveError::NONE;
}

uint32_t RunArchiveStore::remainingReservationBytes() const {
    if (impl_ == nullptr) return 0;
    return remainingReservationInternal(*impl_);
}

bool RunArchiveStore::warmMarkerCacheStep(uint32_t runId,
                                          ArchiveError& error) {
    error = ArchiveError::NONE;
    if (impl_ == nullptr || !impl_->archiveReady) {
        error = ArchiveError::ARCHIVE_UNAVAILABLE;
        return true;
    }
    if (runId == 0) {
        error = ArchiveError::INVALID_REQUEST;
        return true;
    }
    Impl& st = *impl_;
    if (st.warmPhase == Impl::WarmPhase::Idle || st.warmRunId != runId) {
        resetWarm(st);
        st.warmRunId = runId;
        const ArchiveError startError = startWarm(st, runId);
        if (startError != ArchiveError::NONE) {
            st.warmPhase = Impl::WarmPhase::Error;
            st.warmError = startError;
            error = startError;
            return true;
        }
    }
    if (st.warmPhase == Impl::WarmPhase::Error) {
        error = st.warmError;
        return true;
    }
    if (st.warmPhase == Impl::WarmPhase::Ready) {
        if (st.warmTail) {
            error = ArchiveError::NONE;
            return true;
        }
        char dir[128];
        char rel[96];
        if (!resolveRunDir(runId, dir, sizeof(dir), rel, sizeof(rel))) {
            error = ArchiveError::RUN_NOT_FOUND;
            return true;
        }
        char path[160];
        joinFile(dir, "events.bin", path, sizeof(path));
        size_t size = 0;
        if (!pathSize(path, size)) {
            error = ArchiveError::STORAGE_UNAVAILABLE;
            return true;
        }
        if (size < st.warmFileSize) {
            resetWarm(st);
            return false;
        }
        if (size > st.warmFileSize) {
            st.warmFileSize = size;
            st.warmPhase = Impl::WarmPhase::Reading;
        } else {
            error = ArchiveError::NONE;
            return true;
        }
    }
    if (st.warmPhase == Impl::WarmPhase::Reading) {
        char dir[128];
        char rel[96];
        if (!resolveRunDir(runId, dir, sizeof(dir), rel, sizeof(rel))) {
            error = ArchiveError::RUN_NOT_FOUND;
            return true;
        }
        char path[160];
        joinFile(dir, "events.bin", path, sizeof(path));
        int fd = open(path, O_RDONLY);
        if (fd < 0) {
            error = ArchiveError::STORAGE_UNAVAILABLE;
            return true;
        }
        size_t size = 0;
        if (!fileSizeOf(fd, size)) {
            close(fd);
            error = ArchiveError::STORAGE_UNAVAILABLE;
            return true;
        }
        st.warmFileSize = size;
        size_t budget = kWarmReadBudgetBytes;
        while (st.warmReadOffset + eventjournalcodec::kFrameOverheadBytes <=
                   size &&
               budget >= eventjournalcodec::kFrameOverheadBytes) {
            uint8_t lengthBytes[4];
            if (!readAtExact(fd, st.warmReadOffset, lengthBytes,
                             sizeof(lengthBytes))) {
                close(fd);
                error = ArchiveError::STORAGE_UNAVAILABLE;
                return true;
            }
            const uint32_t payloadBytes =
                static_cast<uint32_t>(lengthBytes[0]) |
                (static_cast<uint32_t>(lengthBytes[1]) << 8) |
                (static_cast<uint32_t>(lengthBytes[2]) << 16) |
                (static_cast<uint32_t>(lengthBytes[3]) << 24);
            if (payloadBytes < eventjournalcodec::kMinPayloadBytes ||
                payloadBytes > eventjournalcodec::kMaxPayloadBytes) {
                st.warmTail = true;
                break;
            }
            const size_t frameBytes =
                eventjournalcodec::kFrameOverheadBytes + payloadBytes;
            if (st.warmReadOffset + frameBytes > size) {
                st.warmTail = true;
                break;
            }
            if (budget < frameBytes + 4u) break;
            if (st.warmCacheBytes + frameBytes > kJournalCacheBytes) {
                close(fd);
                st.warmPhase = Impl::WarmPhase::Error;
                st.warmError = ArchiveError::EVENT_BUDGET_EXHAUSTED;
                error = st.warmError;
                return true;
            }
            if (!readAtExact(fd, st.warmReadOffset,
                             st.journalCache + st.warmCacheBytes, frameBytes)) {
                close(fd);
                error = ArchiveError::STORAGE_UNAVAILABLE;
                return true;
            }
            eventjournalcodec::FrameView view;
            if (!eventjournalcodec::decodeFrame(
                    st.journalCache + st.warmCacheBytes, frameBytes, view)) {
                st.warmTail = true;
                break;
            }
            uint32_t revision = 0;
            EventKind kind = EventKind::Unknown;
            if (!eventIdentity(view.payload, view.payloadBytes, revision,
                               kind) ||
                revision != st.warmExpectedRevision) {
                st.warmTail = true;
                break;
            }
            if (kind == EventKind::Started) {
                if (revision != 1 || st.warmStarted) {
                    st.warmTail = true;
                    break;
                }
                st.warmStarted = true;
            } else if (kind == EventKind::Ended) {
                if (st.warmEnded) {
                    st.warmTail = true;
                    break;
                }
                st.warmEnded = true;
            }
            if (revision > 4096) {
                close(fd);
                error = ArchiveError::CORRUPT_RUN;
                return true;
            }
            st.eventOffsets[revision - 1] = st.warmCacheBytes;
            st.warmCacheBytes += frameBytes;
            st.warmReadOffset += frameBytes;
            st.warmExpectedRevision = revision + 1u;
            budget -= frameBytes + 4u;
        }
        const size_t remaining =
            st.warmReadOffset <= size ? size - st.warmReadOffset : 0;
        if (!st.warmTail && remaining > 0 && remaining < 8u) {
            st.warmTail = true;  // incomplete frame header/tail
        }
        close(fd);
        if (!st.warmTail && remaining != 0) return false;
        st.warmPhase = Impl::WarmPhase::Reducing;
        return warmReduceStep(st, error);
    }
    if (st.warmPhase == Impl::WarmPhase::Reducing) {
        return warmReduceStep(st, error);
    }
    error = ArchiveError::INVALID_REQUEST;
    return true;
}

const MarkerReducer& RunArchiveStore::markerReducer() const {
    static MarkerReducer empty;
    if (impl_ == nullptr) return empty;
    return impl_->reducer;
}

#endif  // PYROVUE_RUN_ARCHIVE && ARDUINO_ARCH_ESP32
