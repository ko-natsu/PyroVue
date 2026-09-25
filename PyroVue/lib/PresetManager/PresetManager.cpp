#include "PresetManager.h"

#if defined(ARDUINO_ARCH_ESP8266)
#include <EEPROM.h>
#else
#include <Preferences.h>
Preferences preferences;
#endif

#if defined(ARDUINO_ARCH_ESP8266)
void PresetManager::begin() {
    EEPROM.begin(64);
    char value[33] = {};
    for (size_t i = 0; i < sizeof(value) - 1; ++i) value[i] = static_cast<char>(EEPROM.read(i));
    value[sizeof(value) - 1] = '\0';
    currentPreset = (value[0] == '\0' || static_cast<uint8_t>(value[0]) == 0xff) ? "idle" : String(value);
}

void PresetManager::setPreset(const String& preset) {
    currentPreset = preset;
    const size_t length = preset.length() < 32 ? preset.length() : 32;
    for (size_t i = 0; i < 32; ++i) EEPROM.write(i, i < length ? preset[i] : 0);
    EEPROM.commit();
}
#else
const char* PRESET_KEY = "preset";

void PresetManager::begin() {
    preferences.begin("pyrovue", false);
    currentPreset = preferences.getString(PRESET_KEY, "idle");
}

void PresetManager::setPreset(const String& preset) {
    currentPreset = preset;
    preferences.putString(PRESET_KEY, preset);
}
#endif

String PresetManager::getPreset() const { return currentPreset; }
