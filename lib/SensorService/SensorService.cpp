#include "SensorService.h"

#include <math.h>

SensorService::SensorService(ISensorReader& reader, IRunClock& clock,
                             ISampleSink& sink)
    : reader_(reader),
      clock_(clock),
      sink_(sink),
      intervalMs_(1000U / SAMPLE_HZ),
      lastTickMs_(0),
      armed_(false) {}

void SensorService::begin() {
  armed_ = true;
  lastTickMs_ = 0;
}

void SensorService::tick(uint32_t nowMs) {
  if (!armed_) {
    return;
  }

  // Re-prime if time moved backwards (millis reset / new clock origin).
  if (nowMs < lastTickMs_) {
    lastTickMs_ = nowMs;
    if (clock_.state().active) {
      emit(nowMs, clock_.nextSeq());
    }
    return;
  }
  // Rollover-safe: unsigned difference, no absolute comparison.
  uint32_t elapsed = nowMs - lastTickMs_;
  if (elapsed < intervalMs_) {
    return;
  }

  lastTickMs_ = nowMs;

  if (!clock_.state().active) {
    return;  // No read, no emission, no sequence consumed.
  }

  emit(nowMs, clock_.nextSeq());
}

void SensorService::emit(uint32_t nowMs, uint32_t seq) {
  RawReading reading = reader_.read();

  TempSample sample;
  sample.runId = clock_.state().runId;
  sample.seq = seq;
  sample.ms = clock_.elapsedMs(nowMs);
  sample.tempC = reading.tempC;
  sample.fault = reading.fault;

  if (!reading.valid) {
    sample.fault |= SENSOR_FAULT_NAN;
  }
  if (isnan(sample.tempC) || isinf(sample.tempC)) {
    sample.tempC = 0.0f;
    sample.fault |= SENSOR_FAULT_NAN;
  }

  sink_.onSample(sample);
}
