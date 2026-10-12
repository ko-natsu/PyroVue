#ifndef PERSISTENCE_CRC32_H
#define PERSISTENCE_CRC32_H

// Shared IEEE CRC-32 for every on-disk persistence format (coarse history,
// raw run log, event journal). Extracted from CoarseHistoryCodec so old and
// new codecs hash bytes identically.
//
// No Arduino/platform dependencies: only C standard headers. Pure function,
// no tables, no allocation.

#include <stddef.h>
#include <stdint.h>

namespace persistence {

// IEEE CRC-32 (reflected, polynomial 0xEDB88320, init 0xFFFFFFFF, final
// XOR 0xFFFFFFFF). Standard check value: crc32("123456789", 9) == 0xCBF43926.
// `data` may be nullptr only when len == 0.
uint32_t crc32(const uint8_t* data, size_t len);

}  // namespace persistence

#endif  // PERSISTENCE_CRC32_H
