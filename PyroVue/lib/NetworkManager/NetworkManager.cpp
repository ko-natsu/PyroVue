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
#if defined(PYROVUE_BOARD_N16R8)
#include "RunLogService.h"
#include <esp_heap_caps.h>
#include <esp32s3/spiram.h>
#endif

namespace {
void writeStatus(JsonDocument& doc, const NetworkStatusSnapshot& status) {
    doc["protocol"] = PROTOCOL_VERSION;
    doc["firmware"] = status.firmware;
    doc["deviceId"] = status.identity.deviceId;
    doc["bootId"] = status.identity.bootId;
    doc["capabilities"]["runArchive"] = status.runArchive;
    doc["sampleHz"] = status.sampleHz;
    doc["acquireIntervalMs"] = status.acquireIntervalMs;
    doc["logIntervalMs"] = status.logIntervalMs;
    doc["maxRunMs"] = status.maxRunMs;
    doc["runId"] = status.runId;
    doc["runActive"] = status.runActive;
    doc["runStartMs"] = status.runStartMs;
    doc["recentWindowMs"] = status.recentWindowMs;
    doc["coarseResolutionMs"] = status.coarseResolutionMs;
    doc["snapshotThroughSeq"] = status.snapshotThroughSeq;
    doc["preset"] = status.preset;
    doc["clients"] = status.clients;
    const char* state = "unavailable";
    switch (status.logging.state) {
        case LoggingState::Idle: state = "idle"; break;
        case LoggingState::Recording: state = "recording"; break;
        case LoggingState::Error: state = "error"; break;
        case LoggingState::Unavailable: break;
    }
    JsonObject logging = doc["logging"].to<JsonObject>();
    logging["state"] = state;
    logging["durableLogSeq"] = status.logging.durableLogSeq;
    logging["latestAcqSeq"] = status.logging.latestAcqSeq;
    logging["bufferedRecords"] = status.logging.bufferedRecords;
    logging["acquisitionGaps"] = status.logging.acquisitionGaps;
    logging["droppedAcquired"] = status.logging.droppedAcquired;
    logging["queueHighWater"] = status.logging.queueHighWater;
    logging["errorCode"] = archiveErrorName(status.logging.error);
    JsonObject storage = doc["storage"].to<JsonObject>();
    storage["mounted"] = status.storage.mounted;
    storage["totalBytes"] = status.storage.totalBytes;
    storage["usedBytes"] = status.storage.usedBytes;
    storage["freeBytes"] = status.storage.freeBytes;
    storage["reservedRemainingBytes"] = status.storage.reservedRemainingBytes;
    storage["startRequiredBytes"] = status.storage.startRequiredBytes;
#if defined(PYROVUE_BOARD_N16R8)
    JsonObject hardware = doc["hardware"].to<JsonObject>();
    hardware["flashBytes"] = ESP.getFlashChipSize();
    hardware["psramBytes"] = esp_spiram_is_initialized() ? esp_spiram_get_size() : 0;
    hardware["psramHeapBytes"] = ESP.getPsramSize();
    hardware["psramLargestBlock"] = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    hardware["filesystemLabel"] = "spiffs";
#endif
}

void sendJson(AsyncWebSocketClient* client, const JsonDocument& doc) {
    String json;
    serializeJson(doc, json);
    client->text(json);
}
}

NetworkManager::NetworkManager(RunManager& run, HistoryStore& history, PresetManager& preset)
    : server(80), ws("/ws"), runManager(run), historyStore(history), presetManager(preset) {}

void NetworkManager::publishStatus() {
    NetworkStatusSnapshot status{};
#if defined(PYROVUE_BOARD_N16R8)
    if (runLogger) runLogger->copyPublishedStatus(status);
#endif
    status.identity = deviceIdentity;
    snprintf(status.firmware, sizeof(status.firmware), "%s", FIRMWARE_VERSION);
    status.protocolVersion = PROTOCOL_VERSION;
    status.sampleHz = SAMPLE_HZ;
    status.runId = runManager.runId();
    status.runActive = runManager.active();
    status.runStartMs = runManager.originMs();
    status.acquireIntervalMs = 1000u / SAMPLE_HZ;
    status.recentWindowMs = RECENT_HISTORY_SECONDS * 1000u;
    status.coarseResolutionMs = COARSE_BUCKET_MS;
    status.snapshotThroughSeq = historyStore.latestRawSequence();
    status.clients = ws.count();
    const String preset = presetManager.getPreset();
    snprintf(status.preset, sizeof(status.preset), "%s", preset.c_str());
    TempSample reading{};
    const bool hasReading = historyStore.latestReading(reading);
#if defined(ARDUINO_ARCH_ESP32)
    portENTER_CRITICAL(&statusMux);
#else
    noInterrupts();
#endif
    published = status;
    publishedReading = reading;
    hasPublishedReading = hasReading;
#if defined(ARDUINO_ARCH_ESP32)
    portEXIT_CRITICAL(&statusMux);
#else
    interrupts();
#endif
}

