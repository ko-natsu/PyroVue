#ifndef NETWORK_MANAGER_H
#define NETWORK_MANAGER_H

#include <atomic>
#include <functional>
#include "Arduino.h"
#include "ESPAsyncWebServer.h"
#include "protocol.h"
#include "RunLogTypes.h"
#include "ArchiveRoutes.h"
#if defined(PYROVUE_SINGLE_RUN_CSV)
#include "LoopRequestQueue.h"
#endif

class HistoryStore;
class PresetManager;
class RunManager;
class RunLogService;

class NetworkManager {
public:
    NetworkManager(RunManager& run, HistoryStore& history, PresetManager& preset);
    void setIdentity(const DeviceIdentity& identity) { deviceIdentity = identity; }
    void begin();
    void loop();
    void onNewSample(const TempSample& sample);
    size_t clientCount() const;
    std::function<CommandResult(const CommandRequest&)> onCommand;
    void broadcastRunState();
#if defined(PYROVUE_SINGLE_RUN_CSV)
    void setCsvRecorder(csvrun::SingleRunCsv& recorder) { csvRequests.setCsvRecorder(recorder); }
#endif
#if defined(PYROVUE_BOARD_N16R8)
    void setRunLogger(RunLogService& logger) { runLogger = &logger; }
#endif

private:
    AsyncWebServer server;
    AsyncWebSocket ws;
    ArchiveRoutes archiveRoutes;
#if defined(PYROVUE_SINGLE_RUN_CSV)
    LoopRequestQueue csvRequests;
#endif
    RunManager& runManager;
    HistoryStore& historyStore;
    PresetManager& presetManager;
    DeviceIdentity deviceIdentity{};
    RunLogService* runLogger = nullptr;
    uint32_t nextSnapshotId = 0;
    std::atomic<uint32_t> pendingReplayClient{0};
    struct Replay {
        enum class Phase : uint8_t { Idle, Coarse, Raw, End };
        Phase phase{Phase::Idle};
        uint32_t clientId{};
        uint32_t snapshotId{};
        uint32_t runId{};
        uint32_t throughSeq{};
        uint32_t afterSeq{};
        uint32_t fromCoarseMs{};
        uint32_t throughCoarseMs{};
        uint32_t chunk{};
    } replay;
    NetworkStatusSnapshot published{};
    TempSample publishedReading{};
    bool hasPublishedReading = false;
#if defined(ARDUINO_ARCH_ESP32)
    mutable portMUX_TYPE statusMux = portMUX_INITIALIZER_UNLOCKED;
#endif

    void publishStatus();
    void copyStatus(NetworkStatusSnapshot& out, TempSample& reading, bool& hasReading) const;
    void handleWsEvent(AsyncWebSocketClient* client, AwsEventType type,
                       void* arg, uint8_t* data, size_t len);
    void startReplay(uint32_t clientId);
    void pollReplay();
    void sendDeviceState(AsyncWebServerRequest* request, bool info);
    void sendStateJson(AsyncWebSocketClient* client);
};

#endif
