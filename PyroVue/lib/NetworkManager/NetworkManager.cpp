#include "NetworkManager.h"
#include "PresetManager.h"
#include "config.h"
#include "ui_index.h"
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
AsyncEventSource events("/events");

// Need to have a pointer to the NetworkManager instance to be able to access it from the ws handler
NetworkManager* nm_instance = nullptr;

void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
    if (type == WS_EVT_CONNECT) {
        Serial.printf("WebSocket client #%u connected from %s\n", client->id(), client->remoteIP().toString().c_str());

        // Send hello message
        JsonDocument hello_doc;
        hello_doc["type"] = "hello";
        hello_doc["fw"] = PROTOCOL_VERSION;
        hello_doc["rate"] = SAMPLE_HZ;
        hello_doc["buf"] = UI_HISTORY_ON_CONNECT;
        JsonObject state = hello_doc["state"].to<JsonObject>();
        if (nm_instance) {
            state["preset"] = nm_instance->getPresetManager().getPreset();
        }

        String hello_json;
        serializeJson(hello_doc, hello_json);
        client->text(hello_json);

        // Send history
        if (nm_instance) {
            std::vector<uint32_t> ms;
            std::vector<float> t;
            std::vector<uint8_t> fault;
            nm_instance->getBuffer().snapshot(UI_HISTORY_ON_CONNECT, ms, t, fault);

            JsonDocument hist_doc;
            hist_doc["type"] = "hist";
            JsonArray ms_array = hist_doc["ms"].to<JsonArray>();
            for(uint32_t val : ms) ms_array.add(val);
            JsonArray t_array = hist_doc["t"].to<JsonArray>();
            for(float val : t) t_array.add(val);
            JsonArray fault_array = hist_doc["fault"].to<JsonArray>();
            for(uint8_t val : fault) fault_array.add(val);

            String hist_json;
            serializeJson(hist_doc, hist_json);
            client->text(hist_json);
        }

    } else if (type == WS_EVT_DISCONNECT) {
        Serial.printf("WebSocket client #%u disconnected\n", client->id());
    } else if (type == WS_EVT_DATA) {
        AwsFrameInfo *info = (AwsFrameInfo*)arg;
        if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
            data[len] = 0;
            if (nm_instance && nm_instance->onCommand) {
                nm_instance->onCommand((char*)data);
            }
        }
    }
}

NetworkManager::NetworkManager(TelemetryBuffer& buffer, PresetManager& presetMgr) : _buffer(&buffer), _presetManager(&presetMgr) {
    nm_instance = this;
}

void NetworkManager::begin() {
    // Start WiFi AP
    WiFi.softAP(WIFI_SSID, WIFI_PASSWORD, AP_CHANNEL);
    Serial.print("AP IP address: ");
    Serial.println(WiFi.softAPIP());

    // Serve Gzipped UI
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        AsyncWebServerResponse *response = request->beginResponse(200, "text/html", UI_INDEX_GZ, UI_INDEX_GZ_LEN);
        response->addHeader("Content-Encoding", "gzip");
        request->send(response);
    });

    // API endpoints
    server.on("/api/now", HTTP_GET, [this](AsyncWebServerRequest *request){
        TempSample s = _buffer->getLastSample();
        JsonDocument doc;
        doc["type"] = "tick";
        doc["ms"] = s.ms;
        doc["t"] = s.t_c;
        doc["fault"] = s.fault;
        String json;
        serializeJson(doc, json);
        request->send(200, "application/json", json);
    });

    server.on("/api/info", HTTP_GET, [this](AsyncWebServerRequest *request){
        JsonDocument doc;
        doc["fw"] = PROTOCOL_VERSION;
        doc["clients"] = ws.count();
        JsonObject state = doc["state"].to<JsonObject>();
        state["preset"] = _presetManager->getPreset();
        String json;
        serializeJson(doc, json);
        request->send(200, "application/json", json);
    });

    // Command endpoint
    server.on("/api/cmd", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL, [this](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
        if (index == 0 && len == total) {
            if (onCommand) {
                char* str_data = new char[len + 1];
                memcpy(str_data, data, len);
                str_data[len] = 0;
                onCommand(str_data);
                delete[] str_data;
            }
            request->send(200, "text/plain", "OK");
        } else {
            request->send(400, "text/plain", "Bad Request");
        }
    });

    // WebSocket event handlers
    ws.onEvent(onWsEvent);
    server.addHandler(&ws);

    // SSE event handlers
    server.addHandler(&events);

    // Start server
    server.begin();
}

void NetworkManager::loop() {
    ws.cleanupClients();
}

void NetworkManager::onNewSample(const TempSample& s) {
    JsonDocument doc;
    doc["type"] = "tick";
    doc["ms"] = s.ms;
    doc["t"] = s.t_c;
    doc["fault"] = s.fault;

    String json;
    serializeJson(doc, json);
    ws.textAll(json);
    events.send(json.c_str(), "tick", millis());
}

size_t NetworkManager::clientCount() const {
    return ws.count() + events.count();
}
