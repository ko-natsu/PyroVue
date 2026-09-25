#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

struct TempSample {
  uint32_t runId;
  uint32_t seq;
  uint32_t ms;
  float tempC;
  uint8_t fault;
};

struct CoarseSample {
  uint32_t runId;
  uint32_t ms;
  float averageC;
  float minimumC;
  float maximumC;
  uint8_t fault;
};

enum SensorFault : uint8_t {
  SENSOR_FAULT_OPEN = 1 << 0,
  SENSOR_FAULT_SHORT_GND = 1 << 1,
  SENSOR_FAULT_SHORT_VCC = 1 << 2,
  SENSOR_FAULT_NAN = 1 << 3,
};

struct RunState {
  uint32_t runId;
  uint32_t startMs;
  bool active;
};

#endif // PROTOCOL_H
