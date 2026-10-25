#include "RunManager.h"

RunManager::RunManager(IRunIdStore* storeRef)
    : store(storeRef), id(0), startMs(0), seq(0), frozenMs(0),
      preparedStartMs(0), running(false), prepared(false) {
}

void RunManager::begin(uint32_t nowMs, uint32_t minimumRunId) {
    (void)nowMs; // identity recovery only; no active state is resumed
    id = 0;
    startMs = 0;
    seq = 0;
    frozenMs = 0;
    preparedStartMs = 0;
    prepared = false;
    running = false;
    if (store != nullptr) {
        uint32_t stored = 0;
        if (store->loadRunId(stored)) {
            id = stored;
        }
    }
    if (id < minimumRunId) id = minimumRunId;
}

RunManager::StartResult RunManager::prepareRun(uint32_t nowMs, uint32_t minimumNextRunId) {
    if (running || prepared) return START_ALREADY_ACTIVE;
    if (id == UINT32_MAX) return START_ID_EXHAUSTED;
    uint32_t nextId = id + 1;
    if (nextId < minimumNextRunId) nextId = minimumNextRunId;
    if (store != nullptr && !store->saveRunId(nextId)) return START_STORE_FAILED;
    id = nextId;
    preparedStartMs = nowMs;
    prepared = true;
    return START_STARTED;
}

bool RunManager::activatePreparedRun() {
    if (!prepared || running) return false;
    startMs = preparedStartMs;
    seq = 0;
    frozenMs = 0;
    prepared = false;
    running = true;
    return true;
}

void RunManager::abandonPreparedRun() {
    prepared = false;
}

RunManager::StartResult RunManager::startRun(uint32_t nowMs) {
    const auto result = prepareRun(nowMs);
    if (result == START_STARTED) activatePreparedRun();
    return result;
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
