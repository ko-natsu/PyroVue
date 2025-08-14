#ifndef TELEMETRY_BUFFER_H
#define TELEMETRY_BUFFER_H

#include <vector>
#include "protocol.h"

class TelemetryBuffer {
 public:
  void push(const TempSample& s);
  size_t size() const;
  void snapshot(size_t count,
                std::vector<uint32_t>& ms,
                std::vector<float>& t,
                std::vector<uint8_t>& fault) const;
  TempSample getLastSample() const;
};

#endif
