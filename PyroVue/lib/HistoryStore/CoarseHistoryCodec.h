#ifndef COARSE_HISTORY_CODEC_H
#define COARSE_HISTORY_CODEC_H

// Portable fixed-record persistence codec for coarse history buckets.
//
// No Arduino/platform dependencies: only C standard headers. All encoding
// happens into caller-provided byte buffers; the codec never allocates.
// LittleFS/filesystem integration is intentionally NOT here.
//
// On-disk file layout:
//
//   +--------------------+
//   | header (20 bytes)  |
//   +--------------------+
//   | record 0 (24 B)    |
//   | record 1 (24 B)    |
//   | ...                |   appended at low cadence, one bucket each
//   +--------------------+
//
// All multi-byte fields are LITTLE-ENDIAN, explicit byte-wise serialization
// (no struct casts, no alignment/packing assumptions across platforms).
//
// Header (20 bytes):
//   offset  0 : magic[4]        = 'P','V','C','1'
//   offset  4 : version (u8)    = COARSE_CODEC_VERSION (1)
//   offset  5 : reserved[3]     = 0x00 (must be zero; ignored on decode)
//   offset  8 : bucketMs (u32)  = coarse bucket resolution in milliseconds
//   offset 12 : runId    (u32)  = run the persisted buckets belong to
//   offset 16 : crc32    (u32)  = IEEE CRC-32 (poly 0xEDB88320) over header
//                                    bytes 0..15 (crc field itself excluded)
//
// Record (24 bytes), one per CoarseSample:
//   offset  0 : ms       (u32)  = run-relative bucket start in milliseconds
//   offset  4 : averageC (f32)  = IEEE-754 bit pattern
//   offset  8 : minimumC (f32)
//   offset 12 : maximumC (f32)
//   offset 16 : fault    (u8)   = SensorFault bitmask
//   offset 17 : reserved[3]     = 0x00
//   offset 20 : crc32    (u32)  = CRC-32 over record bytes 0..19
//
// Recovery model: records are appended back-to-back after the header. A
// decoder scans whole records from the start of the record region; the first
// record with a bad CRC (or a truncated tail shorter than one record) ends
// the scan. Everything before that point is valid and usable, and the scan
// result gives the exact append offset to resume writing after a crash.

#include <stddef.h>
#include <stdint.h>

#include "protocol.h"  // CoarseSample (portable stdint-based contract)

namespace coarsecodec {

constexpr uint8_t kMagic[4] = {'P', 'V', 'C', '1'};
constexpr uint8_t kVersion = 1;

constexpr size_t kHeaderSize = 20;
constexpr size_t kRecordSize = 24;

// Result of scanning a record region for the valid prefix.
struct ScanResult {
  // Byte offset just past the last valid record, relative to the buffer
  // passed to scanRecords(). This is the safe append position.
  size_t appendOffset;
  uint32_t validCount;  // number of consecutive valid records found
};

struct Header {
  uint32_t bucketMs;
  uint32_t runId;
};

// Serializes the header. Returns kHeaderSize on success, 0 if cap is too
// small (out is left untouched in that case).
size_t encodeHeader(uint8_t* out, size_t cap, const Header& header);

// Parses and validates a header: magic, version, reserved bytes, CRC.
// Returns true and fills `header` on success; returns false on truncation
// or corruption (header contents undefined unless true).
bool decodeHeader(const uint8_t* in, size_t len, Header& header);

// Serializes one coarse sample record. Returns kRecordSize on success,
// 0 if cap is too small.
size_t encodeRecord(uint8_t* out, size_t cap, const CoarseSample& sample);

// Parses and validates one record (CRC checked). Returns true on success.
bool decodeRecord(const uint8_t* in, size_t len, CoarseSample& sample);

// Scans `len` bytes of records (buffer starts at the first record, i.e.
// just past the header) and reports the valid prefix. Trailing truncated
// bytes or bad-CRC records are rejected; earlier valid records remain
// usable. Never reads past the buffer.
ScanResult scanRecords(const uint8_t* in, size_t len);

// Convenience: decode the header and scan the record region in one call.
// `data` points at the file start (header included). On success returns
// true and fills `header` plus `scan` (appendOffset is relative to `data`,
// so it includes kHeaderSize and can be used as the append position for
// the whole file). Returns false if the header itself is invalid.
bool scan(const uint8_t* data, size_t len, Header& header, ScanResult& scan);

}  // namespace coarsecodec

#endif  // COARSE_HISTORY_CODEC_H
