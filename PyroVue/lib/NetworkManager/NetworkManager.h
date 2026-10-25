#ifndef NETWORK_MANAGER_H
#define NETWORK_MANAGER_H

#include <functional>
#include "Arduino.h"
#include "ESPAsyncWebServer.h"
#include "protocol.h"
#include "LoopRequestQueue.h"
#include "MonitorStateGate.h"

class HistoryStore;
class PresetManager;
class RunManager;

class NetworkManager {
public:
    NetworkManager(RunManager& run, HistoryStore& history, PresetManager& preset);
    void begin();
    void loop();
    void onNewSample(const TempSample& sample);
    size_t clientCount() const;
    std::function<NetworkCommandResult(const char*)> onCommand;
    void broadcastRunState();
#if defined(PYROVUE_SINGLE_RUN_CSV)
    void setCsvRecorder(csvrun::SingleRunCsv& recorder) { requests.setCsvRecorder(recorder); }
#endif

private:
    AsyncWebServer server;
    AsyncWebSocket ws;
    LoopRequestQueue requests;
    RunManager& runManager;
    HistoryStore& historyStore;
    PresetManager& presetManager;
    uint32_t nextSnapshotId;
    std::atomic<bool> replaying;
    MonitorStateGate monitorGate;

    void handleWsEvent(AsyncWebSocketClient* client, AwsEventType type,
                      void* arg, uint8_t* data, size_t len);
    void sendSnapshot(AsyncWebSocketClient* client);
    void sendError(const char* code);
    void sendDeviceState(AsyncWebServerRequest* request, bool info);
    void sendStateJson(AsyncWebSocketClient* client);
    void sendSampleJson(AsyncWebSocketClient* client, const TempSample& sample);
};

#endif
