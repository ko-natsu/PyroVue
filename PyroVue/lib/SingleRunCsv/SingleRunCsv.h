#ifndef SINGLE_RUN_CSV_H
#define SINGLE_RUN_CSV_H

// Single-run durable CSV temperature recorder (portable core).
//
// Records the selected real readings of ONE firing run as an explicit,
// self-describing CSV file on a borrowed byte-storage backend (IStorage).
// Nothing here touches Arduino or any filesystem API directly: the physical
// adapter lives in FeatherCsvStorage (ESP32 Arduino only) and native tests
// supply their own IStorage. No heap allocation, no PSRAM, no RAM history:
// every method runs on the loop-owner task only (no locking provided).
//
// Selection policy (all samples are consumed for fault accumulation):
//   - the first acquired sample is logged immediately (durable "first"),
//   - then the first sample with elapsed >= logIntervalMs since the
//     previously logged row (durable "3s" readings),
//   - end of run logs the latest sample if it was not already selected
//     (durable "terminal" reading).
// Rows preserve the sample's ACTUAL runId-independent identity: acq_seq is
// TempSample.seq, elapsed_ms is TempSample.ms, temp_c is TempSample.tempC.
//
// On-disk format (UTF-8 text, LF line endings, "%.9g" float formatting):
//
//   # pyrovue-single-run-csv,1
//   # run_id,123
//   # acquire_interval_ms,500
//   # log_interval_ms,3000
//   # max_duration_ms,21600000
//   # record_limit,7202
//   run_id,log_seq,acq_seq,elapsed_ms,temp_c,fault,interval_fault,flags
//   123,1,1,0,23.5,0,0,0
//   123,2,7,3000,,2,2,1
//   ...
//   # end,reason=complete,rows=2,elapsed_ms=3000
//
//   * Metadata block: exactly the six '#' lines above, in order. Values are
//     pinned to the format (500/3000ms rates, 6h cap, row limit) so a file
//     never lies about its cadence; run_id must be > 0.
//   * Column header: exactly the line shown above.
//   * Data rows: exactly 8 comma-separated fields.
//       run_id        == metadata run_id
//       log_seq       1-based, strictly consecutive from 1
//       acq_seq       original acquisition identity, strictly increasing
//       elapsed_ms    run-relative ms, strictly increasing, <= maxDurationMs
//       temp_c        "%.9g" of the float (lossless for float32), or EMPTY
//                     when the value is non-finite
//       fault         SensorFault bitmask of that sample (decimal 0..255)
//       interval_fault  OR of fault over every acquired sample since the
//                     previous logged row, including this one (decimal 0..255)
//       flags         bit0 kFlagTerminal: row appended by end-of-run
//                     selection (stop or duration boundary)
//                     bit1 kFlagGap: sticky acquisition gap observed before
//                     this row was written. A gap is raised between two
//                     consecutive ACQUIRED samples when dt > 1.5*
//                     acquireIntervalMs or acq_seq != previous+1 (checked for
//                     every sample, so a hole can never hide inside a normal
//                     logged interval). The flag ORs into the next selected
//                     or terminal row, then resets. The first acquired sample
//                     is never gap-flagged (no prior baseline).
//   * Footer: exactly "# end,reason=<complete|duration_limit>,rows=<n>,
//     elapsed_ms=<n>". reason=complete for an explicit stop(), reason=
//     duration_limit when the run ended at the duration boundary. rows must
//     equal the validated row count. elapsed_ms is the stop-time run elapsed
//     (informational). NO VALID FOOTER -> the run is "interrupted".
//
// Recovery: begin() scans the saved file line by line in bounded buffers and
// exposes ONLY the validated prefix (metadata + header + rows + footer, if
// the footer is the last byte of the file). A truncated/corrupt tail never
// discards the good prefix and the file is never rewritten or deleted
// implicitly. Rows after a footer line are treated as corruption. A file
// whose metadata/header block is not valid is reported as error state
// "corrupt_saved_run" with runId 0 (it still blocks start() until an
// explicit erase(0) or format()). The last active run survives reboot as
// "interrupted" and is never resumed.
//
// Capacity: start() requires startRequiredBytes free. Rows are append-only
// and the format bounds (recordLimit rows of kMaxRowBytes plus header/footer)
// keep the whole file inside the 512KiB run allowance (checked at compile
// time below). During recording the run stops with error "storage_full"
// before the free space falls under kSpaceFloorBytes. Every write is checked
// and every boundary (header, each row, footer) is fsync'd via IStorage;
// any write/sync failure stops recording with a stable error literal and
// leaves the validated prefix on disk.

