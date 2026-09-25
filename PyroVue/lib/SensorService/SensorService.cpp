#include "SensorService.h"
#include "config.h"
#include <math.h>

SensorService::SensorService(ISensorReader& reader, IRunClock& run,
                             ISampleSink& sink, uint32_t sampleHz)
    : sensor(reader), runManager(run), output(sink),
      interval(sampleHz == 0 ? 1000 : 1000 / sampleHz),
      lastSampleMs(0), started(false) {}

bool SensorService::begin() {
    started = sensor.begin();
    return started;
}

bool SensorService::tick(uint32_t nowMs) {
    if (!started || static_cast<uint32_t>(nowMs - lastSampleMs) < interval) return false;
    lastSampleMs = nowMs;
    RawReading reading{};
    if (!sensor.read(reading)) return false;
    uint8_t fault = reading.fault;
    if (!isfinite(reading.temperatureC)) fault |= SENSOR_FAULT_NAN;
    TempSample sample{
        runManager.active() ? runManager.runId() : 0,
        runManager.active() ? runManager.nextSequence() : 0,
        runManager.active() ? runManager.elapsedMs(nowMs) : 0,
        reading.temperatureC,
        fault
    };
    output.onSample(sample);
    return true;
}

uint32_t SensorService::intervalMs() const { return interval; }

#if defined(ARDUINO)
#include "config.h"

#if defined(ARDUINO_ARCH_ESP32)
Max31855Sensor::Max31855Sensor()
    : thermoSpi(FSPI), thermocouple(THERMOCOUPLE_CS, &thermoSpi) {}
#else
Max31855Sensor::Max31855Sensor()
    : thermocouple(THERMOCOUPLE_SCK, THERMOCOUPLE_CS, THERMOCOUPLE_MISO) {}
#endif

bool Max31855Sensor::begin() {
#if defined(ARDUINO_ARCH_ESP32)
    thermoSpi.begin(THERMOCOUPLE_SCK, THERMOCOUPLE_MISO, THERMOCOUPLE_MOSI);
#endif
    return thermocouple.begin();
}

bool Max31855Sensor::read(RawReading& reading) {
    const double value = thermocouple.readCelsius();
    reading.temperatureC = static_cast<float>(value);
    uint8_t fault = thermocouple.readError();
    if (fault & MAX31855_FAULT_OPEN) fault = static_cast<uint8_t>(fault | SENSOR_FAULT_OPEN);
    if (fault & MAX31855_FAULT_SHORT_GND) fault = static_cast<uint8_t>(fault | SENSOR_FAULT_SHORT_GND);
    if (fault & MAX31855_FAULT_SHORT_VCC) fault = static_cast<uint8_t>(fault | SENSOR_FAULT_SHORT_VCC);
    if (!isfinite(reading.temperatureC)) fault = static_cast<uint8_t>(fault | SENSOR_FAULT_NAN);
    reading.fault = fault;
    return true;
}
#endif
