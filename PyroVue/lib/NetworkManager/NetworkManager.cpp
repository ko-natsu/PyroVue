#include "NetworkManager.h"
#if defined(ARDUINO_ARCH_ESP8266)
#include <ESP8266WiFi.h>
#else
#include <WiFi.h>
#endif
#include "HistoryStore.h"
#include "RunManager.h"
#include "PresetManager.h"
#include "config.h"
#include "ui_index.h"
#include <ArduinoJson.h>
#include <string.h>

NetworkManager::NetworkManager(RunManager& run, HistoryStore& history, PresetManager& preset)
    : server(80), ws("/ws"), runManager(run), historyStore(history),
      presetManager(preset), nextSnapshotId(0), replaying(false) {}

void NetworkManager::sendStateJson(AsyncWebSocketClient* client) {
    JsonDocument doc;
    doc["type"] = "hello";
    doc["protocol"] = PROTOCOL_VERSION;
    doc["firmware"] = FIRMWARE_VERSION;
    doc["sampleHz"] = SAMPLE_HZ;
    doc["runId"] = runManager.runId();
    doc["runActive"] = runManager.active();
    doc["runStartMs"] = runManager.originMs();
    doc["recentWindowMs"] = RECENT_HISTORY_SECONDS * 1000UL;
    doc["coarseResolutionMs"] = COARSE_BUCKET_MS;
    doc["preset"] = presetManager.getPreset();
    doc["snapshotThroughSeq"] = historyStore.latestRawSequence();
    String json;
    serializeJson(doc, json);
    client->text(json);
}

void NetworkManager::sendSnapshot(AsyncWebSocketClient* client) {
    replaying = true;
    const uint32_t snapshotId = ++nextSnapshotId;
    const uint32_t throughSeq = historyStore.latestRawSequence();

    JsonDocument begin;
    begin["type"] = "history.begin";
    begin["protocol"] = PROTOCOL_VERSION;
    begin["snapshotId"] = snapshotId;
    begin["runId"] = runManager.runId();
    begin["throughSeq"] = throughSeq;
    String beginJson;
    serializeJson(begin, beginJson);
    client->text(beginJson);

    const size_t coarseCount = historyStore.coarseCount();
    const size_t coarseChunkSize = 8;
    size_t chunk = 0;
    for (size_t offset = 0; offset < coarseCount; offset += coarseChunkSize, ++chunk) {
        JsonDocument doc;
        doc["type"] = "history";
        doc["protocol"] = PROTOCOL_VERSION;
        doc["snapshotId"] = snapshotId;
        doc["runId"] = runManager.runId();
        doc["kind"] = "coarse";
        doc["resolutionMs"] = COARSE_BUCKET_MS;
        doc["chunk"] = chunk;
        doc["final"] = false;
        JsonArray samples = doc["samples"].to<JsonArray>();
        CoarseSample sample{};
        const size_t end = (offset + coarseChunkSize < coarseCount) ? offset + coarseChunkSize : coarseCount;
        for (size_t i = offset; i < end; ++i) {
            if (!historyStore.coarseAt(i, sample)) continue;
            JsonObject out = samples.add<JsonObject>();
            out["ms"] = sample.ms;
            out["avg"] = sample.averageC;
            out["min"] = sample.minimumC;
            out["max"] = sample.maximumC;
            out["fault"] = sample.fault;
        }
        String json;
        serializeJson(doc, json);
        client->text(json);
    }

    const size_t rawCount = historyStore.rawCount();
    const size_t rawChunkSize = 12;
    for (size_t offset = 0; offset < rawCount; offset += rawChunkSize, ++chunk) {
        JsonDocument doc;
        doc["type"] = "history";
        doc["protocol"] = PROTOCOL_VERSION;
        doc["snapshotId"] = snapshotId;
        doc["runId"] = runManager.runId();
        doc["kind"] = "raw";
        doc["resolutionMs"] = 1000UL / SAMPLE_HZ;
        doc["chunk"] = chunk;
        doc["final"] = false;
        JsonArray samples = doc["samples"].to<JsonArray>();
        TempSample sample{};
        const size_t end = (offset + rawChunkSize < rawCount) ? offset + rawChunkSize : rawCount;
        for (size_t i = offset; i < end; ++i) {
            if (!historyStore.rawAt(i, sample) || sample.seq > throughSeq) continue;
            JsonObject out = samples.add<JsonObject>();
            out["seq"] = sample.seq;
            out["ms"] = sample.ms;
            out["temp"] = sample.tempC;
            out["fault"] = sample.fault;
        }
        String json;
        serializeJson(doc, json);
        client->text(json);
    }

    uint32_t sentThrough = throughSeq;
    for (unsigned pass = 0; pass < 3; ++pass) {
        const uint32_t latest = historyStore.latestRawSequence();
        const size_t countNow = historyStore.rawCount();
        TempSample sample{};
        for (size_t i = 0; i < countNow; ++i) {
            if (historyStore.rawAt(i, sample) && sample.seq > sentThrough && sample.seq <= latest) {
                sendSampleJson(client, sample);
                sentThrough = sample.seq;
            }
        }
        if (sentThrough >= historyStore.latestRawSequence()) break;
    }
    replaying = false;

    JsonDocument end;
    end["type"] = "history.end";
    end["protocol"] = PROTOCOL_VERSION;
    end["snapshotId"] = snapshotId;
    end["runId"] = runManager.runId();
    end["throughSeq"] = sentThrough;
    String endJson;
    serializeJson(end, endJson);
    client->text(endJson);
}

