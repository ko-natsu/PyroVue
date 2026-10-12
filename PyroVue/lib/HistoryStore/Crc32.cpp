#include "Crc32.h"

namespace persistence {

// IEEE CRC-32 (reflected, poly 0xEDB88320), bit-at-a-time: small table-free
// form, adequate for the low persistence cadence of history/journal writes.
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

}  // namespace persistence
