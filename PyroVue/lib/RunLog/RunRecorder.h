#ifndef RUN_RECORDER_H
#define RUN_RECORDER_H
#include "RunLogTypes.h"
#include "protocol.h"
// Consumes every in-run acquisition; selects real readings without interpolation.
class RunRecorder {
public:
    void reset(uint32_t runId);
    bool select(const TempSample&, LoggedSample&);
    bool finish(LoggedSample&);

private:
    void emit(const TempSample& src, uint8_t flags, LoggedSample& out);

    uint32_t runId_{};
    bool active_{};        // selection open; reset(0) and finish() close it
    bool haveAcq_{};       // a consumed acquisition exists (gap baseline)
    bool haveLogged_{};    // a record was emitted (cadence anchor)
    bool haveLatest_{};    // latest consumed acquisition is not yet selected
    bool gapPending_{};    // acquired dt beyond threshold since the last record
    uint32_t lastAcqSeq_{};
    uint32_t lastAcqMs_{};
    uint32_t lastLoggedMs_{};
    uint32_t nextLogSeq_{1};
    uint8_t intervalFault_{};  // faults ORed since previous record, current included
    TempSample latest_{};      // retained in-range acquisition for finish()
};
#endif
