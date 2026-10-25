// Portable implementation of the single-run CSV recorder. See SingleRunCsv.h
// for the on-disk format and the full behavior contract. C++11-compatible
// (device builds use gnu++11), no Arduino dependencies, no heap allocation.

#include "SingleRunCsv.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace csvrun {
namespace {

constexpr char kFormatIdLine[] = "# pyrovue-single-run-csv,1";
constexpr char kColumnHeader[] =
    "run_id,log_seq,acq_seq,elapsed_ms,temp_c,fault,interval_fault,flags";

// Text helpers over (begin, end) spans; parsing is strict so corruption is
// detected instead of laundered into plausible-looking values.

bool spanEq(const char* begin, const char* end, const char* literal) {
    const size_t n = strlen(literal);
    return static_cast<size_t>(end - begin) == n && memcmp(begin, literal, n) == 0;
}

bool spanStartsWith(const char* begin, const char* end, const char* literal, const char*& rest) {
    const size_t n = strlen(literal);
    if (static_cast<size_t>(end - begin) < n || memcmp(begin, literal, n) != 0) {
        return false;
    }
    rest = begin + n;
    return true;
}

bool spanU32(const char* begin, const char* end, uint32_t& out) {
    if (begin == end || static_cast<size_t>(end - begin) > 10) {
        return false;
    }
    uint32_t value = 0;
    for (const char* p = begin; p != end; ++p) {
        if (*p < '0' || *p > '9') {
            return false;
        }
        const uint32_t digit = static_cast<uint32_t>(*p - '0');
        if (value > (0xFFFFFFFFu - digit) / 10u) {
            return false;
        }
        value = value * 10u + digit;
    }
    out = value;
    return true;
}

bool spanF32(const char* begin, const char* end, float& out) {
    if (begin == end || static_cast<size_t>(end - begin) >= 32) {
        return false;
    }
    // Fields are comma-delimited inside the scanner's NUL-terminated line.
    char* parseEnd = nullptr;
    const float value = strtof(begin, &parseEnd);
    if (parseEnd != end || !isfinite(value)) return false;
    out = value;
    return true;
}

// Footer: "# end,reason=<complete|duration_limit>,rows=<n>,elapsed_ms=<n>".
bool parseFooterLine(const char* begin, const char* end, const char*& reason,
                     uint32_t& rows, uint32_t& elapsed) {
    const char* p = nullptr;
    if (!spanStartsWith(begin, end, "# end,reason=", p)) {
        return false;
    }
    const char* reasonStart = p;
    while (p < end && *p != ',') {
        ++p;
    }
    if (p == end) {
        return false;
    }
    if (spanEq(reasonStart, p, "complete")) {
        reason = kStateComplete;
    } else if (spanEq(reasonStart, p, "duration_limit")) {
        reason = kStateDurationLimit;
    } else {
        return false;
    }
    if (!spanStartsWith(p, end, ",rows=", p)) {
        return false;
    }
    const char* rowsStart = p;
    while (p < end && *p != ',') {
        ++p;
    }
    if (p == end) {
        return false;
    }
    if (!spanU32(rowsStart, p, rows)) {
        return false;
    }
    if (!spanStartsWith(p, end, ",elapsed_ms=", p)) {
        return false;
    }
    return spanU32(p, end, elapsed);
}

struct RowValues {
    uint32_t runId;
    uint32_t logSeq;
    uint32_t acqSeq;
    uint32_t elapsedMs;
    float tempC;
    uint32_t fault;
    uint32_t intervalFault;
    uint32_t flags;
};

bool splitRow(const char* begin, const char* end, const char* fieldBegin[8], const char* fieldEnd[8]) {
    int count = 0;
    const char* start = begin;
    for (const char* p = begin;; ++p) {
        if (p == end || *p == ',') {
            if (count >= 8) {
                return false;
            }
            fieldBegin[count] = start;
            fieldEnd[count] = p;
            ++count;
            if (p == end) {
                break;
            }
            start = p + 1;
        }
    }
    return count == 8;
}

// Strict row validation; ordering rules are cross-record and need the state
// of the previously validated row (rowCount/lastAcq/lastMs).
bool parseRowLine(const char* begin, const char* end, uint32_t metaRunId, uint32_t rowCount,
                  uint32_t lastAcq, uint32_t lastMs, RowValues& out) {
    if (rowCount >= recordLimit) return false;
    const char* fb[8];
    const char* fe[8];
    if (!splitRow(begin, end, fb, fe)) {
        return false;
    }
    RowValues row;
    if (!spanU32(fb[0], fe[0], row.runId) || row.runId != metaRunId) {
        return false;
    }
    if (!spanU32(fb[1], fe[1], row.logSeq) || row.logSeq != rowCount + 1) {
        return false;
    }
    if (!spanU32(fb[2], fe[2], row.acqSeq) || row.acqSeq == 0) {
        return false;
    }
    if (rowCount > 0 && row.acqSeq <= lastAcq) {
        return false;
    }
    if (!spanU32(fb[3], fe[3], row.elapsedMs) || row.elapsedMs > maxDurationMs) {
        return false;
    }
    if (rowCount > 0 && row.elapsedMs <= lastMs) {
        return false;
    }
    if (fb[4] == fe[4]) {
        row.tempC = (float)NAN;  // blank field: non-finite value
    } else if (!spanF32(fb[4], fe[4], row.tempC)) {
        return false;
    }
    if (!spanU32(fb[5], fe[5], row.fault) || row.fault > 255) {
        return false;
    }
    if (!spanU32(fb[6], fe[6], row.intervalFault) || row.intervalFault > 255) {
        return false;
    }
    if (!spanU32(fb[7], fe[7], row.flags) || row.flags > kFlagMask) {
        return false;
    }
    out = row;
    return true;
}

}  // namespace

