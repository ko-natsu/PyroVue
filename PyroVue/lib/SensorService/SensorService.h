#ifndef SENSOR_SERVICE_H
#define SENSOR_SERVICE_H

#include <stdint.h>
#include "protocol.h"

struct RawReading {
    float temperatureC;
    uint8_t fault;
};

class ISensorReader {
public:
    virtual ~ISensorReader() {}
    virtual bool begin() = 0;
    virtual bool read(RawReading& reading) = 0;
};

class IRunClock {
public:
    virtual ~IRunClock() {}
    virtual bool active() const = 0;
    virtual uint32_t runId() const = 0;
    virtual uint32_t elapsedMs(uint32_t nowMs) const = 0;
    virtual uint32_t nextSequence() = 0;
};

class ISampleSink {
public:
    virtual ~ISampleSink() {}
    virtual void onSample(const TempSample& sample) = 0;
};

class SensorService {
public:
    SensorService(ISensorReader& reader, IRunClock& run, ISampleSink& sink,
                  uint32_t sampleHz);
    bool begin();
    bool tick(uint32_t nowMs);
    uint32_t intervalMs() const;

private:
    ISensorReader& sensor;
    IRunClock& runManager;
    ISampleSink& output;
    uint32_t interval;
    uint32_t lastSampleMs;
    bool started;
};

#if defined(ARDUINO)
#include <SPI.h>
#include <Adafruit_MAX31855.h>

class Max31855Sensor : public ISensorReader {
public:
    Max31855Sensor();
    bool begin() override;
    bool read(RawReading& reading) override;
private:
#if defined(ARDUINO_ARCH_ESP32)
    SPIClass thermoSpi;
#endif
    Adafruit_MAX31855 thermocouple;
};
#endif

#endif