void NetworkManager::copyStatus(NetworkStatusSnapshot& out, TempSample& reading, bool& hasReading) const {
#if defined(ARDUINO_ARCH_ESP32)
    portENTER_CRITICAL(&statusMux);
#else
    noInterrupts();
#endif
    out = published;
    reading = publishedReading;
    hasReading = hasPublishedReading;
#if defined(ARDUINO_ARCH_ESP32)
    portEXIT_CRITICAL(&statusMux);
#else
    interrupts();
#endif
}

void NetworkManager::sendStateJson(AsyncWebSocketClient* client) {
    NetworkStatusSnapshot status{};
    TempSample reading{};
    bool hasReading;
    copyStatus(status, reading, hasReading);
    JsonDocument doc;
    doc["type"] = "hello";
    writeStatus(doc, status);
    sendJson(client, doc);
}

void NetworkManager::startReplay(uint32_t clientId) {
    auto* client = ws.client(clientId);
    if (!client || client->status() != WS_CONNECTED) return;
    replay = Replay{};
    replay.clientId = clientId;
    replay.snapshotId = ++nextSnapshotId;
    replay.runId = runManager.runId();
    replay.throughSeq = historyStore.latestRawSequence();
    CoarseSample last{};
    const size_t count = historyStore.coarseCount();
    const bool hasCoarse = count && historyStore.coarseAt(count - 1, last);
    replay.throughCoarseMs = hasCoarse ? last.ms : 0;
    replay.phase = hasCoarse ? Replay::Phase::Coarse : Replay::Phase::Raw;
    sendStateJson(client);
    JsonDocument doc;
    doc["type"] = "history.begin";
    doc["protocol"] = PROTOCOL_VERSION;
    doc["snapshotId"] = replay.snapshotId;
    doc["runId"] = replay.runId;
    doc["throughSeq"] = replay.throughSeq;
    sendJson(client, doc);
}

void NetworkManager::pollReplay() {
#if defined(ARDUINO_ARCH_ESP8266)
    noInterrupts();
    const uint32_t pending = pendingReplayClient.load(std::memory_order_relaxed);
    pendingReplayClient.store(0, std::memory_order_relaxed);
    interrupts();
#else
    const uint32_t pending = pendingReplayClient.exchange(0, std::memory_order_acq_rel);
#endif
    if (pending) {
        startReplay(pending);
        return;
    }
    if (replay.phase == Replay::Phase::Idle) return;
    auto* client = ws.client(replay.clientId);
    if (!client || client->status() != WS_CONNECTED) {
        replay.phase = Replay::Phase::Idle;
        return;
    }
    if (replay.runId != runManager.runId()) {
        startReplay(replay.clientId);
        return;
    }
    if (client->queueIsFull()) return;
    JsonDocument doc;
    doc["protocol"] = PROTOCOL_VERSION;
    doc["snapshotId"] = replay.snapshotId;
    doc["runId"] = replay.runId;
    if (replay.phase == Replay::Phase::End) {
        doc["type"] = "history.end";
        doc["throughSeq"] = replay.throughSeq;
        sendJson(client, doc);
        replay.phase = Replay::Phase::Idle;
        return;
    }
    doc["type"] = "history";
    doc["chunk"] = replay.chunk;
    doc["final"] = false;
    JsonArray samples = doc["samples"].to<JsonArray>();
    if (replay.phase == Replay::Phase::Coarse) {
        CoarseSample page[8];
        const size_t count = historyStore.copyCoarseFrom(replay.fromCoarseMs,
            replay.throughCoarseMs, page, 8);
        if (!count) {
            replay.phase = Replay::Phase::Raw;
            return;
        }
        doc["kind"] = "coarse";
        doc["resolutionMs"] = COARSE_BUCKET_MS;
        for (size_t i = 0; i < count; ++i) {
            JsonObject out = samples.add<JsonObject>();
            out["ms"] = page[i].ms;
            out["avg"] = page[i].averageC;
            out["min"] = page[i].minimumC;
            out["max"] = page[i].maximumC;
            out["fault"] = page[i].fault;
        }
        replay.fromCoarseMs = page[count - 1].ms + COARSE_BUCKET_MS;
        if (count < 8 || page[count - 1].ms >= replay.throughCoarseMs)
            replay.phase = Replay::Phase::Raw;
    } else {
        TempSample page[12];
        const size_t count = historyStore.copyRawAfter(replay.afterSeq,
            replay.throughSeq, page, 12);
        if (!count) {
            replay.phase = Replay::Phase::End;
            return;
        }
        doc["kind"] = "raw";
        doc["resolutionMs"] = 1000u / SAMPLE_HZ;
        for (size_t i = 0; i < count; ++i) {
            JsonObject out = samples.add<JsonObject>();
            out["seq"] = page[i].seq;
            out["ms"] = page[i].ms;
            out["temp"] = page[i].tempC;
            out["fault"] = page[i].fault;
        }
        replay.afterSeq = page[count - 1].seq;
        if (count < 12 || replay.afterSeq >= replay.throughSeq)
            replay.phase = Replay::Phase::End;
    }
    ++replay.chunk;
    sendJson(client, doc);
}

