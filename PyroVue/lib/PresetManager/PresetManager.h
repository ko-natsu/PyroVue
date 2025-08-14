#ifndef PRESET_MANAGER_H
#define PRESET_MANAGER_H

#include <Arduino.h>

class PresetManager {
public:
    void begin();
    void setPreset(const String& preset);
    String getPreset() const;

private:
    String currentPreset;
};

#endif // PRESET_MANAGER_H
