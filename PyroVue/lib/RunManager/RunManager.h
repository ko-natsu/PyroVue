#ifndef RUN_MANAGER_H
#define RUN_MANAGER_H

#include <stdint.h>
#include <atomic>
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
//   - prepareRun durably allocates a new ID while remaining inactive. A failed
//     save leaves identity/state unchanged; abandoning preparation never reuses it.
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
//   START_STORE_FAILED    - identity persistence failed; no run activated.
//   START_ID_EXHAUSTED    - uint32 identity space exhausted; never reuse an ID.
//
// Results returned by stopRun():
//   STOP_OK               - active run stopped.
//   STOP_NOT_ACTIVE       - rejected, no run is active.
class RunManager : public IRunClock {
public:
    enum StartResult {
        START_STARTED = 0,
        START_ALREADY_ACTIVE = 1,
        START_STORE_FAILED = 2,
        START_ID_EXHAUSTED = 3
    };

    enum StopResult {
        STOP_OK = 0,
        STOP_NOT_ACTIVE = 1
    };

    // store may be null for fully in-memory operation; begin() then leaves
    // the initial run id at 0 and saves are skipped.
    explicit RunManager(IRunIdStore* store = nullptr);

    // Recover the maximum of stored identity and a validated saved-run floor.
    // Never resume a run. Call once per boot before preparing/starting.
    void begin(uint32_t nowMs, uint32_t minimumRunId = 0);

    // Start a run: increments the run id, persists it once, stamps startMs
    // from nowMs and resets the sequence. See repeated-start rules above.
    StartResult startRun(uint32_t nowMs);

    // Persist identity first, then let a storage owner prepare/sync its file.
    // Activation performs no I/O and uses the original preparation timestamp.
    StartResult prepareRun(uint32_t nowMs, uint32_t minimumNextRunId = 0);
    bool activatePreparedRun();
    void abandonPreparedRun();

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
    uint32_t preparedStartMs;
    std::atomic<bool> running;
    bool prepared;
};

#endif // RUN_MANAGER_H
