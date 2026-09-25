#ifndef HISTORY_STORE_H
#define HISTORY_STORE_H

#include <stddef.h>
#include <stdint.h>
#include "protocol.h"

class HistoryStore {
public:
    HistoryStore(TempSample* rawStorage, size_t rawCapacity,
                 CoarseSample* coarseStorage, size_t coarseCapacity,
                 uint32_t bucketMs);

    void clear(uint32_t runId);
    void push(const TempSample& sample);
    void flush();
    void restoreCoarse(const CoarseSample& sample);
    bool latestReading(TempSample& out) const;
    size_t rawCount() const;
    bool rawAt(size_t oldestIndex, TempSample& out) const;
    size_t coarseCount() const;
    bool coarseAt(size_t oldestIndex, CoarseSample& out) const;
    bool currentBucket(CoarseSample& out) const;
    uint32_t runId() const;
    uint32_t nextSequence() const;
    uint32_t oldestRawSequence() const;
    uint32_t latestRawSequence() const;

private:
    TempSample* raw;
    size_t rawCap;
    size_t rawHead;
    size_t rawSize;
    CoarseSample* coarse;
    size_t coarseCap;
    size_t coarseHead;
    size_t coarseSize;
    uint32_t resolution;
    uint32_t currentRun;
    uint32_t currentBucketMs;
    double bucketSum;
    uint32_t bucketCount;
    float bucketMin;
    float bucketMax;
    uint8_t bucketFault;
    bool bucketOpen;

    TempSample latestPhysicalSample;
    bool hasLatestPhysicalSample; // Survives clear(): run resets must not erase current sensor readout.
    void finalizeBucket();
};

#endif
