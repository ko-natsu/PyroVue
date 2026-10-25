#ifndef LOOP_REQUEST_QUEUE_H
#define LOOP_REQUEST_QUEUE_H

#if defined(PYROVUE_SINGLE_RUN_CSV)
#include <atomic>
#include <memory>
#include "ESPAsyncWebServer.h"
#include "CsvDownloadBuffer.h"

namespace csvrun { class SingleRunCsv; }

// Retained Feather CSV jobs only. Protocol commands use ArchiveRoutes.
class LoopRequestQueue {
public:
    void begin(AsyncWebServer& server);
    void poll();
    void setCsvRecorder(csvrun::SingleRunCsv& recorder) { csv = &recorder; }
private:
    enum class Kind : uint8_t { Info, Download, Erase, Format };
    enum class SlotState : uint8_t { Free, Filling, Ready };
    struct Job {
        std::atomic<SlotState> state{SlotState::Free};
        Kind kind = Kind::Info;
        uint32_t runId = 0;
        uint32_t ticket = 0;
        char command[65]{};
        AsyncWebServerRequestPtr request;
    };
    static constexpr size_t slotCount = 4;
    Job jobs[slotCount];
    std::atomic<uint32_t> nextTicket{0};

    Job* reserve();
    bool enqueue(Kind kind, AsyncWebServerRequest* request, uint32_t runId = 0,
                 const uint8_t* data = nullptr, size_t length = 0);
    static void sendError(AsyncWebServerRequest* request, uint16_t status, const char* code);
    void execute(Job& job);
    csvrun::SingleRunCsv* csv = nullptr;
    std::shared_ptr<CsvDownloadBuffer> download;
    size_t downloadRemaining = 0;
    static bool parseRunId(AsyncWebServerRequest* request, uint32_t& out, bool allowZero = false);
    void pollDownload();
};
#endif // PYROVUE_SINGLE_RUN_CSV

#endif