SingleRunCsv::SingleRunCsv(IStorage& storage) : storage_(storage) {}

void SingleRunCsv::refreshFreeBytes() {
    freeBytesCache_ = storage_.mounted() ? storage_.freeBytes() : 0;
}

void SingleRunCsv::failStop(const char* error) {
    error_ = error;
    state_ = kStateError;
    recording_ = false;
    storage_.closeWriter();
}

bool SingleRunCsv::begin() {
    if (recording_ || readerActive_) {
        return false;
    }
    if (!storage_.mount()) {
        // Mount NEVER auto-formats; the volume contents stay unknown.
        error_ = kErrMountFailed;
        state_ = kStateError;
        return false;
    }
    hasRun_ = false;
    runId_ = 0;
    sampleCount_ = 0;
    validatedBytes_ = 0;
    lastAcqSeq_ = 0;
    lastMs_ = 0;
    haveLastSample_ = false;
    pendingIntervalFault_ = 0;
    gapPending_ = false;
    error_ = nullptr;
    state_ = kStateIdle;
    if (storage_.fileExists()) {
        hasRun_ = true;
        if (!scanSavedRun()) {
            error_ = kErrReadFailed;
            state_ = kStateError;
            runId_ = 0;
            sampleCount_ = 0;
            validatedBytes_ = 0;
            refreshFreeBytes();
            return false;
        }
    }
    refreshFreeBytes();
    return true;
}

bool SingleRunCsv::canStart() {
    refreshFreeBytes();
    return storage_.mounted() && !recording_ && !readerActive_ && !hasRun_ &&
           error_ == nullptr && freeBytesCache_ >= startRequiredBytes;
}

