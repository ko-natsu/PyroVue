#ifndef NETWORK_MANAGER_H
#define NETWORK_MANAGER_H

#include <functional>
#include "Arduino.h"
#include "protocol.h"
#include "TelemetryBuffer.h"

class PresetManager;

class NetworkManager {
 public:
  NetworkManager(TelemetryBuffer& buffer, PresetManager& presetMgr);
  void begin();
  void loop();
  void onNewSample(const TempSample& s);
  size_t clientCount() const;
  std::function<void(const String&)> onCommand;
  TelemetryBuffer& getBuffer() { return *_buffer; }
  PresetManager& getPresetManager() { return *_presetManager; }

 private:
  TelemetryBuffer* _buffer;
  PresetManager* _presetManager;
};

#endif
