#ifndef NETWORK_MANAGER_H
#define NETWORK_MANAGER_H

#include <functional>
#include "Arduino.h"
#include "ESPAsyncWebServer.h"
#include "protocol.h"

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
    std::function<bool(const String&)> onCommand;

private:
    AsyncWebServer server;
    AsyncWebSocket ws;
    RunManager& runManager;
    HistoryStore& historyStore;
    PresetManager& presetManager;
    uint32_t nextSnapshotId;
    volatile bool replaying;

    void handleWsEvent(AsyncWebSocketClient* client, AwsEventType type,
                      void* arg, uint8_t* data, size_t len);
    void sendSnapshot(AsyncWebSocketClient* client);
    void sendCommand(const uint8_t* data, size_t len);
    void sendStateJson(AsyncWebSocketClient* client);
    void sendSampleJson(AsyncWebSocketClient* client, const TempSample& sample);
    void broadcastRunState();
};

#endif
