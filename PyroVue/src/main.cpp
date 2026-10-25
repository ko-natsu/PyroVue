#include <Arduino.h>
#include "config.h"
#include "protocol.h"
#include "SensorService.h"
#include "RunManager.h"
#include "HistoryStore.h"
#include "NetworkManager.h"
#include "RunLogTypes.h"
#if defined(ARDUINO_ARCH_ESP8266)
#include <ESP8266WiFi.h>
#else
#include <WiFi.h>
#include <esp_mac.h>
#include <esp_system.h>
#endif
#if !defined(PYROVUE_BOARD_N16R8) && !defined(PYROVUE_SINGLE_RUN_CSV)
#include "LittleFsHistoryPersistence.h"
#endif
#if defined(PYROVUE_SINGLE_RUN_CSV)
#include "SingleRunCsv.h"
#include "FeatherCsvStorage.h"
#endif
#if defined(PYROVUE_BOARD_N16R8)
#include "RunArchiveStore.h"
#include "RunLogService.h"
#include <esp_heap_caps.h>
#include <esp32s3/spiram.h>
#endif
#include "PresetManager.h"

#if defined(PYROVUE_HAS_DISPLAY)
#include <SPI.h>
#include "Adafruit_ST7789.h"
#include "DisplayManager.h"
#include "StatusManager.h"
#endif

#if defined(ARDUINO)
#if defined(ARDUINO_ARCH_ESP8266)
#include <EEPROM.h>
class PreferencesRunIdStore : public IRunIdStore {
public:
    void begin() { EEPROM.begin(8); }
    bool loadRunId(uint32_t& out) {
        EEPROM.get(0, out);
        return out != 0 && out != 0xffffffffUL;
    }
    bool saveRunId(uint32_t id) {
        EEPROM.put(0, id);
        return EEPROM.commit();
    }
};
#else
#include <Preferences.h>
class PreferencesRunIdStore : public IRunIdStore {
public:
    void begin() { preferences.begin("pyrovue", false); }
    bool loadRunId(uint32_t& out) override {
        if (!preferences.isKey("runId")) return false;
        out = preferences.getUInt("runId", 0);
        return out != 0;
    }
    bool saveRunId(uint32_t id) override { return preferences.putUInt("runId", id) == sizeof(uint32_t); }
private:
    Preferences preferences;
};
#endif

#if defined(PYROVUE_HAS_DISPLAY)
SPIClass spiDisplay(FSPI);
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);
DisplayManager displayManager(tft);
StatusManager statusManager(displayManager);
#endif

PreferencesRunIdStore runIdStore;
Max31855Sensor thermocouple;
RunManager runManager(&runIdStore);
DeviceIdentity deviceIdentity;
#if defined(PYROVUE_BOARD_N16R8)
RunArchiveStore runArchive;
#elif defined(PYROVUE_SINGLE_RUN_CSV)
csvrun::FeatherCsvStorage csvStorage;
csvrun::SingleRunCsv csvRecorder(csvStorage);
#else
LittleFsHistoryPersistence historyPersistence;
#endif
#if defined(ARDUINO_ARCH_ESP8266)
TempSample rawStorage[ESP8266_RAW_HISTORY_CAPACITY];
#else
TempSample rawStorage[RAW_HISTORY_CAPACITY];
#endif
CoarseSample coarseStorage[COARSE_HISTORY_CAPACITY];
HistoryStore historyStore(rawStorage,
#if defined(ARDUINO_ARCH_ESP8266)
                          ESP8266_RAW_HISTORY_CAPACITY,
#else
                          RAW_HISTORY_CAPACITY,
#endif
                          coarseStorage, COARSE_HISTORY_CAPACITY, COARSE_BUCKET_MS);
#if defined(PYROVUE_BOARD_N16R8)
RunLogService runLogger(runManager, historyStore, runArchive, deviceIdentity);
#endif
PresetManager presetManager;
NetworkManager networkManager(runManager, historyStore, presetManager);
#if !defined(PYROVUE_BOARD_N16R8) && !defined(PYROVUE_SINGLE_RUN_CSV)
uint32_t persistedBucketMs = 0xffffffffUL;
static void persistLatestCoarse() {
    const size_t count = historyStore.coarseCount();
    if (count == 0) return;
    CoarseSample sample{};
    if (!historyStore.coarseAt(count - 1, sample) || sample.ms == persistedBucketMs) return;
    if (historyPersistence.append(sample)) persistedBucketMs = sample.ms;
}
#endif

class SampleRouter : public ISampleSink {
public:
    void onSample(const TempSample& sample) override {
        historyStore.push(sample);
#if defined(PYROVUE_BOARD_N16R8)
        runLogger.enqueueSample(sample);
#endif
#if !defined(PYROVUE_BOARD_N16R8) && !defined(PYROVUE_SINGLE_RUN_CSV)
        persistLatestCoarse();
#endif
        networkManager.onNewSample(sample);
#if defined(PYROVUE_HAS_DISPLAY)
        statusManager.update(sample.tempC, sample.fault);
#endif
#if defined(PYROVUE_SINGLE_RUN_CSV)
        csvRecorder.onSample(sample);
#endif
    }
};

