#include "RunManager.h"

RunManager::RunManager(IRunIdStore* storeRef)
    : store(storeRef), id(0), startMs(0), seq(0), frozenMs(0), running(false) {
}

void RunManager::begin(uint32_t nowMs) {
    (void)nowMs; // identity recovery only; no active state is resumed
    id = 0;
    startMs = 0;
    seq = 0;
    running = false;
    if (store != nullptr) {
        uint32_t stored = 0;
        if (store->loadRunId(stored)) {
            id = stored;
        }
    }
}

RunManager::StartResult RunManager::startRun(uint32_t nowMs) {
    if (running) {
        return START_ALREADY_ACTIVE;
    }
    ++id;
    if (id == 0) ++id;
    if (store != nullptr) {
        (void)store->saveRunId(id);
    }
    startMs = nowMs;
    seq = 0;
    frozenMs = 0;
    running = true;
    return START_STARTED;
}

RunManager::StopResult RunManager::stopRun() {
    if (!running) {
        return STOP_NOT_ACTIVE;
    }
    running = false;
    // Freeze elapsed time at the clock value last observed via elapsedMs().
    return STOP_OK;
}

bool RunManager::active() const {
    return running;
}

uint32_t RunManager::runId() const {
    return id;
}

uint32_t RunManager::originMs() const {
    return startMs;
}

uint32_t RunManager::elapsedMs(uint32_t nowMs) const {
    if (!running) {
        return frozenMs;
    }
    frozenMs = (uint32_t)(nowMs - startMs);
    return frozenMs;
}

uint32_t RunManager::nextSequence() {
    return ++seq;
}
