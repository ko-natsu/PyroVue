#include "LoopRequestQueue.h"
#if defined(PYROVUE_SINGLE_RUN_CSV)
#include "config.h"
#include <ArduinoJson.h>
#include <stdio.h>
#include <string.h>
#include "SingleRunCsv.h"
#include "WebResponseImpl.h"

namespace {
class CsvResponse : public AsyncAbstractResponse {
public:
    CsvResponse(const std::shared_ptr<CsvDownloadBuffer>& source, size_t length, uint32_t runId)
        : buffer(source) {
        _code = 200;
        _contentType = "text/csv; charset=utf-8";
        _contentLength = length;
        _sendContentLength = true;
        _chunked = false;
        char filename[96];
        snprintf(filename, sizeof(filename), "attachment; filename=\"run-%lu-temperatures.csv\"",
                 static_cast<unsigned long>(runId));
        addHeader("Content-Disposition", filename);
        addHeader("Cache-Control", "no-store");
    }
    ~CsvResponse() override { buffer->cancelled.store(true, std::memory_order_release); }
    bool _sourceValid() const override { return !buffer->failed.load(std::memory_order_acquire); }
    size_t _fillBuffer(uint8_t* out, size_t maximum) override {
        if (maximum == 0) return 0;
        const size_t count = buffer->consume(out, maximum);
        if (count) return count;
        return buffer->finished.load(std::memory_order_acquire) ? 0 : RESPONSE_TRY_AGAIN;
    }
private:
    std::shared_ptr<CsvDownloadBuffer> buffer;
};
}

LoopRequestQueue::Job* LoopRequestQueue::reserve() {
#if defined(ARDUINO_ARCH_ESP8266)
    // LX106 has no CAS builtin; protect only the single-core slot reservation.
    noInterrupts();
    for (auto& job : jobs) {
        if (job.state.load(std::memory_order_relaxed) != SlotState::Free) continue;
        job.state.store(SlotState::Filling, std::memory_order_release);
        interrupts();
        return &job;
    }
    interrupts();
#else
    for (auto& job : jobs) {
        SlotState expected = SlotState::Free;
        if (job.state.compare_exchange_strong(expected, SlotState::Filling,
                                             std::memory_order_acquire)) return &job;
    }
#endif
    return nullptr;
}

void LoopRequestQueue::sendError(AsyncWebServerRequest* request, uint16_t status, const char* code) {
    char body[224];
    snprintf(body, sizeof(body),
             "{\"type\":\"error\",\"protocol\":%u,\"code\":\"%s\",\"message\":\"%s\"}",
             PROTOCOL_VERSION, code, code);
    AsyncWebServerResponse* response = request->beginResponse(status, "application/json", body);
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
}

bool LoopRequestQueue::enqueue(Kind kind, AsyncWebServerRequest* request, uint32_t runId,
                               const uint8_t* data, size_t length) {
    if (length > 64 || (length && (!data || memchr(data, 0, length)))) {
        if (request) sendError(request, 400, "INVALID_REQUEST");
        return false;
    }
    Job* job = reserve();
    if (!job) {
        if (request) sendError(request, 503, "BUSY");
        return false;
    }
    job->kind = kind;
#if defined(ARDUINO_ARCH_ESP8266)
    noInterrupts();
    job->ticket = nextTicket.load(std::memory_order_relaxed);
    nextTicket.store(job->ticket + 1, std::memory_order_relaxed);
    interrupts();
#else
    job->ticket = nextTicket.fetch_add(1, std::memory_order_relaxed);
#endif
    job->runId = runId;
    job->request = request ? request->pause() : AsyncWebServerRequestPtr{};
    if (length) memcpy(job->command, data, length);
    job->command[length] = '\0';
    job->state.store(SlotState::Ready, std::memory_order_release);
    return true;
}

void LoopRequestQueue::begin(AsyncWebServer& server) {
    server.on("/api/csv", HTTP_GET, [this](AsyncWebServerRequest* request) {
        enqueue(Kind::Info, request);
    });
    server.on("/api/run.csv", HTTP_GET, [this](AsyncWebServerRequest* request) {
        uint32_t runId;
        if (!parseRunId(request, runId)) sendError(request, 400, "INVALID_RUN_ID");
        else enqueue(Kind::Download, request, runId);
    });
    server.on("/api/run.csv", HTTP_DELETE, [this](AsyncWebServerRequest* request) {
        uint32_t runId;
        if (!parseRunId(request, runId, true)) sendError(request, 400, "INVALID_RUN_ID");
        else enqueue(Kind::Erase, request, runId);
    });
    server.on("/api/csv/format", HTTP_POST, [](AsyncWebServerRequest* request) {
        if (!request->contentLength()) sendError(request, 400, "CONFIRMATION_REQUIRED");
    }, nullptr, [this](AsyncWebServerRequest* request, uint8_t* data, size_t length,
                       size_t index, size_t total) {
        if (index) return;
        if (!total || length != total || total > 64) {
            sendError(request, 400, "INVALID_REQUEST");
            return;
        }
        enqueue(Kind::Format, request, 0, data, length);
    });
}

