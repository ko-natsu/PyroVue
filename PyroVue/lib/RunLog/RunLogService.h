#ifndef RUN_LOG_SERVICE_H
#define RUN_LOG_SERVICE_H
#include "RunLogTypes.h"
#include "protocol.h"
class RunManager;
class HistoryStore;
class RunArchiveStore;
// M2/M3 implementation contract. Only copyPublishedStatus is callback-safe/RAM-only.
class RunLogService {
public:
    RunLogService(RunManager&, HistoryStore&, RunArchiveStore&, const DeviceIdentity&);
    ArchiveError begin(uint32_t nowMs);
    bool enqueueSample(const TempSample&);
    void enforceLimit(uint32_t nowMs);
    void poll(uint32_t nowMs);
    CommandResult handleCommand(const CommandRequest&);
    bool mutateMarkerStep(uint32_t runId, const MarkerMutation&, uint32_t nowMs, MutationResult& out, ArchiveError& error);
    RunArchiveStore& archive();
    void copyPublishedStatus(NetworkStatusSnapshot& out) const;
};
#endif
