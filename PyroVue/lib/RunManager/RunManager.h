#ifndef RUN_MANAGER_H
#define RUN_MANAGER_H

#include <stdint.h>
#include "SensorService.h"

// Portable run-identity storage seam. Device firmware supplies an
// implementation backed by the platform filesystem / EEPROM / NVS; native
// tests supply an in-memory fake. Implementations must be portable-safe:
// loadRunId only reads, saveRunId only writes.
//
// Contract:
//   loadRunId(outId)  -> true and sets outId if a stored run id exists,
//                        false if none stored (or unreadable).
//   saveRunId(id)     -> true if the id was persisted, false on failure.
//                        Failure must not corrupt previously stored data.
class IRunIdStore {
public:
    virtual ~IRunIdStore() {}
    virtual bool loadRunId(uint32_t& outId) = 0;
    virtual bool saveRunId(uint32_t id) = 0;
};

// Owns run lifecycle and per-run counters. Explicitly started and stopped by
// the caller; begin() only recovers identity, never an active run.
//
// Sequencing rules:
//   - runId increases by exactly 1 per successful startRun() and is persisted
//     to the IRunIdStore once per start (best effort; a failed save does not
//     abort the run, the in-memory id is still authoritative until next boot).
//   - The per-run sequence is monotonic within a run and resets to 0 at every
//     startRun(); nextSequence() returns 1, 2, 3, ... during a run.
//
// Rollover safety: elapsedMs(nowMs) uses unsigned 32-bit subtraction,
// (uint32_t)(nowMs - startMs), so it stays correct across a single wrap of
// the 32-bit millisecond clock (~49.7 days).
//
// Statelessness across instances: RunManager holds no static state; two
// instances share nothing. Identity collision is only possible if two
// instances share one IRunIdStore.
//
// Repeated start/stop behavior:
//   - startRun() while a run is active is rejected: returns
//     START_ALREADY_ACTIVE, run id is NOT incremented, no store write,
//     sequence is NOT reset.
//   - stopRun() while no run is active is rejected: returns STOP_NOT_ACTIVE,
//     no state change. stopRun() does not clear the last run id; the next
//     startRun() still increments from it.
//
// Results returned by startRun():
//   START_STARTED         - new run began (id incremented and persisted).
//   START_ALREADY_ACTIVE  - rejected, a run is already active.
//
// Results returned by stopRun():
//   STOP_OK               - active run stopped.
//   STOP_NOT_ACTIVE       - rejected, no run is active.
class RunManager : public IRunClock {
public:
    enum StartResult {
        START_STARTED = 0,
        START_ALREADY_ACTIVE = 1
    };

    enum StopResult {
        STOP_OK = 0,
        STOP_NOT_ACTIVE = 1
    };

    // store may be null for fully in-memory operation; begin() then leaves
    // the initial run id at 0 and saves are skipped.
    explicit RunManager(IRunIdStore* store = nullptr);

    // Recover identity at boot. Loads the stored run id when available.
    // Never resumes an active run: active() is false afterwards regardless
    // of any persisted state, and startMs/sequence start fresh on the next
    // startRun(). Safe to call at most once per boot, before any start.
    void begin(uint32_t nowMs);

    // Start a run: increments the run id, persists it once, stamps startMs
    // from nowMs and resets the sequence. See repeated-start rules above.
    StartResult startRun(uint32_t nowMs);

    // Stop the active run. Elapsed time freezes at the last elapsedMs()
    // reading; run id is retained for the next start.
    StopResult stopRun();

    // True between a successful startRun() and stopRun().
    bool active() const override;
    uint32_t originMs() const;

    // Current run id (0 before the first successful start).
    uint32_t runId() const override;

    // Milliseconds since the active run started, rollover-safe. While
    // active, caches the latest reading so that after stopRun() it returns
    // the elapsed time observed at the last call (0 if elapsedMs() was
    // never called for that run).
    uint32_t elapsedMs(uint32_t nowMs) const override;

    // Returns the next sequence number for the current run (1-based),
    // monotonically increasing until the next startRun() resets it.
    uint32_t nextSequence() override;

private:
    IRunIdStore* store;   // not owned
    uint32_t id;          // last/current run id
    uint32_t startMs;     // nowMs captured at startRun()
    uint32_t seq;         // per-run sequence counter
    mutable uint32_t frozenMs; // last elapsedMs() reading; frozen at stopRun()
    bool running;
};

#endif // RUN_MANAGER_H
