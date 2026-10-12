#include "RawLogCodec.h"

#include <string.h>

#include "Crc32.h"  // persistence::crc32: shared IEEE CRC-32

namespace rawlogcodec {
namespace {

void putU16(uint8_t* out, uint16_t v) {
  out[0] = static_cast<uint8_t>(v & 0xFFu);
  out[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
}

uint16_t getU16(const uint8_t* in) {
  return static_cast<uint16_t>(static_cast<uint16_t>(in[0]) |
                               (static_cast<uint16_t>(in[1]) << 8));
}

void putU32(uint8_t* out, uint32_t v) {
  out[0] = static_cast<uint8_t>(v & 0xFFu);
  out[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
  out[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
  out[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

uint32_t getU32(const uint8_t* in) {
  return static_cast<uint32_t>(in[0]) |
         (static_cast<uint32_t>(in[1]) << 8) |
         (static_cast<uint32_t>(in[2]) << 16) |
         (static_cast<uint32_t>(in[3]) << 24);
}

void putF32(uint8_t* out, float v) {
  // IEEE-754 float is required by the platform toolchains (ESP8266, ESP32-S3,
  // host gcc/clang); memcpy moves the bit pattern without aliasing games.
  uint32_t bits;
  memcpy(&bits, &v, sizeof(bits));
  putU32(out, bits);
}

float getF32(const uint8_t* in) {
  uint32_t bits = getU32(in);
  float v;
  memcpy(&v, &bits, sizeof(v));
  return v;
}

bool validRateFields(const Header& header) {
  return header.acquireIntervalMs == runlog::kAcquireIntervalMs &&
         header.logIntervalMs == runlog::kLogIntervalMs &&
         header.maxDurationMs == runlog::kMaxDurationMs;
}

}  // namespace

size_t encodeHeader(uint8_t* out, size_t cap, const Header& header) {
  if (out == nullptr || cap < kHeaderSize) {
    return 0;
  }
  if (header.runId == 0 || !validRateFields(header)) {
    return 0;
  }
  memset(out, 0, kHeaderSize);
  memcpy(out + kHeaderMagicOffset, kMagic, sizeof(kMagic));
  putU16(out + kHeaderVersionOffset, kVersion);
  putU16(out + kHeaderBytesOffset, static_cast<uint16_t>(kHeaderSize));
  putU32(out + kHeaderRunIdOffset, header.runId);
  putU32(out + kHeaderAcquireIntervalMsOffset, header.acquireIntervalMs);
  putU32(out + kHeaderLogIntervalMsOffset, header.logIntervalMs);
  putU32(out + kHeaderMaxDurationMsOffset, header.maxDurationMs);
  // reserved already zero
  putU32(out + kHeaderCrcOffset,
         persistence::crc32(out, kHeaderCrcOffset));
  return kHeaderSize;
}

bool decodeHeader(const uint8_t* in, size_t len, Header& header) {
  if (in == nullptr || len < kHeaderSize) {
    return false;
  }
  if (memcmp(in + kHeaderMagicOffset, kMagic, sizeof(kMagic)) != 0) {
    return false;
  }
  if (getU16(in + kHeaderVersionOffset) != kVersion) {
    return false;
  }
  if (getU16(in + kHeaderBytesOffset) != static_cast<uint16_t>(kHeaderSize)) {
    return false;
  }
  Header parsed{};
  parsed.runId = getU32(in + kHeaderRunIdOffset);
  parsed.acquireIntervalMs = getU32(in + kHeaderAcquireIntervalMsOffset);
  parsed.logIntervalMs = getU32(in + kHeaderLogIntervalMsOffset);
  parsed.maxDurationMs = getU32(in + kHeaderMaxDurationMsOffset);
  if (parsed.runId == 0 || !validRateFields(parsed)) {
    return false;
  }
  if (getU32(in + kHeaderReservedOffset) != 0) {
    return false;
  }
  if (getU32(in + kHeaderCrcOffset) !=
      persistence::crc32(in, kHeaderCrcOffset)) {
    return false;
  }
  header = parsed;
  return true;
}

size_t encodeRecord(uint8_t* out, size_t cap, const LoggedSample& sample) {
  if (out == nullptr || cap < kRecordSize) {
    return 0;
  }
  if (sample.logSeq == 0 || sample.acqSeq == 0) {
    return 0;
  }
  if (sample.ms > runlog::kMaxDurationMs) {
    return 0;
  }
  if ((sample.flags & ~kFlagsMask) != 0) {
    return 0;
  }
  memset(out, 0, kRecordSize);
  putU32(out + kRecordLogSeqOffset, sample.logSeq);
  putU32(out + kRecordAcqSeqOffset, sample.acqSeq);
  putU32(out + kRecordMsOffset, sample.ms);
  putF32(out + kRecordTempCOffset, sample.tempC);
  out[kRecordFaultOffset] = sample.fault;
  out[kRecordIntervalFaultOffset] = sample.intervalFault;
  out[kRecordFlagsOffset] = sample.flags;
  // reserved already zero
  putU32(out + kRecordCrcOffset,
         persistence::crc32(out, kRecordCrcOffset));
  return kRecordSize;
}

bool decodeRecord(const uint8_t* in, size_t len, LoggedSample& sample) {
  if (in == nullptr || len < kRecordSize) {
    return false;
  }
  if (getU32(in + kRecordCrcOffset) !=
      persistence::crc32(in, kRecordCrcOffset)) {
    return false;
  }
  if (in[kRecordReservedOffset] != 0) {
    return false;
  }
  const uint8_t flags = in[kRecordFlagsOffset];
  if ((flags & ~kFlagsMask) != 0) {
    return false;
  }
  LoggedSample parsed{};
  parsed.logSeq = getU32(in + kRecordLogSeqOffset);
  parsed.acqSeq = getU32(in + kRecordAcqSeqOffset);
  parsed.ms = getU32(in + kRecordMsOffset);
  if (parsed.logSeq == 0 || parsed.acqSeq == 0) {
    return false;
  }
  if (parsed.ms > runlog::kMaxDurationMs) {
    return false;
  }
  parsed.tempC = getF32(in + kRecordTempCOffset);
  parsed.fault = in[kRecordFaultOffset];
  parsed.intervalFault = in[kRecordIntervalFaultOffset];
  parsed.flags = flags;
  sample = parsed;
  return true;
}

ScanResult scanRecords(const uint8_t* in, size_t len, ScanCursor& cursor) {
  ScanResult result{};
  if (in == nullptr || cursor.terminalSeen) {
    return result;
  }
  size_t offset = 0;
  while (offset + kRecordSize <= len) {
    LoggedSample record{};
    if (!decodeRecord(in + offset, len - offset, record)) {
      break;
    }
    if (record.logSeq != cursor.nextLogSeq) {
      break;
    }
    if (cursor.hasPrevious &&
        (record.acqSeq <= cursor.lastAcqSeq || record.ms <= cursor.lastMs)) {
      break;
    }
    ++result.validCount;
    result.validBytes = offset + kRecordSize;
    result.lastLogSeq = record.logSeq;
    cursor.nextLogSeq = record.logSeq + 1;
    cursor.lastAcqSeq = record.acqSeq;
    cursor.lastMs = record.ms;
    cursor.hasPrevious = true;
    offset += kRecordSize;
    if ((record.flags & kTerminalFlag) != 0) {
      cursor.terminalSeen = true;
      break;
    }
  }
  return result;
}

}  // namespace rawlogcodec