SampleRouter sampleRouter;
SensorService sensorService(thermocouple, runManager, sampleRouter, SAMPLE_HZ);

static CommandResult monitorResult(const CommandRequest& request, ArchiveError error,
                                   uint32_t targetRunId = 0) {
    CommandResult result{};
    memcpy(result.requestId, request.requestId, sizeof(result.requestId));
    result.ok = error == ArchiveError::NONE;
    result.error = error;
    result.runId = runManager.runId();
    result.runActive = runManager.active();
    result.targetRunId = targetRunId;
    return result;
}

static CommandResult handleCommand(const CommandRequest& request) {
    if (request.command == CommandKind::Preset) {
        if (strcmp(request.bootId, deviceIdentity.bootId) != 0)
            return monitorResult(request, ArchiveError::STALE_COMMAND);
        presetManager.setPreset(request.value);
        return monitorResult(request, ArchiveError::NONE);
    }
#if defined(PYROVUE_BOARD_N16R8)
    return runLogger.handleCommand(request);
#else
    static char lastStartRequestId[81]{};
    static uint32_t lastStartRunId = 0;
    if (request.command == CommandKind::Start && lastStartRunId &&
        strcmp(request.requestId, lastStartRequestId) == 0)
        return monitorResult(request, ArchiveError::NONE, lastStartRunId);
    if (strcmp(request.bootId, deviceIdentity.bootId) != 0)
        return monitorResult(request, ArchiveError::STALE_COMMAND, request.targetRunId);
    if (request.command == CommandKind::Start) {
        if (request.afterRunId != runManager.runId())
            return monitorResult(request, ArchiveError::STALE_COMMAND);
        if (runManager.active())
            return monitorResult(request, ArchiveError::RUN_ACTIVE);
#if defined(PYROVUE_SINGLE_RUN_CSV)
        if (!csvRecorder.canStart()) {
            const auto status = csvRecorder.status();
            const ArchiveError error = status.hasRun ? ArchiveError::RUN_ID_CONFLICT :
                !status.mounted ? ArchiveError::STORAGE_UNAVAILABLE :
                status.error ? ArchiveError::WRITE_FAILED : ArchiveError::INSUFFICIENT_STORAGE;
            return monitorResult(request, error);
        }
        const auto start = runManager.prepareRun(millis());
#else
        const auto start = runManager.startRun(millis());
#endif
        if (start == RunManager::START_STORE_FAILED)
            return monitorResult(request, ArchiveError::RUN_ID_SAVE_FAILED);
        if (start == RunManager::START_ID_EXHAUSTED)
            return monitorResult(request, ArchiveError::RUN_ID_EXHAUSTED);
        if (start != RunManager::START_STARTED)
            return monitorResult(request, ArchiveError::RUN_ACTIVE);
#if defined(PYROVUE_SINGLE_RUN_CSV)
        if (!csvRecorder.start(runManager.runId())) {
            runManager.abandonPreparedRun();
            return monitorResult(request, ArchiveError::WRITE_FAILED);
        }
        if (!runManager.activatePreparedRun()) {
            csvRecorder.stop(0);
            runManager.abandonPreparedRun();
            return monitorResult(request, ArchiveError::WRITE_FAILED);
        }
#endif
        historyStore.clear(runManager.runId());
#if !defined(PYROVUE_SINGLE_RUN_CSV)
        persistedBucketMs = 0xffffffffUL;
        historyPersistence.startRun(runManager.runId(), COARSE_BUCKET_MS);
#endif
        memcpy(lastStartRequestId, request.requestId, sizeof(lastStartRequestId));
        lastStartRunId = runManager.runId();
        return monitorResult(request, ArchiveError::NONE, lastStartRunId);
    }
    if (request.command != CommandKind::Stop)
        return monitorResult(request, ArchiveError::INVALID_REQUEST);
    if (!request.targetRunId || request.targetRunId != runManager.runId())
        return monitorResult(request, ArchiveError::STALE_COMMAND, request.targetRunId);
    if (!runManager.active())
        return monitorResult(request, ArchiveError::NONE, request.targetRunId);
    bool saved = true;
#if defined(PYROVUE_SINGLE_RUN_CSV)
    saved = csvRecorder.stop(runManager.elapsedMs(millis()));
#endif
    runManager.stopRun();
    historyStore.flush();
#if !defined(PYROVUE_SINGLE_RUN_CSV)
    persistLatestCoarse();
    historyPersistence.flush();
#endif
    return monitorResult(request, saved ? ArchiveError::NONE : ArchiveError::WRITE_FAILED,
                         request.targetRunId);
#endif
}
#endif

