#ifndef ARCHIVE_RESPONSE_H
#define ARCHIVE_RESPONSE_H

#include <stddef.h>
#include <stdint.h>

#include <ESPAsyncWebServer.h>

class ArchiveRoutes;

// Completed in-RAM response.  The constructor is called only after a job has
// produced its final status code and body.  The slot lease is released from
// this object's destructor; _fillBuffer only copies bytes, it never performs
// filesystem work or waits for a pending result.
class ArchiveResponse : public AsyncAbstractResponse {
public:
    ArchiveResponse(ArchiveRoutes& owner, uint8_t slotIndex,
                    uint32_t generation, int statusCode,
                    const uint8_t* data, size_t length);
    ~ArchiveResponse() override;

    size_t _fillBuffer(uint8_t* buf, size_t maxLen) override;
    bool _sourceValid() const override { return valid_; }

private:
    ArchiveRoutes& owner_;
    uint8_t slotIndex_;
    uint32_t generation_;
    const uint8_t* data_;
    size_t length_;
    size_t offset_;
    bool valid_;
};

#endif  // ARCHIVE_RESPONSE_H
