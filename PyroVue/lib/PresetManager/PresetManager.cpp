#include "PresetManager.h"
#include <Preferences.h>

Preferences preferences;

const char* PRESET_KEY = "preset";

void PresetManager::begin() {
    preferences.begin("pyrovue", false);
    currentPreset = preferences.getString(PRESET_KEY, "idle");
}

void PresetManager::setPreset(const String& preset) {
    currentPreset = preset;
    preferences.putString(PRESET_KEY, preset);
}

String PresetManager::getPreset() const {
    return currentPreset;
}
