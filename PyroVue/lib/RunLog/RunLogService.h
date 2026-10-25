#ifndef RUN_LOG_SERVICE_H
#define RUN_LOG_SERVICE_H

#include <stddef.h>
#include <stdint.h>

#include "RunLogTypes.h"
#include "RunRecorder.h"
#include "protocol.h"

#if defined(ARDUINO_ARCH_ESP32)
#include <Arduino.h>
#endif

#ifndef RUN_SAMPLE_QUEUE_CAPACITY
#define RUN_SAMPLE_QUEUE_CAPACITY 64
#endif

class RunManager;
class HistoryStore;
class RunArchiveStore;

// M2/M3 implementation contract. Only copyPublishedStatus is callback-safe/RAM-only.
// All other calls happen on the Arduino owner loop.
class RunLogService {
public:
    RunLogService(RunManager&, HistoryStore&, RunArchiveStore&, const DeviceIdentity&);
    ~RunLogService();
    RunLogService(const RunLogService&) = delete;
    RunLogService& operator=(const RunLogService&) = delete;
    ArchiveError begin(uint32_t nowMs);
    bool enqueueSample(const TempSample&);
    void enforceLimit(uint32_t nowMs);
    void poll(uint32_t nowMs);
    CommandResult handleCommand(const CommandRequest&);
    bool mutateMarkerStep(uint32_t runId, const MarkerMutation&, uint32_t nowMs, MutationResult& out, ArchiveError& error);
    RunArchiveStore& archive();
    void copyPublishedStatus(NetworkStatusSnapshot& out) const;

private:
    // Fixed in-run acquisition queue. A full queue is a logging failure, not
    // backpressure: idle monitoring continues after the run is finalized.
    TempSample queue_[RUN_SAMPLE_QUEUE_CAPACITY]{};
    uint32_t queueHead_{};
    uint32_t queueTail_{};
    uint32_t queueCount_{};

    void clearQueue();
    void pushQueued(const TempSample& sample);
    bool popQueued(TempSample& out);
    void drainQueue(bool limitOnly);
    void appendSelected(const LoggedSample& record);
    ArchiveError syncPending();
    ArchiveError finalizeRun(RunStatus status, ArchiveError cause,
                             const char* requestId, uint32_t nowMs);
    void refreshPublishedStatus(uint32_t nowMs, bool forceStorage);
    ArchiveError markerBoundary(uint32_t runId, uint32_t nowMs,
                                uint32_t& boundary);

    // One explicit PSRAM scratch reused by every marker mutation. It keeps the
    // two ~2 KB mutation domains, the ~2 KB marker state and the <=4 KB event
    // JSON off the 8 KB owner-loop stack.
    struct MarkerScratch;
    bool ensureMarkerScratch();

    RunManager& runManager_;
    HistoryStore& history_;
    RunArchiveStore& store_;
    const DeviceIdentity& identity_;

    RunRecorder recorder_;

    bool recording_{};
    bool hasDirty_{};
    bool overflowed_{};
    bool storeFailure_{};
    ArchiveError storeFailureError_{ArchiveError::NONE};

    uint32_t dirtySinceMs_{};       // elapsed ms of first unsynced record
    uint32_t durableLogSeq_{};
    uint32_t lastSelectedLogSeq_{};
    uint32_t latestAcqSeq_{};
    uint32_t acquisitionGaps_{};
    uint32_t droppedAcquired_{};
    uint32_t queueHighWater_{};

    // Catalog enumeration result from begin(); Start must not allocate an id
    // that could collide with archives that could not be enumerated.
    bool catalogValid_{};
    ArchiveError catalogError_{ArchiveError::ARCHIVE_UNAVAILABLE};

    MarkerScratch* markerScratch_{};

    LoggingState state_{LoggingState::Unavailable};
    ArchiveError loggingError_{ArchiveError::ARCHIVE_UNAVAILABLE};

    // Writer-owned cache of the last computed storage status. Only owner-loop
    // code calls store_.storageStatus(); the copy below is RAM-only.
    StorageStatus storageCache_{};
    uint32_t lastStorageRefreshMs_{};
    bool statusInitialized_{};

    NetworkStatusSnapshot published_{};
#if defined(ARDUINO_ARCH_ESP32)
    mutable portMUX_TYPE statusMux_ = portMUX_INITIALIZER_UNLOCKED;
#else
    mutable int statusMux_{};
#endif
};

#endif // RUN_LOG_SERVICE_H