#include <stddef.h>
#include <stdint.h>

#include "protocol.h"  // TempSample

namespace csvrun {

constexpr uint32_t acquireIntervalMs = 500;
constexpr uint32_t logIntervalMs = 3000;
constexpr uint32_t maxDurationMs = 21600000;     // 6h
constexpr uint32_t startRequiredBytes = 524288;  // 512KiB run allowance
constexpr uint32_t recordLimit = 7202;           // max data rows per run

// Format bounds (exposed for tests; see the layout documentation above).
constexpr size_t kMaxRowBytes = 64;      // worst case row incl. '\n' is 62
constexpr size_t kMaxLineBytes = 96;     // longest valid line incl. '\n'
constexpr size_t kMetaBlockBytes = 512;  // bound for the 6 metadata lines
constexpr size_t kHeaderLineBytes = 96;  // bound for the column header line
constexpr size_t kFooterBytes = 72;      // bound for the footer line
constexpr uint32_t kSpaceFloorBytes = 8192;  // stop before exhausting volume

// flags column bits (same semantics as runlog::kTerminal/kAcquisitionGap).
constexpr uint8_t kFlagTerminal = 1;
constexpr uint8_t kFlagGap = 2;
constexpr uint8_t kFlagMask = 0x03;

// Stable error literals (Status::error); nullptr means "no error".
constexpr const char* kErrMountFailed = "mount_failed";
constexpr const char* kErrReadFailed = "read_failed";
constexpr const char* kErrCreateFailed = "create_failed";
constexpr const char* kErrWriteFailed = "write_failed";
constexpr const char* kErrSyncFailed = "sync_failed";
constexpr const char* kErrStorageFull = "storage_full";
constexpr const char* kErrRemoveFailed = "remove_failed";
constexpr const char* kErrFormatFailed = "format_failed";
constexpr const char* kErrCorruptSavedRun = "corrupt_saved_run";
constexpr const char* kErrInvalidSample = "invalid_sample";

// Stable state literals (Status::state).
constexpr const char* kStateIdle = "idle";
constexpr const char* kStateRecording = "recording";
constexpr const char* kStateComplete = "complete";
constexpr const char* kStateInterrupted = "interrupted";
constexpr const char* kStateDurationLimit = "duration_limit";
constexpr const char* kStateError = "error";

// Compile-time proof that a full run stays inside the 512KiB allowance.
static_assert(kMetaBlockBytes + kHeaderLineBytes +
                  static_cast<size_t>(recordLimit) * kMaxRowBytes + kFooterBytes <=
                  startRequiredBytes,
              "run file must fit inside the startRequiredBytes allowance");

// Borrowed byte-storage backend: exactly one saved-run file on one volume.
// All calls run on the loop-owner task. Implementations MUST check every
// operation and MUST NOT format on mount; the single destructive operation
// is eraseVolume(), reachable only through SingleRunCsv::format().
class IStorage {
public:
    virtual ~IStorage() = default;

    // Volume lifecycle.
    virtual bool mount() = 0;      // attach an EXISTING volume; never formats
    virtual bool mounted() const = 0;
    virtual bool eraseVolume() = 0;  // erase ENTIRE volume, remount it empty

    // Space accounting for the mounted volume.
    virtual uint32_t freeBytes() = 0;

    // The single saved-run file (backend-chosen fixed name).
    virtual bool fileExists() = 0;
    virtual bool removeFile() = 0;

