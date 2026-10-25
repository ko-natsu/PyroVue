#ifndef RUN_LOG_TYPES_H
#define RUN_LOG_TYPES_H

#include <stddef.h>
#include <stdint.h>

// Archive schema 1. POD values cross the owner-loop/network boundary by copy.
// Optional times use presence flags; zero is a valid elapsed timestamp.
namespace runlog {
constexpr uint32_t kSchemaVersion = 1;
constexpr uint32_t kAcquireIntervalMs = 1000;
constexpr uint32_t kLogIntervalMs = 2000;
constexpr uint32_t kMaxDurationMs = 86400000;
constexpr size_t kMaxEventPayloadBytes = 4096;
constexpr size_t kMaxNoteBytes = 2000;
constexpr size_t kMaxNoteScalars = 500;
constexpr uint8_t kTerminal = 1;
constexpr uint8_t kAcquisitionGap = 2;
}

struct DeviceIdentity { char deviceId[13]{}; char bootId[17]{}; };
struct LoggedSample {
    uint32_t logSeq{};
    uint32_t acqSeq{};
    uint32_t ms{};
    float tempC{};
    uint8_t fault{};
    uint8_t intervalFault{};
    uint8_t flags{};
};

enum class RunStatus : uint8_t { Active, Complete, Interrupted, DurationLimit, StorageError, Corrupt };
enum class ArchiveError : uint8_t {
    NONE, INVALID_REQUEST, RUN_NOT_FOUND, MARKER_NOT_FOUND, ARCHIVE_UNAVAILABLE,
    RUN_ACTIVE, RUN_ID_CONFLICT, REVISION_CONFLICT, OPERATION_ID_CONFLICT,
    EVENT_TIME_OUT_OF_RANGE, CORRUPT_RUN, STALE_COMMAND, DEVICE_ID_MISMATCH,
    BODY_TOO_LARGE, INVALID_CURSOR, BUSY, STORAGE_UNAVAILABLE, WRITE_FAILED,
    INSUFFICIENT_STORAGE, EVENT_BUDGET_EXHAUSTED, RUN_ID_SAVE_FAILED, RUN_ID_EXHAUSTED
};

inline const char* archiveErrorName(ArchiveError error) {
    switch (error) {
        case ArchiveError::NONE: return nullptr;
        case ArchiveError::INVALID_REQUEST: return "INVALID_REQUEST";
        case ArchiveError::RUN_NOT_FOUND: return "RUN_NOT_FOUND";
        case ArchiveError::MARKER_NOT_FOUND: return "MARKER_NOT_FOUND";
        case ArchiveError::ARCHIVE_UNAVAILABLE: return "ARCHIVE_UNAVAILABLE";
        case ArchiveError::RUN_ACTIVE: return "RUN_ACTIVE";
        case ArchiveError::RUN_ID_CONFLICT: return "RUN_ID_CONFLICT";
        case ArchiveError::REVISION_CONFLICT: return "REVISION_CONFLICT";
        case ArchiveError::OPERATION_ID_CONFLICT: return "OPERATION_ID_CONFLICT";
        case ArchiveError::EVENT_TIME_OUT_OF_RANGE: return "EVENT_TIME_OUT_OF_RANGE";
        case ArchiveError::CORRUPT_RUN: return "CORRUPT_RUN";
        case ArchiveError::STALE_COMMAND: return "STALE_COMMAND";
        case ArchiveError::DEVICE_ID_MISMATCH: return "DEVICE_ID_MISMATCH";
        case ArchiveError::BODY_TOO_LARGE: return "BODY_TOO_LARGE";
        case ArchiveError::INVALID_CURSOR: return "INVALID_CURSOR";
        case ArchiveError::BUSY: return "BUSY";
        case ArchiveError::STORAGE_UNAVAILABLE: return "STORAGE_UNAVAILABLE";
        case ArchiveError::WRITE_FAILED: return "WRITE_FAILED";
        case ArchiveError::INSUFFICIENT_STORAGE: return "INSUFFICIENT_STORAGE";
        case ArchiveError::EVENT_BUDGET_EXHAUSTED: return "EVENT_BUDGET_EXHAUSTED";
        case ArchiveError::RUN_ID_SAVE_FAILED: return "RUN_ID_SAVE_FAILED";
        case ArchiveError::RUN_ID_EXHAUSTED: return "RUN_ID_EXHAUSTED";
    }
    return "INVALID_REQUEST";
}
enum class WallTimeSource : uint8_t { Unavailable, ClientEstimated };
enum class TimeBasis : uint8_t { Estimated, Manual, Untimed };
enum class MarkerMutationKind : uint8_t { Create, Update, Delete };
enum class CommandKind : uint8_t { Start, Stop, Preset };
enum class LoggingState : uint8_t { Idle, Recording, Error, Unavailable };

