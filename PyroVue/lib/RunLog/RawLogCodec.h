#ifndef RAW_LOG_CODEC_H
#define RAW_LOG_CODEC_H

// Portable fixed-record codec for the durable raw run log (samples.bin).
//
// No Arduino/platform dependencies: only C standard headers plus the shared
// runlog types. All encoding happens into caller-provided byte buffers; the
// codec never allocates. Filesystem integration is intentionally NOT here.
//
// On-disk file layout:
//
//   +---------------------+
//   | header (32 bytes)   |
//   +---------------------+
//   | record 0 (24 B)     |
//   | record 1 (24 B)     |
//   | ...                 |  appended at the log cadence, one LoggedSample each
//   +---------------------+
//
// All multi-byte fields are LITTLE-ENDIAN, explicit byte-wise serialization
// (no struct casts, no alignment/packing assumptions across platforms);
// float temperature is written as its IEEE-754 bit pattern via memcpy, so any
// value (negative, negative zero, NaN payloads, faults) round-trips exactly.
//
// Header (32 bytes):
//   offset  0 : magic[4]          = 'P','V','R','1'
//   offset  4 : version (u16)     = 1 (rawlogcodec::kVersion)
//   offset  6 : headerBytes (u16) = 32 (rawlogcodec::kHeaderSize)
//   offset  8 : runId (u32)       = run these records belong to (> 0)
//   offset 12 : acquireIntervalMs = 1000 (runlog::kAcquireIntervalMs)
//   offset 16 : logIntervalMs     = 2000 (runlog::kLogIntervalMs)
//   offset 20 : maxDurationMs     = 86400000 (runlog::kMaxDurationMs)
//   offset 24 : reserved (u32)    = 0 (must be zero on disk)
//   offset 28 : crc32 (u32)       = persistence::crc32 over header bytes 0..27
//
// Record (24 bytes), one per LoggedSample:
//   offset  0 : logSeq (u32)       = consecutive recorded sequence, starts at 1
//   offset  4 : acqSeq (u32)       = original acquisition identity (> 0)
//   offset  8 : ms (u32)           = run-relative elapsed ms, 0..maxDurationMs
//   offset 12 : tempC (f32)        = IEEE-754 bit pattern (any bits valid)
//   offset 16 : fault (u8)         = SensorFault bitmask
//   offset 17 : intervalFault (u8) = faults ORed since the previous selection
//   offset 18 : flags (u8)         = bit0 terminal, bit1 acquisition_gap;
//                                    every other bit MUST be zero on disk
//   offset 19 : reserved (u8)      = 0 (must be zero on disk)
//   offset 20 : crc32 (u32)        = persistence::crc32 over record bytes 0..19
//
// Strictness. decodeHeader rejects any deviation from the layout above:
// wrong magic, version, or headerBytes; zero runId; rate/duration fields that
// differ from the runlog schema constants; nonzero reserved; bad CRC.
// decodeRecord rejects a truncated buffer, bad CRC, nonzero reserved byte,
// undefined flag bits, zero logSeq/acqSeq, and ms beyond runlog::kMaxDurationMs
// (the format fixes the maximum duration, so no per-record header is needed).
// scanRecords adds the cross-record invariants: consecutive logSeq starting
// at 1, strictly increasing acqSeq and ms relative to the previous validated
// record, and no record after a terminal record. Gaps in time or acqSeq are
// data, not corruption. Every decoder leaves its output untouched on failure,
// and scanning advances the cursor ONLY for validated records, so the
// validated prefix is always exact.
//
// Recovery model: records are appended back-to-back after the header. Scanning
// stops at the first invalid/incomplete record; everything before it is the
// valid prefix and the exact append position for a healthy writer (interrupted
// writers are never resumed; that policy lives in the store, not here).

#include <stddef.h>
#include <stdint.h>

#include "RunLogTypes.h"  // LoggedSample, runlog:: constants