void NetworkManager::broadcastRunState() {
    publishStatus();
    JsonDocument doc;
    doc["type"] = "state";
    doc["protocol"] = PROTOCOL_VERSION;
    doc["runId"] = runManager.runId();
    doc["runActive"] = runManager.active();
    String json;
    serializeJson(doc, json);
    ws.textAll(json);
}

void NetworkManager::handleWsEvent(AsyncWebSocketClient* client, AwsEventType type,
                                   void* arg, uint8_t* data, size_t len) {
    (void)data;
    (void)len;
    if (type == WS_EVT_CONNECT) {
        if (ws.count() > WS_MAX_CLIENTS) {
            client->close(1013, "BUSY");
            return;
        }
        pendingReplayClient.store(client->id(), std::memory_order_release);
    } else if (type == WS_EVT_DATA) {
        const auto* info = static_cast<AwsFrameInfo*>(arg);
        if (info && info->opcode == WS_TEXT) {
            JsonDocument doc;
            doc["type"] = "error";
            doc["protocol"] = PROTOCOL_VERSION;
            doc["code"] = "HTTP_COMMAND_REQUIRED";
            doc["message"] = "Send commands to POST /api/cmd";
            sendJson(client, doc);
        }
    }
}

void NetworkManager::sendDeviceState(AsyncWebServerRequest* request, bool info) {
    NetworkStatusSnapshot status{};
    TempSample reading{};
    bool hasReading;
    copyStatus(status, reading, hasReading);
    JsonDocument doc;
    if (info) {
        doc["type"] = "hello";
        writeStatus(doc, status);
    } else {
        doc["type"] = hasReading ? "sample" : "state";
        doc["protocol"] = PROTOCOL_VERSION;
        doc["runId"] = status.runId;
        doc["runActive"] = status.runActive;
        if (hasReading) {
            doc["runId"] = reading.runId;
            doc["seq"] = reading.seq;
            doc["ms"] = reading.ms;
            doc["temp"] = reading.tempC;
            doc["fault"] = reading.fault;
        }
    }
    auto* response = request->beginResponseStream("application/json", 1024);
    serializeJson(doc, *response);
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
}

void NetworkManager::begin() {
    DefaultHeaders::Instance().addHeader("Cache-Control", "no-store");
    WiFi.softAP(WIFI_SSID, WIFI_PASSWORD, AP_CHANNEL);
    publishStatus();
    server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
        auto* response = request->beginResponse(200, "text/html", UI_INDEX_GZ, UI_INDEX_GZ_LEN);
        response->addHeader("Content-Encoding", "gzip");
        response->addHeader("Cache-Control", "no-store");
        request->send(response);
    });
    server.on("/api/info", HTTP_GET, [this](AsyncWebServerRequest* request) {
        sendDeviceState(request, true);
    });
    server.on("/api/now", HTTP_GET, [this](AsyncWebServerRequest* request) {
        sendDeviceState(request, false);
    });
    archiveRoutes.setIdentity(deviceIdentity);
    archiveRoutes.onStateChanged([this]() { broadcastRunState(); });
    archiveRoutes.begin(server, onCommand, runLogger);
#if defined(PYROVUE_SINGLE_RUN_CSV)
    csvRequests.begin(server);
#endif
    ws.onEvent([this](AsyncWebSocket*, AsyncWebSocketClient* client,
                     AwsEventType type, void* arg, uint8_t* data, size_t len) {
        handleWsEvent(client, type, arg, data, len);
    });
    server.addHandler(&ws);
    server.begin();
}

void NetworkManager::loop() {
    archiveRoutes.poll(millis());
#if defined(PYROVUE_SINGLE_RUN_CSV)
    csvRequests.poll();
#endif
    publishStatus();
    pollReplay();
    ws.cleanupClients(WS_MAX_CLIENTS);
}

void NetworkManager::onNewSample(const TempSample& sample) {
    if (ws.count() == 0) return;
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