struct RunMeta {
    // Corrupt wire variant includes schema/device/run/status/path/size/tail,
    // rawComplete=false and error=CORRUPT_RUN; never invent healthy provenance.
    bool corrupt{};
    uint32_t schemaVersion{runlog::kSchemaVersion};
    char deviceId[13]{};
    char bootId[17]{};
    uint32_t runId{};
    char firmware[64]{};
    uint32_t protocolVersion{}; // Originating firmware protocol, not current wire version.
    RunStatus status{RunStatus::Interrupted};
    bool rawComplete{};
    uint32_t acquireIntervalMs{};
    uint32_t logIntervalMs{};
    uint32_t graphBucketMs{};
    uint32_t maxRunMs{};
    uint32_t startUptimeMs{};
    bool hasEndElapsedMs{};
    uint32_t endElapsedMs{};
    bool hasWallStartUtcMs{};
    uint64_t wallStartUtcMs{};
    WallTimeSource wallTimeSource{WallTimeSource::Unavailable};
    uint32_t sampleCount{};
    uint32_t durableLogSeq{};
    uint32_t latestAcqSeq{};
    uint32_t eventRevision{};
    uint32_t annotationCount{};
    uint32_t acquisitionGaps{};
    uint32_t droppedAcquired{};
    uint32_t sizeBytes{};
    uint32_t recoveredTailBytes{};
    ArchiveError error{ArchiveError::NONE};
    char path[96]{};
};
struct RunStart {
    uint32_t runId{};
    uint32_t startUptimeMs{};
    char startRequestId[81]{};
    DeviceIdentity identity{};
    char firmware[64]{};
    uint32_t protocolVersion{};
    uint32_t acquireIntervalMs{runlog::kAcquireIntervalMs};
    uint32_t logIntervalMs{runlog::kLogIntervalMs};
    uint32_t graphBucketMs{5000};
    uint32_t maxRunMs{runlog::kMaxDurationMs};
    bool hasWallStartUtcMs{};
    uint64_t wallStartUtcMs{};
    WallTimeSource wallTimeSource{WallTimeSource::Unavailable};
};
struct CommandRequest {
    CommandKind command{CommandKind::Start};
    char requestId[81]{};
    char bootId[17]{};
    uint32_t afterRunId{};
    uint32_t targetRunId{};
    char value[33]{};
    bool hasWallStartUtcMs{};
    uint64_t wallStartUtcMs{};
};
struct CommandResult {
    char requestId[81]{};
    bool ok{};
    uint32_t runId{};
    bool runActive{};
    uint32_t targetRunId{};
    bool durable{};
    ArchiveError error{ArchiveError::NONE};
};
struct MarkerMutation {
    MarkerMutationKind kind{MarkerMutationKind::Create};
    char operationId[81]{};
    char id[81]{};
    uint32_t baseRevision{};
    bool hasMs{};
    uint32_t ms{};
    char note[2001]{};
    TimeBasis timeBasis{TimeBasis::Untimed};
};
struct MarkerState {
    char id[81]{};
    bool hasMs{};
    uint32_t ms{};
    char note[2001]{};
    TimeBasis timeBasis{TimeBasis::Untimed};
    uint32_t revision{};
    bool deleted{};
};
struct MutationResult {
    uint32_t runId{};
    char operationId[81]{};
    char id[81]{};
    uint32_t revision{};
    bool persisted{};
    bool deleted{};
};
struct RunSnapshot { RunMeta meta{}; uint32_t throughLogSeq{}; uint32_t throughEventRevision{}; };
struct CatalogSummary { uint32_t maxRunId{}; uint32_t runCount{}; uint32_t corruptCount{}; };
struct PageInfo {
    uint32_t count{};
    uint32_t nextCursor{};
    bool done{};
    char deviceId[13]{};
    char runBootId[17]{};
    uint32_t runId{};
    uint32_t through{};
};
struct LoggingHealth {
    LoggingState state{LoggingState::Unavailable};
    uint32_t durableLogSeq{};
    uint32_t latestAcqSeq{};
    uint32_t bufferedRecords{};
    uint32_t acquisitionGaps{};
    uint32_t droppedAcquired{};
    uint32_t queueHighWater{};
    ArchiveError error{ArchiveError::ARCHIVE_UNAVAILABLE};
};
struct StorageStatus {
    bool mounted{};
    uint32_t totalBytes{};
    uint32_t usedBytes{};
    uint32_t freeBytes{};
    uint32_t reservedRemainingBytes{};
    uint32_t startRequiredBytes{};
};
struct NetworkStatusSnapshot {
    DeviceIdentity identity{};
    char firmware[64]{};
    uint32_t protocolVersion{};
    uint32_t sampleHz{};
    uint32_t runId{};
    bool runActive{};
    uint32_t runStartMs{};
    char preset[33]{};
    uint32_t clients{};
    uint32_t recentWindowMs{};
    uint32_t coarseResolutionMs{};
    uint32_t snapshotThroughSeq{};
    bool runArchive{};
    uint32_t acquireIntervalMs{};
    uint32_t logIntervalMs{};
    uint32_t maxRunMs{};
    LoggingHealth logging{};
    StorageStatus storage{};
};

#endif // RUN_LOG_TYPES_H