namespace rawlogcodec {

constexpr uint8_t kMagic[4] = {'P', 'V', 'R', '1'};
constexpr uint16_t kVersion = 1;

constexpr size_t kHeaderSize = 32;
constexpr size_t kRecordSize = 24;

// Header field offsets (exposed for fixtures; keep in sync with the layout).
constexpr size_t kHeaderMagicOffset = 0;
constexpr size_t kHeaderVersionOffset = 4;
constexpr size_t kHeaderBytesOffset = 6;
constexpr size_t kHeaderRunIdOffset = 8;
constexpr size_t kHeaderAcquireIntervalMsOffset = 12;
constexpr size_t kHeaderLogIntervalMsOffset = 16;
constexpr size_t kHeaderMaxDurationMsOffset = 20;
constexpr size_t kHeaderReservedOffset = 24;
constexpr size_t kHeaderCrcOffset = 28;

// Record field offsets (exposed for fixtures; keep in sync with the layout).
constexpr size_t kRecordLogSeqOffset = 0;
constexpr size_t kRecordAcqSeqOffset = 4;
constexpr size_t kRecordMsOffset = 8;
constexpr size_t kRecordTempCOffset = 12;
constexpr size_t kRecordFaultOffset = 16;
constexpr size_t kRecordIntervalFaultOffset = 17;
constexpr size_t kRecordFlagsOffset = 18;
constexpr size_t kRecordReservedOffset = 19;
constexpr size_t kRecordCrcOffset = 20;

// Flags written at kRecordFlagsOffset (same bits as runlog::kTerminal /
// runlog::kAcquisitionGap); any other bit makes the record invalid.
constexpr uint8_t kTerminalFlag = runlog::kTerminal;
constexpr uint8_t kAcquisitionGapFlag = runlog::kAcquisitionGap;
constexpr uint8_t kFlagsMask = 0x03;

// Header carries the four data fields of the binary layout; magic, version,
// headerBytes, reserved and CRC are fixed format values handled internally.
// Default-constructed fields match the fixed on-disk values.
struct Header {
  uint32_t runId{};
  uint32_t acquireIntervalMs{runlog::kAcquireIntervalMs};
  uint32_t logIntervalMs{runlog::kLogIntervalMs};
  uint32_t maxDurationMs{runlog::kMaxDurationMs};
};

// Cursor for incremental, page-at-a-time validation of the record region.
// State carries cross-page ordering (consecutive logSeq, strictly increasing
// acqSeq/ms) and the terminal condition, so a file read in arbitrary chunks
// cannot fabricate order across page boundaries.
//
// Cursor constraints (callers MUST follow):
//  - Start every file scan with a fresh `ScanCursor cursor{};`
//    (nextLogSeq=1, lastAcqSeq=0, lastMs=0, hasPrevious=false,
//    terminalSeen=false) and reuse that SAME cursor for every page of the
//    file. Never share a cursor between files or runs.
//  - Each scanRecords() call must start at a record boundary: a byte offset
//    that is an exact multiple of kRecordSize from the first record after the
//    32-byte header. Trailing bytes shorter than one record are never
//    consumed; the caller keeps them and prepends them to the next page.
//  - The cursor advances ONLY for validated records; a record that fails any
//    check stops the scan and leaves the cursor at the last validated record.
//  - After a terminal record is validated, terminalSeen is set and every later
//    call validates nothing and leaves the cursor untouched: records after
//    terminal are rejected and the valid prefix ends at the terminal record.
//  - logSeq never wraps in practice (record budget 43202); if a scan ever
//    consumed logSeq 0xFFFFFFFF the cursor would wrap to 0 and no later
//    record could validate, which is a safe stop, not a bypass.
struct ScanCursor {
  uint32_t nextLogSeq{1};
  uint32_t lastAcqSeq{0};
  uint32_t lastMs{0};
  bool hasPrevious{false};
  bool terminalSeen{false};
};

// Result of scanning one buffer of records. Describes this call only;
// cross-call state lives in the ScanCursor.
struct ScanResult {
  size_t validCount{};   // records validated by this call
  size_t validBytes{};   // bytes consumed by this call (validCount * kRecordSize),
                         // relative to the buffer passed in
  uint32_t lastLogSeq{}; // logSeq of the last record validated by this call,
                         // 0 if this call validated nothing
};

// Serializes the header. Returns kHeaderSize on success, 0 on insufficient
// capacity or invalid input (null out, runId == 0, or rate/duration fields
// that differ from the runlog schema constants); out is left untouched then.
size_t encodeHeader(uint8_t* out, size_t cap, const Header& header);

// Parses and validates a header: magic, version, headerBytes, runId,
// rate/duration fields, reserved, CRC. Returns true and fills `header` on
// success; on failure `header` is left untouched.
bool decodeHeader(const uint8_t* in, size_t len, Header& header);

// Serializes one record. Returns kRecordSize on success, 0 on insufficient
// capacity or invalid input (null out, logSeq/acqSeq == 0, ms beyond
// runlog::kMaxDurationMs, undefined flag bits); out is left untouched then.
// tempC is never validated: every float bit pattern round-trips.
size_t encodeRecord(uint8_t* out, size_t cap, const LoggedSample& sample);

// Parses and validates one record: bounds, CRC, reserved byte, flag bits,
// positive identities, ms within runlog::kMaxDurationMs. Returns true and
// fills `sample` on success; on failure `sample` is left untouched. Ordering,
// sequence and terminal rules are cross-record and enforced by scanRecords.
bool decodeRecord(const uint8_t* in, size_t len, LoggedSample& sample);

// Scans one record-aligned buffer of records, continuing the state in
// `cursor`, and returns the valid prefix of this call. Stops at the first
// record that fails decodeRecord or a cross-record rule (nonconsecutive
// logSeq, non-increasing acqSeq/ms, record after terminal) and never reads
// past the buffer. On return the cursor reflects exactly the validated
// records; the overall durable watermark is cursor.nextLogSeq - 1.
ScanResult scanRecords(const uint8_t* in, size_t len, ScanCursor& cursor);

}  // namespace rawlogcodec

#endif  // RAW_LOG_CODEC_H