bool SingleRunCsv::start(uint32_t runId) {
    if (runId == 0 || !canStart()) {
        return false;
    }
    if (!storage_.createExclusive()) {
        // Never overwrite: if a file exists after all, keep it and surface it.
        if (storage_.fileExists()) {
            hasRun_ = true;
            runId_ = 0;
            sampleCount_ = 0;
            validatedBytes_ = 0;
        }
        error_ = kErrCreateFailed;
        state_ = kStateError;
        refreshFreeBytes();
        return false;
    }
    hasRun_ = true;
    runId_ = runId;
    sampleCount_ = 0;
    validatedBytes_ = 0;
    lastAcqSeq_ = 0;
    lastMs_ = 0;
    haveLastSample_ = false;
    pendingIntervalFault_ = 0;
    gapPending_ = false;
    error_ = nullptr;

    char block[kMetaBlockBytes];
    const int n = snprintf(block, sizeof(block),
                           "# pyrovue-single-run-csv,1\n"
                           "# run_id,%lu\n"
                           "# acquire_interval_ms,500\n"
                           "# log_interval_ms,3000\n"
                           "# max_duration_ms,21600000\n"
                           "# record_limit,7202\n"
                           "%s\n",
                           static_cast<unsigned long>(runId), kColumnHeader);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(block)) {
        failStop(kErrWriteFailed);
        return false;
    }
    if (!storage_.writeAll(reinterpret_cast<const uint8_t*>(block), static_cast<size_t>(n))) {
        failStop(kErrWriteFailed);
        return false;
    }
    // The block is fully in the file and structurally valid: account for it
    // even if the sync below is the one that fails, so Status always reports
    // exactly the validated prefix of the current file content.
    validatedBytes_ = static_cast<uint32_t>(n);
    refreshFreeBytes();
    if (!storage_.sync()) {
        failStop(kErrSyncFailed);
        return false;
    }
    recording_ = true;
    state_ = kStateRecording;
    return true;
}

void SingleRunCsv::onSample(const TempSample& sample) {
    if (!recording_ || sample.runId != runId_) {
        return;
    }
    if (sample.ms > maxDurationMs) {
        finishRun(kStateDurationLimit, maxDurationMs, true);
        return;
    }
    if (sample.seq == 0 || (haveLastSample_ &&
        (sample.seq <= lastSample_.seq || sample.ms <= lastSample_.ms))) {
        failStop(kErrInvalidSample);
        return;
    }

    // Faults accumulate over every acquired sample, independently of gaps.
    pendingIntervalFault_ |= sample.fault;

    // Acquisition gap: evaluated between EVERY pair of consecutive acquired
    // samples so a hole can never hide inside a normal logged interval, while
    // ordinary cadence jitter (e.g. 550ms spacing) never raises a flag. The
    // first acquired sample has no prior baseline and is never flagged.
    if (haveLastSample_) {
        const uint32_t dt = sample.ms - lastSample_.ms;
        if (dt > acquireIntervalMs + acquireIntervalMs / 2 ||
            sample.seq != lastSample_.seq + 1) {
            gapPending_ = true;
        }
    }


    lastSample_ = sample;
    haveLastSample_ = true;

    if (sample.ms == maxDurationMs) {
        finishRun(kStateDurationLimit, maxDurationMs, true);
        return;
    }


    const bool due = (sampleCount_ == 0) || (sample.ms - lastMs_ >= logIntervalMs);
    if (!due) {
        return;
    }
    if (sampleCount_ >= recordLimit) {
        failStop(kErrStorageFull);
        return;
    }
    logRow(sample, false);
}

