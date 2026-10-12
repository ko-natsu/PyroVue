#ifndef EVENT_JOURNAL_CODEC_H
#define EVENT_JOURNAL_CODEC_H

// Portable framing codec for the durable event journal (events.bin):
// lifecycle and marker mutation entries as UTF-8 JSON payloads.
//
// No Arduino/platform dependencies: only C standard headers plus the shared
// runlog types. All encoding happens into caller-provided byte buffers; the
// codec never allocates. decodeFrame BORROWS the input buffer (no copying);
// the FrameView is valid only while that buffer lives. Filesystem and JSON
// semantics are intentionally NOT here.
//
// On-disk file layout:
//
//   +----------------------+
//   | header (16 bytes)    |
//   +----------------------+
//   | frame 0              |
//   | frame 1              |
//   | ...                  |  appended as revisions occur, one JSON value each
//   +----------------------+
//
// All multi-byte fields are LITTLE-ENDIAN, explicit byte-wise serialization
// (no struct casts, no alignment/packing assumptions across platforms).
//
// Header (16 bytes):
//   offset  0 : magic[4]          = 'P','V','E','1'
//   offset  4 : version (u16)     = 1 (eventjournalcodec::kVersion)
//   offset  6 : headerBytes (u16) = 16 (eventjournalcodec::kHeaderSize)
//   offset  8 : runId (u32)       = run this journal belongs to (> 0)
//   offset 12 : crc32 (u32)       = persistence::crc32 over header bytes 0..11
//
// Frame:
//   offset  0 : payloadLength (u32) = 1..4096 (runlog::kMaxEventPayloadBytes)
//   offset  4 : payload (bytes)     = UTF-8 encoded JSON value
//   offset 4+n : crc32 (u32)        = persistence::crc32 over the length field
//                                     plus payload (bytes 0..3+n)
//
// Strictness. decodeHeader rejects wrong magic, version, or headerBytes, a
// zero runId, and a bad CRC. decodeFrame rejects a missing/incomplete frame,
// a payloadLength outside 1..4096, a bad CRC, and any payload that is not
// valid UTF-8 (overlong encodings, surrogates, code points above U+10FFFF,
// stray continuation bytes and truncated sequences are all invalid). Every
// decoder leaves its output untouched on failure.
//
// UTF-8 framing only. U+0000 is a valid UTF-8 sequence and is NOT rejected
// here; embedded-NUL rejection and all exact event semantics (kinds, fields,
// consecutive revisions) belong to the JSON reducer over explicit string
// lengths, not to this framing codec. This codec deliberately implements NO
// JSON parsing.
//
// Recovery model: frames are appended back-to-back after the header. Scanning
// stops at the first invalid/incomplete frame; everything before it is the
// valid prefix. decodeFrame returns the same failure for a truncated frame
// and a corrupt one: callers that know their buffer ends at end-of-file treat
// a failed frame as an uncommitted tail, streaming callers that still have
// bytes coming treat it as corruption and stop.

#include <stddef.h>
#include <stdint.h>

#include "RunLogTypes.h"  // runlog::kMaxEventPayloadBytes

namespace eventjournalcodec {

constexpr uint8_t kMagic[4] = {'P', 'V', 'E', '1'};
constexpr uint16_t kVersion = 1;

constexpr size_t kHeaderSize = 16;

// Header field offsets (exposed for fixtures; keep in sync with the layout).
constexpr size_t kHeaderMagicOffset = 0;
constexpr size_t kHeaderVersionOffset = 4;
constexpr size_t kHeaderBytesOffset = 6;
constexpr size_t kHeaderRunIdOffset = 8;
constexpr size_t kHeaderCrcOffset = 12;

// Frame field offsets and sizes (exposed for fixtures).
constexpr size_t kFrameLengthOffset = 0;
constexpr size_t kFramePayloadOffset = 4;
constexpr size_t kFrameOverheadBytes = 8;  // u32 length + u32 crc
constexpr size_t kMinPayloadBytes = 1;
constexpr size_t kMaxPayloadBytes = runlog::kMaxEventPayloadBytes;  // 4096
constexpr size_t kMaxFrameBytes = kFrameOverheadBytes + kMaxPayloadBytes;

// Header carries the one data field of the binary layout; magic, version,
// headerBytes and CRC are fixed format values handled internally.
struct Header {
  uint32_t runId{};
};

// Borrowed view of one decoded frame; payload points INTO the input buffer.
struct FrameView {
  const uint8_t* payload{};
  size_t payloadBytes{};
  size_t frameBytes{};  // total bytes the frame occupies in the input
};

// Serializes the header. Returns kHeaderSize on success, 0 on insufficient
// capacity or invalid input (null out, runId == 0); out is left untouched
// then.
size_t encodeHeader(uint8_t* out, size_t cap, const Header& header);

// Parses and validates a header: magic, version, headerBytes, runId, CRC.
// Returns true and fills `header` on success; on failure `header` is left
// untouched.
bool decodeHeader(const uint8_t* in, size_t len, Header& header);

// Serializes one frame (length + payload + CRC). Returns frameBytes
// (kFrameOverheadBytes + payloadBytes) on success, 0 on insufficient capacity
// or invalid input (null pointers, payloadBytes outside kMinPayloadBytes..
// kMaxPayloadBytes, payload that is not valid UTF-8); out is left untouched
// then. The payload may point into `out` (copying tolerates overlap); on
// return the payload bytes have been consumed into the frame.
size_t encodeFrame(uint8_t* out, size_t cap, const uint8_t* payload,
                   size_t payloadBytes);

// Parses and validates the first frame of `in` and borrows its payload in
// `view`. Requires only that `len` cover the decoded frame (frameBytes);
// callers advance by view.frameBytes to reach the next frame. Returns true on
// success; on failure `view` is left untouched and the caller must treat the
// bytes as an uncommitted tail (incomplete) or corruption (full frame
// available but invalid).
bool decodeFrame(const uint8_t* in, size_t len, FrameView& view);

}  // namespace eventjournalcodec

#endif  // EVENT_JOURNAL_CODEC_H
