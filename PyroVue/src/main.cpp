#include <Arduino.h>
#include <ArduinoJson.h>
#include "config.h"
#include "protocol.h"
#include "SensorService.h"
#include "RunManager.h"
#include "HistoryStore.h"
#include "NetworkManager.h"
#if !defined(PYROVUE_BOARD_N16R8) && !defined(PYROVUE_SINGLE_RUN_CSV)
#include "LittleFsHistoryPersistence.h"
#endif
#if defined(PYROVUE_SINGLE_RUN_CSV)
#include "SingleRunCsv.h"
#include "FeatherCsvStorage.h"
#endif
#if defined(PYROVUE_BOARD_N16R8)
#include <LittleFS.h>
#include <esp_heap_caps.h>
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
#if defined(PYROVUE_BOARD_N16R8)
bool archiveHardwareReady = false;
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

static NetworkCommandResult handleCommand(const char* command) {
    JsonDocument doc;
    if (deserializeJson(doc, command) != DeserializationError::Ok ||
        !doc["type"].is<const char*>()) return {400, "INVALID_REQUEST"};
    const char* type = doc["type"];
    if (strcmp(type, "preset") == 0 && doc["value"].is<const char*>()) {
        presetManager.setPreset(doc["value"].as<const char*>());
        return {200, nullptr};
    }
    if (strcmp(type, "run.start") == 0) {
        if (runManager.active()) return {200, nullptr};
#if defined(PYROVUE_BOARD_N16R8)
        if (!archiveHardwareReady) return {503, "STORAGE_UNAVAILABLE"};
#elif defined(PYROVUE_SINGLE_RUN_CSV)
        if (!csvRecorder.canStart()) {
            const auto status = csvRecorder.status();
            if (status.hasRun) return {409, "SAVED_RUN_EXISTS"};
            if (!status.mounted) return {503, "STORAGE_UNAVAILABLE"};
            if (status.error) return {503, status.error};
            return {507, "INSUFFICIENT_STORAGE"};
        }
#endif
#if defined(PYROVUE_SINGLE_RUN_CSV)
        const auto result = runManager.prepareRun(millis());
#else
        const auto result = runManager.startRun(millis());
#endif
        if (result == RunManager::START_STORE_FAILED) return {500, "RUN_ID_SAVE_FAILED"};
        if (result == RunManager::START_ID_EXHAUSTED) return {409, "RUN_ID_EXHAUSTED"};
        if (result != RunManager::START_STARTED) return {409, "RUN_ACTIVE"};
#if defined(PYROVUE_SINGLE_RUN_CSV)
        if (!csvRecorder.start(runManager.runId())) {
            runManager.abandonPreparedRun();
            return {503, "WRITE_FAILED"};
        }
        if (!runManager.activatePreparedRun()) {
            csvRecorder.stop(0);
            runManager.abandonPreparedRun();
            return {500, "RUN_ACTIVATION_FAILED"};
        }
#endif
        historyStore.clear(runManager.runId());
#if !defined(PYROVUE_BOARD_N16R8) && !defined(PYROVUE_SINGLE_RUN_CSV)
        persistedBucketMs = 0xffffffffUL;
        historyPersistence.startRun(runManager.runId(), COARSE_BUCKET_MS);
#endif
        return {200, nullptr};
    }
    if (strcmp(type, "run.stop") == 0) {
        if (!runManager.active()) return {200, nullptr};
        bool durable = true;
#if defined(PYROVUE_SINGLE_RUN_CSV)
        durable = csvRecorder.stop(runManager.elapsedMs(millis()));
#endif
        runManager.stopRun();
        historyStore.flush();
#if !defined(PYROVUE_BOARD_N16R8) && !defined(PYROVUE_SINGLE_RUN_CSV)
        persistLatestCoarse();
        historyPersistence.flush();
#endif
        return durable ? NetworkCommandResult{200, nullptr}
                       : NetworkCommandResult{503, "WRITE_FAILED"};
    }
    return {400, "INVALID_REQUEST"};
}
#endif

void setup() {
    Serial.begin(115200);
#if defined(ARDUINO)
    runIdStore.begin();
#if !defined(PYROVUE_SINGLE_RUN_CSV)
    runManager.begin(millis());
#endif
#if defined(PYROVUE_BOARD_N16R8)
    const uint32_t flashBytes = ESP.getFlashChipSize();
    const uint32_t psramBytes = ESP.getPsramSize();
    const bool fsMounted = LittleFS.begin(false, "/littlefs", 10, "spiffs");
    archiveHardwareReady = flashBytes == 16777216UL && psramFound() &&
                           psramBytes == 8388608UL && fsMounted;
    Serial.printf("N16R8 flash=%lu psram=%lu fs=%s\n",
                  static_cast<unsigned long>(flashBytes),
                  static_cast<unsigned long>(psramBytes),
                  fsMounted ? "mounted" : "unavailable");
    Serial.printf("N16R8 fs_label=spiffs total=%lu used=%lu psram_largest=%lu\n",
                  fsMounted ? static_cast<unsigned long>(LittleFS.totalBytes()) : 0UL,
                  fsMounted ? static_cast<unsigned long>(LittleFS.usedBytes()) : 0UL,
                  static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
    if (!archiveHardwareReady) Serial.println("ERROR: Recorded Start unavailable; monitoring remains enabled");
    Serial.println("N16R8 M1: monitor sessions only; durable run recording requires M2");
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
    networkManager.loop();
#endif
}
