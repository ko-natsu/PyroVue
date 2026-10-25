#ifndef LOOP_REQUEST_QUEUE_H
#define LOOP_REQUEST_QUEUE_H

#include <atomic>
#include <functional>
#include <memory>
#include "ESPAsyncWebServer.h"
#include "CsvDownloadBuffer.h"

#if defined(PYROVUE_SINGLE_RUN_CSV)
namespace csvrun { class SingleRunCsv; }
#endif

struct NetworkCommandResult {
    uint16_t status;
    const char* error;
};

// Network callbacks enqueue owned arguments; the Arduino loop owns commands/FS.
class LoopRequestQueue {
public:
    void begin(AsyncWebServer& server,
               std::function<NetworkCommandResult(const char*)>& command,
               std::function<void()> stateChanged,
               std::function<void(const char*)> websocketError,
               std::function<bool()> acquireState,
               std::function<void()> releaseState,
               std::function<void(AsyncWebServerRequest*, bool)> readDevice);
    bool enqueueCommand(const uint8_t* data, size_t length);
    void poll();
#if defined(PYROVUE_SINGLE_RUN_CSV)
    void setCsvRecorder(csvrun::SingleRunCsv& recorder) { csv = &recorder; }
#endif

private:
    enum class Kind : uint8_t { Command, DeviceInfo, Now, Info, Download, Erase, Format };
    enum class SlotState : uint8_t { Free, Filling, Ready };
    struct Job {
        std::atomic<SlotState> state{SlotState::Free};
        Kind kind = Kind::Command;
        uint32_t runId = 0;
        uint32_t ticket = 0;
        char command[257]{};
        bool http = false;
        AsyncWebServerRequestPtr request;
    };
    static constexpr size_t slotCount = 4;
    Job jobs[slotCount];
    std::atomic<uint32_t> nextTicket{0};
    std::function<NetworkCommandResult(const char*)>* commandHandler = nullptr;
    std::function<void()> stateChanged;
    std::function<void(const char*)> websocketError;
    std::function<bool()> acquireState;
    std::function<void()> releaseState;
    std::function<void(AsyncWebServerRequest*, bool)> readDevice;

    Job* reserve();
    bool enqueue(Kind kind, AsyncWebServerRequest* request, uint32_t runId = 0,
                 const uint8_t* data = nullptr, size_t length = 0);
    static void sendError(AsyncWebServerRequest* request, uint16_t status, const char* code);
    void execute(Job& job);
#if defined(PYROVUE_SINGLE_RUN_CSV)
    csvrun::SingleRunCsv* csv = nullptr;
    std::shared_ptr<CsvDownloadBuffer> download;
    size_t downloadRemaining = 0;
    void pollDownload();
    static bool parseRunId(AsyncWebServerRequest* request, uint32_t& out, bool allowZero = false);
#endif
};

#endif
