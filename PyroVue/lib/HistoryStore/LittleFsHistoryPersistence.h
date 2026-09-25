#ifndef LITTLEFS_HISTORY_PERSISTENCE_H
#define LITTLEFS_HISTORY_PERSISTENCE_H

#include <stdint.h>
#include "protocol.h"

class HistoryStore;

#if defined(ARDUINO)
class LittleFsHistoryPersistence {
public:
    LittleFsHistoryPersistence();
    bool begin(HistoryStore& history);
    bool startRun(uint32_t runId, uint32_t bucketMs);
    bool append(const CoarseSample& sample);
    bool flush();

private:
    bool mounted;
    uint32_t activeRun;
};
#endif

#endif