void LoopRequestQueue::execute(Job& job) {
    auto request = job.request.lock();
    const auto connected = [&request]() {
        return request && request->client() && request->client()->connected();
    };
    // Accepted mutations still execute after disconnect; abandoned GETs do no flash work.
    if (!connected() && (job.kind == Kind::Info || job.kind == Kind::Download)) return;
    if (!csv) {
        if (connected()) sendError(request.get(), 503, "CSV_UNAVAILABLE");
        return;
    }
    const auto status = csv->status();
    uint16_t code = 200;
    const char* error = nullptr;
    if (job.kind == Kind::Info) {
        JsonDocument doc;
        doc["supported"] = true;
        doc["mounted"] = status.mounted;
        doc["hasRun"] = status.hasRun;
        doc["recording"] = status.recording;
        doc["runId"] = status.runId;
        doc["sampleCount"] = status.sampleCount;
        doc["bytes"] = status.bytes;
        doc["freeBytes"] = status.freeBytes;
        doc["state"] = status.state;
        if (status.error) doc["error"] = status.error;
        else doc["error"] = nullptr;
        doc["logIntervalMs"] = csvrun::logIntervalMs;
        doc["maxDurationMs"] = csvrun::maxDurationMs;
        doc["startRequiredBytes"] = csvrun::startRequiredBytes;
        char json[512];
        const size_t length = serializeJson(doc, json, sizeof(json));
        if (length >= sizeof(json)) {
            sendError(request.get(), 500, "RESPONSE_TOO_LARGE");
            return;
        }
        json[length] = '\0';
        auto* response = request->beginResponse(200, "application/json", json);
        response->addHeader("Cache-Control", "no-store");
        request->send(response);
        return;
    }
    if (job.kind == Kind::Format) {
        JsonDocument doc;
        if (deserializeJson(doc, job.command) != DeserializationError::Ok ||
            !doc["confirm"].is<const char*>() ||
            doc["confirm"].as<JsonString>().size() != sizeof("ERASE_FFAT") - 1 ||
            strcmp(doc["confirm"], "ERASE_FFAT") != 0) {
            code = 400; error = "CONFIRMATION_REQUIRED";
        } else if (status.hasRun || status.recording || download) {
            code = 409; error = "RUN_OR_DOWNLOAD_EXISTS";
        } else if (!csv->format()) {
            code = 503; error = "FORMAT_FAILED";
        }
    } else if (!status.mounted) {
        code = 503; error = "STORAGE_UNAVAILABLE";
    } else if (!status.hasRun) {
        code = 404; error = "RUN_NOT_FOUND";
    } else if (status.runId != job.runId) {
        code = 409; error = "STALE_RUN_ID";
    } else if (status.recording || download) {
        code = 409; error = "RUN_OR_DOWNLOAD_ACTIVE";
    } else if (job.kind == Kind::Erase) {
        if (!csv->erase(job.runId)) { code = 503; error = "ERASE_FAILED"; }
    } else if (job.kind == Kind::Download) {
        if (!status.bytes || !csv->openDownload(job.runId)) {
            code = 503; error = "READ_FAILED";
        } else {
            download = std::make_shared<CsvDownloadBuffer>();
            downloadRemaining = status.bytes;
            pollDownload(); // First block ready before headers; callbacks only copy RAM.
            if (download->failed.load(std::memory_order_acquire)) {
                csv->closeDownload();
                download.reset();
                code = 503; error = "READ_FAILED";
            } else {
                if (!connected()) {
                    csv->closeDownload();
                    download.reset();
                    return;
                }
                request->send(new CsvResponse(download, status.bytes, job.runId));
                return;
            }
        }
    }
    if (connected()) {
        if (error) sendError(request.get(), code, error);
        else {
            auto* response = request->beginResponse(200, "application/json", "{\"ok\":true}");
            response->addHeader("Cache-Control", "no-store");
            request->send(response);
        }
    }
}

void LoopRequestQueue::poll() {
#if defined(PYROVUE_SINGLE_RUN_CSV)
    pollDownload();
#endif
    Job* oldest = nullptr;
    for (auto& job : jobs) {
        const auto state = job.state.load(std::memory_order_acquire);
        if (state == SlotState::Filling) return;
        if (state == SlotState::Ready &&
            (!oldest || static_cast<int32_t>(job.ticket - oldest->ticket) < 0)) oldest = &job;
    }
    if (!oldest) return;
    execute(*oldest);
    oldest->request.reset();
    oldest->state.store(SlotState::Free, std::memory_order_release);
}

#if defined(PYROVUE_SINGLE_RUN_CSV)
bool LoopRequestQueue::parseRunId(AsyncWebServerRequest* request, uint32_t& out, bool allowZero) {
    if (!request->hasParam("runId")) return false;
    const String& value = request->getParam("runId")->value();
    if (!value.length() || value.length() > 10) return false;
    uint32_t result = 0;
    for (size_t i = 0; i < value.length(); ++i) {
        const char c = value[i];
        if (c < '0' || c > '9' || result > (UINT32_MAX - (c - '0')) / 10) return false;
        result = result * 10 + (c - '0');
    }
    if (!result && !allowZero) return false;
    out = result;
    return true;
}

void LoopRequestQueue::pollDownload() {
    if (!download) return;
    if (download->cancelled.load(std::memory_order_acquire)) {
        csv->closeDownload();
        download.reset();
        return;
    }
    if (download->failed.load(std::memory_order_acquire) || !download->canFill()) return;
    if (!downloadRemaining) {
        download->finished.store(true, std::memory_order_release);
        return;
    }
    const size_t count = downloadRemaining < CsvDownloadBuffer::capacity
        ? downloadRemaining : CsvDownloadBuffer::capacity;
    const size_t read = csv->readDownload(download->bytes, count);
    if (!read || read > count) {
        download->failed.store(true, std::memory_order_release);
        return;
    }
    downloadRemaining -= read;
    download->publish(read);
}
#endif
#endif // PYROVUE_SINGLE_RUN_CSV
