#ifndef RUN_RECORDER_H
#define RUN_RECORDER_H
#include "RunLogTypes.h"
#include "protocol.h"
// M2 implementation contract: consumes every acquisition; no catch-up interpolation.
class RunRecorder {
public:
    void reset(uint32_t runId);
    bool select(const TempSample&, LoggedSample&);
    bool finish(LoggedSample&);
};
#endif