bool SingleRunCsv::logRow(const TempSample& sample, bool terminal) {
    if (sampleCount_ >= recordLimit) {
        failStop(kErrStorageFull);
        return false;
    }
    if (sample.seq == 0 || sample.ms > maxDurationMs ||
        (sampleCount_ > 0 && (sample.ms <= lastMs_ || sample.seq <= lastAcqSeq_))) {
        failStop(kErrInvalidSample);
        return false;
    }

    uint8_t flags = 0;
    if (terminal) {
        flags |= kFlagTerminal;
    }
    if (gapPending_) {
        flags |= kFlagGap;
    }

    char tempBuf[24];
    const char* tempText = "";
    if (isfinite(sample.tempC)) {
        const int tn = snprintf(tempBuf, sizeof(tempBuf), "%.9g", static_cast<double>(sample.tempC));
        if (tn <= 0 || static_cast<size_t>(tn) >= sizeof(tempBuf)) {
            failStop(kErrWriteFailed);
            return false;
        }
        tempText = tempBuf;
    }

    char row[kMaxRowBytes + 1];
    const int n = snprintf(row, sizeof(row), "%lu,%lu,%lu,%lu,%s,%u,%u,%u\n",
                           static_cast<unsigned long>(runId_),
                           static_cast<unsigned long>(sampleCount_ + 1),
                           static_cast<unsigned long>(sample.seq),
                           static_cast<unsigned long>(sample.ms),
                           tempText,
                           static_cast<unsigned>(sample.fault),
                           static_cast<unsigned>(pendingIntervalFault_),
                           static_cast<unsigned>(flags));
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(row)) {
        failStop(kErrWriteFailed);
        return false;
    }

    // Conservative capacity gate: never let the run exhaust the volume.
    refreshFreeBytes();
    if (freeBytesCache_ < kSpaceFloorBytes + static_cast<uint32_t>(n)) {
        failStop(kErrStorageFull);
        return false;
    }

    if (!storage_.writeAll(reinterpret_cast<const uint8_t*>(row), static_cast<size_t>(n))) {
        failStop(kErrWriteFailed);
        return false;
    }
    // The row is fully in the file and structurally valid: account for it now
    // so Status always matches the validated prefix (also across reboot).
    validatedBytes_ += static_cast<uint32_t>(n);
    ++sampleCount_;
    lastAcqSeq_ = sample.seq;
    lastMs_ = sample.ms;
    pendingIntervalFault_ = 0;
    gapPending_ = false;
    refreshFreeBytes();
    if (!storage_.sync()) {
        failStop(kErrSyncFailed);
        return false;
    }
    return true;
}

bool SingleRunCsv::finishRun(const char* reason, uint32_t elapsedMs, bool selectLatest) {
    if (!recording_) {
        return false;
    }
    if (selectLatest && haveLastSample_ && lastSample_.ms <= maxDurationMs &&
        (sampleCount_ == 0 || lastSample_.seq != lastAcqSeq_) && sampleCount_ < recordLimit) {
        if (!logRow(lastSample_, true)) {
            return false;
        }
    }
    return endRun(reason, elapsedMs);
}

bool SingleRunCsv::endRun(const char* reason, uint32_t elapsedMs) {
    char footer[kFooterBytes + 1];
    const int n = snprintf(footer, sizeof(footer), "# end,reason=%s,rows=%lu,elapsed_ms=%lu\n",
                           reason, static_cast<unsigned long>(sampleCount_),
                           static_cast<unsigned long>(elapsedMs));
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(footer)) {
        failStop(kErrWriteFailed);
        return false;
    }
    if (!storage_.writeAll(reinterpret_cast<const uint8_t*>(footer), static_cast<size_t>(n))) {
        failStop(kErrWriteFailed);
        return false;
    }
    validatedBytes_ += static_cast<uint32_t>(n);
    refreshFreeBytes();
    if (!storage_.sync()) {
        failStop(kErrSyncFailed);
        return false;
    }
    storage_.closeWriter();
    recording_ = false;
    state_ = reason;
    return true;
}

bool SingleRunCsv::stop(uint32_t elapsedMs) {
    if (!recording_) {
        return false;
    }
    if (haveLastSample_ && elapsedMs < lastSample_.ms) {
        failStop(kErrInvalidSample);
        return false;
    }
    // The caller may enforce the duration cap independently of samples; the
    // run must then finalize as duration_limit, not complete.
    const char* reason = (elapsedMs >= maxDurationMs) ? kStateDurationLimit : kStateComplete;
    return finishRun(reason, elapsedMs > maxDurationMs ? maxDurationMs : elapsedMs, true);
}

bool SingleRunCsv::erase(uint32_t expectedRunId) {
    if (recording_ || readerActive_ || !storage_.mounted()) {
        return false;
    }
    // Exact id match; expectedRunId == 0 is the explicit delete for a saved
    // file whose id is unrecoverable (corrupt/incomplete metadata). Nothing is
    // ever erased implicitly.
    if (!hasRun_ || expectedRunId != runId_) {
        return false;
    }
    if (!storage_.removeFile()) {
        error_ = kErrRemoveFailed;
        state_ = kStateError;
        refreshFreeBytes();
        return false;
    }
    hasRun_ = false;
    runId_ = 0;
    sampleCount_ = 0;
    validatedBytes_ = 0;
    lastAcqSeq_ = 0;
    lastMs_ = 0;
    haveLastSample_ = false;
    pendingIntervalFault_ = 0;
    gapPending_ = false;
    error_ = nullptr;
    state_ = kStateIdle;
    refreshFreeBytes();
    return true;
}

