#include "ArchiveResponse.h"

#include <string.h>

#include "ArchiveRoutes.h"

ArchiveResponse::ArchiveResponse(ArchiveRoutes& owner, uint8_t slotIndex,
                                 uint32_t generation, int statusCode,
                                 const uint8_t* data, size_t length)
    : owner_(owner),
      slotIndex_(slotIndex),
      generation_(generation),
      data_(data),
      length_(length),
      offset_(0),
      valid_(data != nullptr || length == 0) {
    setCode(statusCode);
    setContentType("application/json");
    setContentLength(length);
    addHeader("Cache-Control", "no-store", true);
}

ArchiveResponse::~ArchiveResponse() {
    owner_.releaseResponseSlot(slotIndex_, generation_);
}

size_t ArchiveResponse::_fillBuffer(uint8_t* buf, size_t maxLen) {
    if (!valid_ || buf == nullptr || offset_ >= length_) return 0;
    size_t remaining = length_ - offset_;
    size_t count = remaining < maxLen ? remaining : maxLen;
    if (count != 0) {
        memcpy(buf, data_ + offset_, count);
        offset_ += count;
    }
    return count;
}