void NetworkManager::sendSampleJson(AsyncWebSocketClient* client, const TempSample& sample) {
    JsonDocument doc;
    doc["type"] = "sample";
    doc["protocol"] = PROTOCOL_VERSION;
    doc["runId"] = sample.runId;
    doc["seq"] = sample.seq;
    doc["ms"] = sample.ms;
    doc["temp"] = sample.tempC;
    doc["fault"] = sample.fault;
    String json;
    serializeJson(doc, json);
    client->text(json);
}

void NetworkManager::broadcastRunState() {
    JsonDocument doc;
    doc["type"] = "state";
    doc["protocol"] = PROTOCOL_VERSION;
    doc["runId"] = runManager.runId();
    doc["runActive"] = runManager.active();
    String json;
    serializeJson(doc, json);
    ws.textAll(json);
}

void NetworkManager::sendError(const char* code) {
    JsonDocument doc;
    doc["type"] = "error";
    doc["protocol"] = PROTOCOL_VERSION;
    doc["code"] = code;
    String json;
    serializeJson(doc, json);
    ws.textAll(json);
}

void NetworkManager::handleWsEvent(AsyncWebSocketClient* client, AwsEventType type,
                                   void* arg, uint8_t* data, size_t len) {
    if (type == WS_EVT_CONNECT) {
        if (!monitorGate.tryEnter()) {
            client->close(1013, "BUSY");
            return;
        }
        sendStateJson(client);
        sendSnapshot(client);
        monitorGate.leave();
        return;
    }
    if (type == WS_EVT_DATA) {
        AwsFrameInfo* info = static_cast<AwsFrameInfo*>(arg);
        if (info && info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
            if (!requests.enqueueCommand(data, len)) sendError("BUSY_OR_INVALID_COMMAND");
        }
    }
}

void NetworkManager::sendDeviceState(AsyncWebServerRequest* request, bool info) {
    JsonDocument doc;
    doc["protocol"] = PROTOCOL_VERSION;
    doc["runId"] = runManager.runId();
    doc["runActive"] = runManager.active();
    if (info) {
        doc["type"] = "hello";
        doc["firmware"] = FIRMWARE_VERSION;
        doc["sampleHz"] = SAMPLE_HZ;
        doc["clients"] = ws.count();
        doc["preset"] = presetManager.getPreset();
    } else {
        doc["type"] = "state";
        TempSample sample{};
        if (historyStore.latestReading(sample)) {
            doc["type"] = "sample";
            doc["runId"] = sample.runId;
            doc["seq"] = sample.seq;
            doc["ms"] = sample.ms;
            doc["temp"] = sample.tempC;
            doc["fault"] = sample.fault;
        }
    }
    auto* response = request->beginResponseStream("application/json", 512);
    serializeJson(doc, *response);
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
}

void NetworkManager::begin() {
    WiFi.softAP(WIFI_SSID, WIFI_PASSWORD, AP_CHANNEL);
    server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
        AsyncWebServerResponse* response = request->beginResponse(200, "text/html", UI_INDEX_GZ, UI_INDEX_GZ_LEN);
        response->addHeader("Content-Encoding", "gzip");
        request->send(response);
    });
    requests.begin(server, onCommand, [this]() { broadcastRunState(); },
                   [this](const char* code) { sendError(code); },
                   [this]() { return monitorGate.tryEnter(); },
                   [this]() { monitorGate.leave(); },
                   [this](AsyncWebServerRequest* request, bool info) { sendDeviceState(request, info); });
    ws.onEvent([this](AsyncWebSocket* serverRef, AsyncWebSocketClient* client,
                     AwsEventType type, void* arg, uint8_t* data, size_t len) {
        (void)serverRef;
        handleWsEvent(client, type, arg, data, len);
    });
    server.addHandler(&ws);
    server.begin();
}

void NetworkManager::loop() {
    requests.poll();
    ws.cleanupClients(WS_MAX_CLIENTS);
}
void NetworkManager::onNewSample(const TempSample& sample) {
    if (replaying || ws.count() == 0) return;
    JsonDocument doc;
    doc["type"] = "sample";
    doc["protocol"] = PROTOCOL_VERSION;
    doc["runId"] = sample.runId;
    doc["seq"] = sample.seq;
    doc["ms"] = sample.ms;
    doc["temp"] = sample.tempC;
    doc["fault"] = sample.fault;
    String json;
    serializeJson(doc, json);
    ws.textAll(json);
}

size_t NetworkManager::clientCount() const { return ws.count(); }
