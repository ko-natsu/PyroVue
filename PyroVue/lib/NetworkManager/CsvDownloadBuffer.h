#ifndef CSV_DOWNLOAD_BUFFER_H
#define CSV_DOWNLOAD_BUFFER_H

#include <atomic>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Single producer (loop/file reader), single consumer (AsyncTCP response).
// Publish a whole block only after reading it; callbacks copy RAM, never flash.
class CsvDownloadBuffer {
public:
    static constexpr size_t capacity = 1024;
    uint8_t bytes[capacity]{};
    std::atomic<bool> ready{false};
    std::atomic<bool> finished{false};
    std::atomic<bool> failed{false};
    std::atomic<bool> cancelled{false};

    bool canFill() const { return !ready.load(std::memory_order_acquire); }
    void publish(size_t count) {
        length = count;
        offset = 0;
        ready.store(true, std::memory_order_release);
    }
    size_t consume(uint8_t* out, size_t maximum) {
        if (!ready.load(std::memory_order_acquire)) return 0;
        const size_t remaining = length - offset;
        const size_t count = maximum < remaining ? maximum : remaining;
        memcpy(out, bytes + offset, count);
        offset += count;
        if (offset == length) ready.store(false, std::memory_order_release);
        return count;
    }

private:
    size_t length = 0;
    size_t offset = 0;
};

#endif
