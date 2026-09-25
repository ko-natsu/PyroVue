#include "CoarseHistoryCodec.h"

#include <string.h>

namespace coarsecodec {
namespace {

// IEEE CRC-32 (reflected, poly 0xEDB88320), bit-at-a-time: small table-free
// form, adequate for the low persistence cadence of coarse buckets.
uint32_t crc32(const uint8_t* data, size_t len) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
  }
  return crc ^ 0xFFFFFFFFu;
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

}  // namespace

size_t encodeHeader(uint8_t* out, size_t cap, const Header& header) {
  if (cap < kHeaderSize) {
    return 0;
  }
  memset(out, 0, kHeaderSize);
  memcpy(out, kMagic, sizeof(kMagic));
  out[4] = kVersion;
  // bytes 5..7 reserved, already zero
  putU32(out + 8, header.bucketMs);
  putU32(out + 12, header.runId);
  putU32(out + 16, crc32(out, 16));
  return kHeaderSize;
}

bool decodeHeader(const uint8_t* in, size_t len, Header& header) {
  if (len < kHeaderSize) {
    return false;
  }
  if (memcmp(in, kMagic, sizeof(kMagic)) != 0) {
    return false;
  }
  if (in[4] != kVersion) {
    return false;
  }
  if (getU32(in + 16) != crc32(in, 16)) {
    return false;
  }
  header.bucketMs = getU32(in + 8);
  header.runId = getU32(in + 12);
  return true;
}

size_t encodeRecord(uint8_t* out, size_t cap, const CoarseSample& sample) {
  if (cap < kRecordSize) {
    return 0;
  }
  memset(out, 0, kRecordSize);
  putU32(out, sample.ms);
  putF32(out + 4, sample.averageC);
  putF32(out + 8, sample.minimumC);
  putF32(out + 12, sample.maximumC);
  out[16] = sample.fault;
  // bytes 17..19 reserved, already zero
  putU32(out + 20, crc32(out, 20));
  return kRecordSize;
}

bool decodeRecord(const uint8_t* in, size_t len, CoarseSample& sample) {
  if (len < kRecordSize) {
    return false;
  }
  if (getU32(in + 20) != crc32(in, 20)) {
    return false;
  }
  sample.runId = 0;  // runId lives in the header; not repeated per record
  sample.ms = getU32(in);
  sample.averageC = getF32(in + 4);
  sample.minimumC = getF32(in + 8);
  sample.maximumC = getF32(in + 12);
  sample.fault = in[16];
  return true;
}

ScanResult scanRecords(const uint8_t* in, size_t len) {
  ScanResult result;
  result.appendOffset = 0;
  result.validCount = 0;
  size_t offset = 0;
  while (offset + kRecordSize <= len) {
    if (getU32(in + offset + 20) != crc32(in + offset, 20)) {
      break;  // torn/garbage record: keep the valid prefix, stop here
    }
    offset += kRecordSize;
    ++result.validCount;
  }
  result.appendOffset = offset;
  return result;
}

bool scan(const uint8_t* data, size_t len, Header& header, ScanResult& scan) {
  if (!decodeHeader(data, len, header)) {
    return false;
  }
  ScanResult r = scanRecords(data + kHeaderSize, len - kHeaderSize);
  r.appendOffset += kHeaderSize;
  scan = r;
  return true;
}

}  // namespace coarsecodec