#if defined(PYROVUE_BOARD_N16R8)
static void printHardwareStatus() {
    NetworkStatusSnapshot status{};
    runLogger.copyPublishedStatus(status);
    Serial.printf("N16R8 flash=%lu psram=%lu fs=%s\n",
                  static_cast<unsigned long>(ESP.getFlashChipSize()),
                  static_cast<unsigned long>(esp_spiram_is_initialized() ? esp_spiram_get_size() : 0),
                  status.storage.mounted ? "mounted" : "unavailable");
    Serial.printf("N16R8 fs_label=spiffs total=%lu used=%lu psram_largest=%lu\n",
                  static_cast<unsigned long>(status.storage.totalBytes),
                  static_cast<unsigned long>(status.storage.usedBytes),
                  static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
    Serial.printf("N16R8 run=%lu active=%u durable_records=%lu error=%s\n",
                  static_cast<unsigned long>(status.runId), status.runActive,
                  static_cast<unsigned long>(status.logging.durableLogSeq),
                  archiveErrorName(status.logging.error) ? archiveErrorName(status.logging.error) : "none");
    const char* startError = archiveErrorName(runArchive.checkStart());
    Serial.printf("N16R8 start=%s psram_heap=%lu\n", startError ? startError : "ready",
                  static_cast<unsigned long>(ESP.getPsramSize()));
}
#endif

void setup() {
    Serial.begin(115200);
#if defined(ARDUINO)
    runIdStore.begin();
#if !defined(PYROVUE_SINGLE_RUN_CSV) && !defined(PYROVUE_BOARD_N16R8)
    runManager.begin(millis());
#endif
    uint8_t mac[6]{};
#if defined(ARDUINO_ARCH_ESP8266)
    WiFi.macAddress(mac);
    const uint32_t bootHigh = ESP.random(), bootLow = ESP.random();
#else
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    const uint32_t bootHigh = esp_random(), bootLow = esp_random();
#endif
    snprintf(deviceIdentity.deviceId, sizeof(deviceIdentity.deviceId),
             "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(deviceIdentity.bootId, sizeof(deviceIdentity.bootId), "%08lx%08lx",
             static_cast<unsigned long>(bootHigh), static_cast<unsigned long>(bootLow));
    networkManager.setIdentity(deviceIdentity);
#if defined(PYROVUE_BOARD_N16R8)
    const ArchiveError archiveError = runLogger.begin(millis());
    networkManager.setRunLogger(runLogger);
    if (archiveError != ArchiveError::NONE)
        Serial.printf("N16R8 logger unavailable: %s\n", archiveErrorName(archiveError));
    Serial.println("N16R8: durable run recording; hardware/logging status at /api/info");
    printHardwareStatus();
#elif defined(PYROVUE_SINGLE_RUN_CSV)
    csvRecorder.begin();
    networkManager.setCsvRecorder(csvRecorder);
    const auto csvStatus = csvRecorder.status();
    runManager.begin(millis(), csvStatus.runId);
    Serial.printf("Feather CSV ffat=%s free=%lu saved_run=%lu state=%s error=%s\n",
                  csvStatus.mounted ? "mounted" : "unavailable",
                  static_cast<unsigned long>(csvStatus.freeBytes),
                  static_cast<unsigned long>(csvStatus.runId), csvStatus.state,
                  csvStatus.error ? csvStatus.error : "none");
#else
    historyPersistence.begin(historyStore);
#endif
    presetManager.begin();
#if defined(PYROVUE_HAS_DISPLAY)
    tft.init(135, 240);
    tft.setRotation(1);
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);
    displayManager.begin();
    displayManager.showStartupMessage();
    delay(500);
    displayManager.clearFullScreen();
#endif
    if (!sensorService.begin()) Serial.println("ERROR: Thermocouple initialization failed");
    networkManager.onCommand = handleCommand;
    networkManager.begin();
#endif
}

void loop() {
#if defined(ARDUINO)
    const uint32_t nowMs = millis();
#if defined(PYROVUE_BOARD_N16R8)
    const bool wasActive = runManager.active();
    runLogger.enforceLimit(nowMs);
#endif
#if defined(PYROVUE_SINGLE_RUN_CSV)
    if (runManager.active()) {
        const uint32_t elapsedMs = runManager.elapsedMs(nowMs);
        if (elapsedMs >= csvrun::maxDurationMs) {
            csvRecorder.stop(elapsedMs);
            runManager.stopRun();
            historyStore.flush();
            networkManager.broadcastRunState();
        }
    }
    const bool sampled = sensorService.tick(nowMs);
    if (sampled && runManager.active() && !csvRecorder.status().recording) {
        runManager.stopRun();
        historyStore.flush();
        networkManager.broadcastRunState();
    }
#endif
#if !defined(PYROVUE_SINGLE_RUN_CSV)
    sensorService.tick(nowMs);
#endif
#if defined(PYROVUE_BOARD_N16R8)
    runLogger.poll(nowMs);
    if (wasActive != runManager.active()) networkManager.broadcastRunState();
    static bool serialConnected = false;
    const bool connected = static_cast<bool>(Serial);
    if (connected && !serialConnected) printHardwareStatus();
    serialConnected = connected;
    if (connected && Serial.available()) {
        const int command = Serial.read();
        if (command == 'i' || command == 'I') printHardwareStatus();
    }
#endif
    networkManager.loop();
#endif
}