    // Writer: createExclusive MUST fail if the file already exists.
    // writeAll appends checked bytes (all of them or false); sync makes the
    // appended bytes durable (fsync), closeWriter releases the handle.
    virtual bool createExclusive() = 0;
    virtual bool writeAll(const uint8_t* data, size_t len) = 0;
    virtual bool sync() = 0;
    virtual void closeWriter() = 0;

    // Reader: sequential from byte 0. read returns bytes read (0 at EOF) or
    // a negative value on I/O error.
    virtual bool openReader() = 0;
    virtual int32_t read(uint8_t* dst, size_t cap) = 0;
    virtual void closeReader() = 0;
};

// Cached recorder snapshot; state/error point at stable literals, never at
// dynamic buffers.
struct Status {
    bool mounted;
    bool hasRun;
    bool recording;
    uint32_t runId;
    uint32_t sampleCount;
    uint32_t bytes;
    uint32_t freeBytes;
    const char* state;
    const char* error;
};

class SingleRunCsv {
public:
    explicit SingleRunCsv(IStorage& storage);

    // Mounts the existing volume and recovers any saved run (boot scan).
    // Returns false only on storage failure (mount/read); structurally
    // corrupt content is recovered as error state "corrupt_saved_run" and
    // still returns true. Safe to call again after a failure.
    bool begin();

    // True when start() would succeed (space, no saved run, no reader...).
    bool canStart();

    // Starts a new run file (runId > 0). Refuses to overwrite an existing
    // file; a saved run must be erased explicitly first. Writes and syncs
    // the metadata/header block before returning true.
    bool start(uint32_t runId);

    // Consumes one acquired sample (selection + fault/gap accumulation).
    void onSample(const TempSample& sample);

    // Ends the run: selects the latest sample if it was not already selected,
    // then syncs the footer. elapsedMs is the run elapsed at stop time (>=
    // last sample ms). The run finalizes as "duration_limit" when
    // elapsedMs >= maxDurationMs, otherwise as "complete" (the caller may
    // enforce the duration cap independently of samples).
    bool stop(uint32_t elapsedMs);

    // Explicitly deletes the saved run file. Requires an exact runId match
    // against the recovered run id (0 when unrecoverable), no active
    // recording and no open download.
    bool erase(uint32_t expectedRunId);

    // Erases the ENTIRE volume and remounts it empty. Only reachable behind
    // an explicit user confirmation; refused while recording, while a saved
    // run exists or while a download is open.
    bool format();

    Status status() const;

    // Bounded read-only download of the saved file's validated CSV prefix.
    // Allowed only when not recording and the saved file is a valid CSV
    // (complete/duration_limit/interrupted); refuses stale run ids. While a
    // reader is active, start/erase/format are refused. readDownload returns
    // bytes copied, 0 at the validated-prefix EOF or on error.
    bool openDownload(uint32_t expectedRunId);
    size_t readDownload(uint8_t* dst, size_t cap);
    void closeDownload();

private:
    void failStop(const char* error);
    void refreshFreeBytes();
    bool logRow(const TempSample& sample, bool terminal);
    bool finishRun(const char* reason, uint32_t elapsedMs, bool selectLatest);
    bool endRun(const char* reason, uint32_t elapsedMs);
    bool scanSavedRun();

    IStorage& storage_;
    bool recording_ = false;
    bool readerActive_ = false;
    bool hasRun_ = false;
    bool haveLastSample_ = false;
    TempSample lastSample_{};
    uint32_t runId_ = 0;
    uint32_t sampleCount_ = 0;
    uint32_t validatedBytes_ = 0;
    uint32_t freeBytesCache_ = 0;
    uint32_t lastAcqSeq_ = 0;
    uint32_t lastMs_ = 0;
    uint32_t downloadSent_ = 0;
    uint8_t pendingIntervalFault_ = 0;
    bool gapPending_ = false;
    const char* state_ = kStateIdle;
    const char* error_ = nullptr;
};

}  // namespace csvrun

#endif  // SINGLE_RUN_CSV_H