bool SingleRunCsv::format() {
    if (recording_ || readerActive_ || hasRun_) {
        return false;
    }
    if (!storage_.eraseVolume()) {
        error_ = kErrFormatFailed;
        state_ = kStateError;
        return false;
    }
    runId_ = 0;
    sampleCount_ = 0;
    validatedBytes_ = 0;
    lastAcqSeq_ = 0;
    lastMs_ = 0;
    haveLastSample_ = false;
    pendingIntervalFault_ = 0;
    gapPending_ = false;
    error_ = nullptr;
    state_ = kStateIdle;
    refreshFreeBytes();
    return true;
}

Status SingleRunCsv::status() const {
    Status snapshot;
    snapshot.mounted = storage_.mounted();
    snapshot.hasRun = hasRun_;
    snapshot.recording = recording_;
    snapshot.runId = runId_;
    snapshot.sampleCount = sampleCount_;
    snapshot.bytes = validatedBytes_;
    snapshot.freeBytes = snapshot.mounted ? freeBytesCache_ : 0;
    snapshot.state = state_;
    snapshot.error = error_;
    return snapshot;
}

bool SingleRunCsv::openDownload(uint32_t expectedRunId) {
    if (recording_ || readerActive_ || !storage_.mounted()) {
        return false;
    }
    if (!hasRun_ || expectedRunId != runId_ || validatedBytes_ == 0) {
        return false;
    }
    if (!storage_.openReader()) {
        return false;
    }
    readerActive_ = true;
    downloadSent_ = 0;
    return true;
}

size_t SingleRunCsv::readDownload(uint8_t* dst, size_t cap) {
    if (!readerActive_ || dst == nullptr || cap == 0) {
        return 0;
    }
    const size_t remaining = validatedBytes_ - downloadSent_;
    if (remaining == 0) {
        return 0;
    }
    if (cap > remaining) {
        cap = remaining;
    }
    const int32_t n = storage_.read(dst, cap);
    if (n <= 0) {
        return 0;  // EOF or error: never serves bytes beyond the validated prefix
    }
    downloadSent_ += static_cast<size_t>(n);
    return static_cast<size_t>(n);
}

void SingleRunCsv::closeDownload() {
    if (readerActive_) {
        storage_.closeReader();
        readerActive_ = false;
        downloadSent_ = 0;
    }
}

