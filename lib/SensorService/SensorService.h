#ifndef SENSOR_SERVICE_H
#define SENSOR_SERVICE_H

#include <stdint.h>

#include "config.h"
#include "protocol.h"

// A single raw sensor reading, independent of any transport or platform.
// valid == false means the reader could not obtain a trustworthy
// conversion (missing device, bus error, chip fault with no temperature).
struct RawReading {
  float tempC;
  uint8_t fault;  // SensorFault bits
  bool valid;
};

// Abstraction over the thermocouple front-end. Implementations must be
// cheap and non-blocking: they are called from the sample cadence.
class ISensorReader {
 public:
  virtual ~ISensorReader() {}
  virtual RawReading read() = 0;
};

// Consumer of normalized TempSample emissions (history, telemetry, UI...).
class ISampleSink {
 public:
  virtual ~ISampleSink() {}
  virtual void onSample(const TempSample& sample) = 0;
};

// Injected run clock owned by RunManager. The service never touches
// RunManager directly; native tests substitute fakes here.
class IRunClock {
 public:
  virtual ~IRunClock() {}
  virtual RunState state() const = 0;
  // Rollover-safe elapsed time for the current run at nowMs.
  virtual uint32_t elapsedMs(uint32_t nowMs) const = 0;
  // Next monotonically increasing per-run sequence number.
  virtual uint32_t nextSeq() = 0;
};

// Fixed-cadence sampler. tick(nowMs) is deterministic for native callers:
// it takes no platform time, only the caller-supplied tick timestamp.
//
// Behavior:
//  - Samples at 1000 / SAMPLE_HZ ms, derived from config.h.
//  - First tick after begin() samples immediately, then re-arms.
//  - Rollover-safe scheduling uses unsigned subtraction only.
//  - If the run clock reports inactive, no read is issued and nothing
//    is emitted (sequence numbers are not consumed).
//  - Non-finite temperatures (NaN/Inf), or a failed read, are normalized
//    to tempC = 0.0f with SENSOR_FAULT_NAN set.
class SensorService {
 public:
  SensorService(ISensorReader& reader, IRunClock& clock, ISampleSink& sink);

  // Arms the cadence; the next tick() emits an initial sample.
  void begin();

  // Call periodically (or once per loop) with the current timestamp.
  void tick(uint32_t nowMs);

  uint32_t intervalMs() const { return intervalMs_; }
  bool armed() const { return armed_; }

 private:
  void emit(uint32_t nowMs, uint32_t seq);

  ISensorReader& reader_;
  IRunClock& clock_;
  ISampleSink& sink_;
  uint32_t intervalMs_;
  uint32_t lastTickMs_;
  bool armed_;
};

#endif  // SENSOR_SERVICE_H
