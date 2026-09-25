#include "HistoryStore.h"
#if defined(ARDUINO)
#include <Arduino.h>
#endif

#if defined(ARDUINO_ARCH_ESP32)
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
static portMUX_TYPE historyMux = portMUX_INITIALIZER_UNLOCKED;
#endif

namespace {
class HistoryGuard {
public:
    HistoryGuard() {
#if defined(ARDUINO_ARCH_ESP32)
        portENTER_CRITICAL(&historyMux);
#elif defined(ARDUINO_ARCH_ESP8266)
        noInterrupts();
#endif
    }
    ~HistoryGuard() {
#if defined(ARDUINO_ARCH_ESP32)
        portEXIT_CRITICAL(&historyMux);
#elif defined(ARDUINO_ARCH_ESP8266)
        interrupts();
#endif
    }
};
}

HistoryStore::HistoryStore(TempSample* rawStorage, size_t rawCapacity,
                           CoarseSample* coarseStorage, size_t coarseCapacity,
                           uint32_t bucketMs)
    : raw(rawStorage), rawCap(rawCapacity), rawHead(0), rawSize(0),
      coarse(coarseStorage), coarseCap(coarseCapacity), coarseHead(0), coarseSize(0),
      resolution(bucketMs), currentRun(0), currentBucketMs(0), bucketSum(0),
      bucketCount(0), bucketMin(0), bucketMax(0), bucketFault(0), bucketOpen(false),
      latestPhysicalSample{}, hasLatestPhysicalSample(false) {}

void HistoryStore::clear(uint32_t run) {
    HistoryGuard guard;
    rawHead = rawSize = coarseHead = coarseSize = 0;
    currentRun = run;
    bucketOpen = false;
    bucketSum = 0;
    bucketCount = 0;
    bucketFault = 0;
}

void HistoryStore::finalizeBucket() {
    if (!bucketOpen || coarseCap == 0) return;
    CoarseSample& out = coarse[coarseHead];
    out.runId = currentRun;
    out.ms = currentBucketMs;
    out.averageC = bucketCount ? static_cast<float>(bucketSum / bucketCount) : NAN;
    out.minimumC = bucketCount ? bucketMin : NAN;
    out.maximumC = bucketCount ? bucketMax : NAN;
    out.fault = bucketFault;
    coarseHead = (coarseHead + 1) % coarseCap;
    if (coarseSize < coarseCap) ++coarseSize;
    bucketOpen = false;
    bucketSum = 0;
    bucketCount = 0;
    bucketFault = 0;
}

void HistoryStore::flush() {
    HistoryGuard guard;
    finalizeBucket();
}

void HistoryStore::push(const TempSample& sample) {
    HistoryGuard guard;
    latestPhysicalSample = sample;
    hasLatestPhysicalSample = true;
    if (sample.runId == 0) return;
    if (sample.runId != currentRun) {
        rawHead = rawSize = coarseHead = coarseSize = 0;
        currentRun = sample.runId;
        bucketOpen = false;
        bucketSum = 0;
        bucketCount = 0;
        bucketFault = 0;
    }
    if (rawCap != 0) {
        raw[rawHead] = sample;
        rawHead = (rawHead + 1) % rawCap;
        if (rawSize < rawCap) ++rawSize;
    }

    const uint32_t bucket = (resolution == 0) ? sample.ms : (sample.ms / resolution) * resolution;
    if (!bucketOpen) {
        currentBucketMs = bucket;
        bucketOpen = true;
    } else if (bucket < currentBucketMs) {
        return;
    } else if (bucket != currentBucketMs) {
        finalizeBucket();
        currentBucketMs = bucket;
        bucketOpen = true;
    }

    bucketFault |= sample.fault;
    if (!isfinite(sample.tempC)) {
        bucketFault |= SENSOR_FAULT_NAN;
        return;
    }
    if (bucketCount == 0) {
        bucketMin = bucketMax = sample.tempC;
    } else {
        if (sample.tempC < bucketMin) bucketMin = sample.tempC;
        if (sample.tempC > bucketMax) bucketMax = sample.tempC;
    }
    bucketSum += static_cast<double>(sample.tempC);
    ++bucketCount;
}
bool HistoryStore::latestReading(TempSample& out) const {
    HistoryGuard guard;
    if (!hasLatestPhysicalSample) return false;
    out = latestPhysicalSample;
    return true;
}


void HistoryStore::restoreCoarse(const CoarseSample& sample) {
    HistoryGuard guard;
    if (coarseCap == 0 || sample.runId == 0) return;
    currentRun = sample.runId;
    coarse[coarseHead] = sample;
    coarseHead = (coarseHead + 1) % coarseCap;
    if (coarseSize < coarseCap) ++coarseSize;
}

size_t HistoryStore::rawCount() const {
    HistoryGuard guard;
    return rawSize;
}

bool HistoryStore::rawAt(size_t oldestIndex, TempSample& out) const {
    HistoryGuard guard;
    if (oldestIndex >= rawSize || rawCap == 0) return false;
    const size_t first = (rawHead + rawCap - rawSize) % rawCap;
    out = raw[(first + oldestIndex) % rawCap];
    return true;
}

size_t HistoryStore::coarseCount() const {
    HistoryGuard guard;
    return coarseSize;
}

bool HistoryStore::coarseAt(size_t oldestIndex, CoarseSample& out) const {
    HistoryGuard guard;
    if (oldestIndex >= coarseSize || coarseCap == 0) return false;
    const size_t first = (coarseHead + coarseCap - coarseSize) % coarseCap;
    out = coarse[(first + oldestIndex) % coarseCap];
    return true;
}

bool HistoryStore::currentBucket(CoarseSample& out) const {
    HistoryGuard guard;
    if (!bucketOpen) return false;
    out.runId = currentRun;
    out.ms = currentBucketMs;
    out.averageC = bucketCount ? static_cast<float>(bucketSum / bucketCount) : NAN;
    out.minimumC = bucketCount ? bucketMin : NAN;
    out.maximumC = bucketCount ? bucketMax : NAN;
    out.fault = bucketFault;
    return true;
}

uint32_t HistoryStore::runId() const {
    HistoryGuard guard;
    return currentRun;
}

uint32_t HistoryStore::nextSequence() const {
    HistoryGuard guard;
    if (rawSize == 0 || rawCap == 0) return 0;
    const size_t first = (rawHead + rawCap - rawSize) % rawCap;
    return raw[(first + rawSize - 1) % rawCap].seq;
}

uint32_t HistoryStore::oldestRawSequence() const {
    HistoryGuard guard;
    if (rawSize == 0 || rawCap == 0) return 0;
    const size_t first = (rawHead + rawCap - rawSize) % rawCap;
    return raw[first].seq;
}

uint32_t HistoryStore::latestRawSequence() const { return nextSequence(); }
