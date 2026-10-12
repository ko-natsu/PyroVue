#include "EventJournalCodec.h"

#include <string.h>

#include "Crc32.h"  // persistence::crc32: shared IEEE CRC-32

namespace eventjournalcodec {
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

// Strict UTF-8 well-formedness (Unicode "best practice": rejects overlong
// encodings, surrogates U+D800..U+DFFF, code points above U+10FFFF, stray
// continuation bytes and truncated sequences). U+0000 is a valid sequence and
// is accepted; NUL policy belongs to the JSON reducer.
bool isValidUtf8(const uint8_t* data, size_t len) {
  size_t i = 0;
  while (i < len) {
    const uint8_t lead = data[i];
    if (lead < 0x80) {
      ++i;
      continue;
    }
    size_t continuations;
    uint8_t secondLo;
    uint8_t secondHi;
    if (lead >= 0xC2 && lead <= 0xDF) {       // U+0080..U+07FF
      continuations = 1;
      secondLo = 0x80;
      secondHi = 0xBF;
    } else if (lead == 0xE0) {                // U+0800..U+0FFF
      continuations = 2;
      secondLo = 0xA0;
      secondHi = 0xBF;
    } else if (lead >= 0xE1 && lead <= 0xEC) {  // U+1000..U+CFFF, U+E000..U+FFFF
      continuations = 2;
      secondLo = 0x80;
      secondHi = 0xBF;
    } else if (lead == 0xED) {                // U+D000..U+D7FF (no surrogates)
      continuations = 2;
      secondLo = 0x80;
      secondHi = 0x9F;
    } else if (lead >= 0xEE && lead <= 0xEF) {
      continuations = 2;
      secondLo = 0x80;
      secondHi = 0xBF;
    } else if (lead == 0xF0) {                // U+10000..U+3FFFF
      continuations = 3;
      secondLo = 0x90;
      secondHi = 0xBF;
    } else if (lead >= 0xF1 && lead <= 0xF3) {
      continuations = 3;
      secondLo = 0x80;
      secondHi = 0xBF;
    } else if (lead == 0xF4) {                // U+100000..U+10FFFF
      continuations = 3;
      secondLo = 0x80;
      secondHi = 0x8F;
    } else {
      return false;  // 0x80..0xC1 stray/overlong, 0xF5..0xFF invalid
    }
    if (i + continuations >= len) {
      return false;  // truncated sequence
    }
    if (data[i + 1] < secondLo || data[i + 1] > secondHi) {
      return false;  // overlong, surrogate range, or above U+10FFFF
    }
    for (size_t j = 2; j <= continuations; ++j) {
      if ((data[i + j] & 0xC0u) != 0x80u) {
        return false;
      }
    }
    i += continuations + 1;
  }
  return true;
}

}  // namespace

size_t encodeHeader(uint8_t* out, size_t cap, const Header& header) {
  if (out == nullptr || cap < kHeaderSize) {
    return 0;
  }
  if (header.runId == 0) {
    return 0;
  }
  memset(out, 0, kHeaderSize);
  memcpy(out + kHeaderMagicOffset, kMagic, sizeof(kMagic));
  putU16(out + kHeaderVersionOffset, kVersion);
  putU16(out + kHeaderBytesOffset, static_cast<uint16_t>(kHeaderSize));
  putU32(out + kHeaderRunIdOffset, header.runId);
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
  if (parsed.runId == 0) {
    return false;
  }
  if (getU32(in + kHeaderCrcOffset) !=
      persistence::crc32(in, kHeaderCrcOffset)) {
    return false;
  }
  header = parsed;
  return true;
}

size_t encodeFrame(uint8_t* out, size_t cap, const uint8_t* payload,
                   size_t payloadBytes) {
  if (out == nullptr || payload == nullptr) {
    return 0;
  }
  if (payloadBytes < kMinPayloadBytes || payloadBytes > kMaxPayloadBytes) {
    return 0;
  }
  if (!isValidUtf8(payload, payloadBytes)) {
    return 0;
  }
  const size_t frameBytes = kFrameOverheadBytes + payloadBytes;
  if (cap < frameBytes) {
    return 0;
  }
  // Move first: an in-place payload can occupy the future length prefix.
  memmove(out + kFramePayloadOffset, payload, payloadBytes);
  putU32(out + kFrameLengthOffset, static_cast<uint32_t>(payloadBytes));
  putU32(out + kFramePayloadOffset + payloadBytes,
         persistence::crc32(out, kFramePayloadOffset + payloadBytes));
  return frameBytes;
}

bool decodeFrame(const uint8_t* in, size_t len, FrameView& view) {
  if (in == nullptr || len < kFramePayloadOffset) {
    return false;
  }
  const uint32_t payloadBytes = getU32(in + kFrameLengthOffset);
  if (payloadBytes < kMinPayloadBytes || payloadBytes > kMaxPayloadBytes) {
    return false;
  }
  const size_t frameBytes = kFrameOverheadBytes + payloadBytes;
  if (len < frameBytes) {
    return false;  // incomplete frame: uncommitted tail
  }
  if (getU32(in + kFramePayloadOffset + payloadBytes) !=
      persistence::crc32(in, kFramePayloadOffset + payloadBytes)) {
    return false;
  }
  if (!isValidUtf8(in + kFramePayloadOffset, payloadBytes)) {
    return false;
  }
  FrameView parsed{};
  parsed.payload = in + kFramePayloadOffset;
  parsed.payloadBytes = payloadBytes;
  parsed.frameBytes = frameBytes;
  view = parsed;
  return true;
}

}  // namespace eventjournalcodec
