#ifndef ARCHIVE_ROUTES_H
#define ARCHIVE_ROUTES_H
#include <functional>
#include <atomic>
#include <memory>
#include "RunLogTypes.h"
class AsyncWebServer;
class RunLogService;
class AsyncWebServerRequest;

enum class WorkerJobKind : uint8_t {
    Command,
#if defined(PYROVUE_RUN_ARCHIVE)
    ListRuns, GetMeta, Snapshot, Samples, Events, MutateMarker, DeleteRun
#endif
};
enum class ResponseSlotState : uint8_t { Free, Assembling, Queued, Ready, Sending, Cancelled };

// M3 allocates these once: PSRAM for archive targets, one small internal slot
// for monitor targets. Accepted mutations retain owned arguments after disconnect.
struct WorkerJob {
    WorkerJobKind kind{WorkerJobKind::Command};
    uint8_t slotIndex{};
    uint32_t slotGeneration{};
    std::weak_ptr<AsyncWebServerRequest> request{};
    char expectedDeviceId[13]{};
    std::atomic<bool> cancelled{false};
    CommandRequest command{};
#if defined(PYROVUE_RUN_ARCHIVE)
    uint32_t runId{};
    uint32_t beforeRunId{};
    uint32_t after{};
    uint32_t through{};
    uint8_t limit{};
    MarkerMutation marker{};
#endif
};

// M3 implementation contract. One job step per poll; never filesystem IO in callbacks.
class ArchiveRoutes {
public:
    void begin(AsyncWebServer&, std::function<CommandResult(const CommandRequest&)>&, RunLogService* logger);
    void poll(uint32_t nowMs);
};
#endif