bool SingleRunCsv::scanSavedRun() {
    if (!storage_.openReader()) {
        return false;
    }

    uint8_t chunk[1024];
    char line[kMaxLineBytes + 2];
    size_t lineLen = 0;
    uint32_t offset = 0;
    uint32_t contentEnd = 0;  // end of the last validated metadata/header/row line
    uint32_t footerEnd = 0;
    uint32_t rows = 0;
    uint32_t lastAcq = 0;
    uint32_t lastMs = 0;
    uint32_t metaRunId = 0;
    int stage = 0;  // 0..5 metadata lines, 6 column header, 7 rows, 8 footer
    bool ioOk = true;
    bool done = false;
    bool terminalSeen = false;
    const char* endReason = nullptr;

    while (!done) {
        const int32_t n = storage_.read(chunk, sizeof(chunk));
        if (n < 0) {
            ioOk = false;
            break;
        }
        if (n == 0) {
            break;  // EOF; an unterminated trailing line is never validated
        }
        for (int32_t i = 0; i < n; ++i) {
            const char c = static_cast<char>(chunk[i]);
            ++offset;
            if (stage == 8) {
                // The footer must be the last byte of the file.
                stage = 7;
                done = true;
                break;
            }
            if (c == '\n') {
                line[lineLen] = '\0';
                const char* b = line;
                const char* e = line + lineLen;
                bool valid = false;
                switch (stage) {
                    case 0:
                        valid = spanEq(b, e, kFormatIdLine);
                        if (valid) stage = 1;
                        break;
                    case 1: {
                        const char* p = nullptr;
                        valid = spanStartsWith(b, e, "# run_id,", p) && spanU32(p, e, metaRunId) &&
                                metaRunId > 0;
                        if (valid) stage = 2;
                        break;
                    }
                    case 2: {
                        const char* p = nullptr;
                        uint32_t v = 0;
                        valid = spanStartsWith(b, e, "# acquire_interval_ms,", p) &&
                                spanU32(p, e, v) && v == acquireIntervalMs;
                        if (valid) stage = 3;
                        break;
                    }
                    case 3: {
                        const char* p = nullptr;
                        uint32_t v = 0;
                        valid = spanStartsWith(b, e, "# log_interval_ms,", p) &&
                                spanU32(p, e, v) && v == logIntervalMs;
                        if (valid) stage = 4;
                        break;
                    }
                    case 4: {
                        const char* p = nullptr;
                        uint32_t v = 0;
                        valid = spanStartsWith(b, e, "# max_duration_ms,", p) &&
                                spanU32(p, e, v) && v == maxDurationMs;
                        if (valid) stage = 5;
                        break;
                    }
                    case 5: {
                        const char* p = nullptr;
                        uint32_t v = 0;
                        valid = spanStartsWith(b, e, "# record_limit,", p) &&
                                spanU32(p, e, v) && v == recordLimit;
                        if (valid) stage = 6;
                        break;
                    }
                    case 6:
                        valid = spanEq(b, e, kColumnHeader);
                        if (valid) stage = 7;
                        break;
                    case 7: {
                        if (lineLen > 6 && memcmp(line, "# end,", 6) == 0) {
                            uint32_t footerRows = 0;
                            uint32_t footerElapsed = 0;
                            if (parseFooterLine(b, e, endReason, footerRows, footerElapsed) &&
                                footerRows == rows && footerElapsed >= lastMs &&
                                footerElapsed <= maxDurationMs &&
                                (endReason != kStateDurationLimit || footerElapsed == maxDurationMs)) {
                                valid = true;
                                stage = 8;
                                footerEnd = offset;
                            }
                            break;
                        }
                        RowValues row;
                        valid = !terminalSeen && parseRowLine(b, e, metaRunId, rows, lastAcq, lastMs, row);
                        if (valid) {
                            ++rows;
                            lastAcq = row.acqSeq;
                            lastMs = row.elapsedMs;
                            terminalSeen = (row.flags & kFlagTerminal) != 0;
                        }
                        break;
                    }
                    default:
                        valid = false;
                        break;
                }
                if (!valid) {
                    done = true;
                    break;
                }
                if (stage != 8) {
                    contentEnd = offset;
                }
                lineLen = 0;
            } else {
                if (lineLen >= kMaxLineBytes) {
                    done = true;  // line too long to be valid: stop the prefix here
                    break;
                }
                line[lineLen++] = c;
            }
        }
    }

    storage_.closeReader();
    if (!ioOk) {
        return false;
    }

    if (stage == 8) {
        runId_ = metaRunId;
        sampleCount_ = rows;
        validatedBytes_ = footerEnd;
        state_ = endReason;
    } else if (stage == 7) {
        runId_ = metaRunId;
        sampleCount_ = rows;
        validatedBytes_ = contentEnd;
        state_ = kStateInterrupted;
    } else {
        // Metadata/header block not valid: the saved file is corrupt. Keep
        // hasRun_ (the file must stay visible and block start), expose no
        // partial identity, and require the explicit erase(0).
        runId_ = 0;
        sampleCount_ = 0;
        validatedBytes_ = 0;
        error_ = kErrCorruptSavedRun;
        state_ = kStateError;
    }
    lastAcqSeq_ = lastAcq;
    lastMs_ = lastMs;
    haveLastSample_ = false;
    pendingIntervalFault_ = 0;
    gapPending_ = false;
    return true;
}

}  // namespace csvrun
