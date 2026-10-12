#ifndef RUN_ARCHIVE_STORE_H
#define RUN_ARCHIVE_STORE_H
#include "RunLogTypes.h"
class MarkerReducer;
// M2 implementation contract; Arduino loop owns all calls and filesystem IO.
class RunArchiveStore {
public:
    RunArchiveStore() = default;
    ArchiveError begin(const char* deviceId, const char* currentBootId);
    ArchiveError scanCatalog(CatalogSummary& out);
    ArchiveError listRuns(uint32_t beforeRunId, uint8_t limit, RunMeta* out, size_t capacity, uint32_t& nextBeforeRunId);
    ArchiveError getMeta(uint32_t runId, RunMeta& out);
    ArchiveError findStartRequest(const char* requestId, bool& found, RunMeta& out);
    ArchiveError prepareRun(const RunStart&, RunMeta& out);
    ArchiveError appendSelected(uint32_t runId, const LoggedSample&);
    ArchiveError sync(uint32_t runId);
    ArchiveError finalize(uint32_t runId, RunStatus, uint32_t endMs, const char* requestId, uint32_t droppedAcquired, ArchiveError cause);
    ArchiveError captureSnapshot(uint32_t runId, RunSnapshot& out);
    ArchiveError readSamplesPage(uint32_t runId, uint32_t after, uint32_t through, uint8_t limit, LoggedSample* out, size_t capacity, PageInfo&);
    ArchiveError readEventsPage(uint32_t runId, uint32_t after, uint32_t through, uint8_t limit, uint8_t* jsonArrayOut, size_t capacity, PageInfo&);
    ArchiveError readJournalBytes(uint32_t runId, size_t offset, uint8_t* out, size_t capacity, size_t& bytesRead);
    ArchiveError appendEventAndSync(uint32_t runId, const uint8_t* json, size_t bytes, uint32_t& durableRevision);
    ArchiveError deleteRun(uint32_t runId, bool& deleted);
    uint32_t remainingReservationBytes() const;
    bool warmMarkerCacheStep(uint32_t runId, ArchiveError& error);
    const MarkerReducer& markerReducer() const;
};
#endif
