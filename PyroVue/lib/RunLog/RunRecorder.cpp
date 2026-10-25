#include "RunRecorder.h"

#include <string.h>

namespace {
// A timing hole is any acquired dt beyond 1.5 acquire intervals. It flags the
// next record (selected or terminal) with runlog::kAcquisitionGap and never
// invents a sensor fault or interpolated records.
constexpr uint32_t kGapThresholdMs =
    runlog::kAcquireIntervalMs + runlog::kAcquireIntervalMs / 2;
}  // namespace

void RunRecorder::reset(uint32_t runId) {
    runId_ = runId;
    active_ = runId != 0;
    haveAcq_ = false;
    haveLogged_ = false;
    haveLatest_ = false;
    gapPending_ = false;
    lastAcqSeq_ = 0;
    lastAcqMs_ = 0;
    lastLoggedMs_ = 0;
    nextLogSeq_ = 1;
    intervalFault_ = 0;
    latest_ = TempSample{};
}

bool RunRecorder::select(const TempSample& sample, LoggedSample& out) {
    if (!active_) return false;
    // Idle samples (runId 0) and foreign-run traffic never reach the stream.
    if (sample.runId != runId_ || sample.seq == 0) return false;
    // No record may leave the run window; the latest in-range acquisition is
    // retained untouched for finish().
    if (sample.ms > runlog::kMaxDurationMs) return false;
    // Duplicates and non-increasing identities/timestamps are dropped without
    // advancing the consumed stream (selection state stays intact).
    if (haveAcq_ && (sample.seq <= lastAcqSeq_ || sample.ms <= lastAcqMs_)) return false;

    if (haveAcq_ && (sample.ms - lastAcqMs_) > kGapThresholdMs) gapPending_ = true;
    lastAcqSeq_ = sample.seq;
    lastAcqMs_ = sample.ms;
    haveAcq_ = true;
    intervalFault_ |= sample.fault;

    if (haveLogged_ && (sample.ms - lastLoggedMs_) < runlog::kLogIntervalMs) {
        memcpy(&latest_, &sample, sizeof(latest_));  // hold exact bits for finish()
        haveLatest_ = true;
        return false;
    }
    emit(sample, 0, out);
    return true;
}

bool RunRecorder::finish(LoggedSample& out) {
    if (!active_) return false;
    active_ = false;             // close the selector even when nothing is emitted
    if (!haveLatest_) return false;  // latest already selected: never duplicate it
    emit(latest_, runlog::kTerminal, out);
    return true;
}

void RunRecorder::emit(const TempSample& src, uint8_t flags, LoggedSample& out) {
    out.logSeq = nextLogSeq_++;
    out.acqSeq = src.seq;
    out.ms = src.ms;
    memcpy(&out.tempC, &src.tempC, sizeof(out.tempC));  // keep NaN/-0 payload bits
    out.fault = src.fault;
    out.intervalFault = intervalFault_;
    out.flags = gapPending_ ? (flags | runlog::kAcquisitionGap) : flags;
    gapPending_ = false;
    intervalFault_ = 0;
    lastLoggedMs_ = src.ms;
    haveLogged_ = true;
    haveLatest_ = false;
}
