#include <Arduino.h>
#include <ArduinoJson.h>
#include "config.h"
#include "protocol.h"
#include "SensorService.h"
#include "RunManager.h"
#include "HistoryStore.h"
#include "NetworkManager.h"
#include "LittleFsHistoryPersistence.h"
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
LittleFsHistoryPersistence historyPersistence;
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
uint32_t persistedBucketMs = 0xffffffffUL;
static void persistLatestCoarse() {
    const size_t count = historyStore.coarseCount();
    if (count == 0) return;
    CoarseSample sample{};
    if (!historyStore.coarseAt(count - 1, sample) || sample.ms == persistedBucketMs) return;
    if (historyPersistence.append(sample)) persistedBucketMs = sample.ms;
}

class SampleRouter : public ISampleSink {
public:
    void onSample(const TempSample& sample) override {
        historyStore.push(sample);
        persistLatestCoarse();
        networkManager.onNewSample(sample);
#if defined(PYROVUE_HAS_DISPLAY)
        statusManager.update(sample.tempC, sample.fault);
#endif
    }
};

SampleRouter sampleRouter;
SensorService sensorService(thermocouple, runManager, sampleRouter, SAMPLE_HZ);

static void handleCommand(const String& command) {
    JsonDocument doc;
    if (deserializeJson(doc, command) != DeserializationError::Ok || !doc["type"].is<const char*>()) return;
    const char* type = doc["type"];
    if (strcmp(type, "preset") == 0 && doc["value"].is<const char*>()) {
        presetManager.setPreset(doc["value"].as<const char*>());
    } else if (strcmp(type, "run.start") == 0) {
        if (runManager.startRun(millis()) == RunManager::START_STARTED) {
            historyStore.clear(runManager.runId());
            persistedBucketMs = 0xffffffffUL;
            historyPersistence.startRun(runManager.runId(), COARSE_BUCKET_MS);
        }
    } else if (strcmp(type, "run.stop") == 0) {
        if (runManager.stopRun() == RunManager::STOP_OK) {
            historyStore.flush();
            persistLatestCoarse();
            historyPersistence.flush();
        }
    }
}
#endif

void setup() {
    Serial.begin(115200);
#if defined(ARDUINO)
    runIdStore.begin();
    runManager.begin(millis());
    historyPersistence.begin(historyStore);
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
    sensorService.tick(millis());
    networkManager.loop();
#endif
}
