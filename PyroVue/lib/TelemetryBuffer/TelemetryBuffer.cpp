#include "TelemetryBuffer.h"
#include "config.h"

// Note: This is a simple, non-thread-safe ring buffer.
// For this project, the single-core access pattern (loop) is fine.

TempSample buffer[HISTORY_LEN];
size_t head = 0; // Points to the next slot to be written
size_t count = 0; // Number of elements currently in the buffer

void TelemetryBuffer::push(const TempSample& s) {
    buffer[head] = s;
    head = (head + 1) % HISTORY_LEN;
    if (count < HISTORY_LEN) {
        count++;
    }
}

size_t TelemetryBuffer::size() const {
    return count;
}

void TelemetryBuffer::snapshot(size_t num_samples, std::vector<uint32_t>& ms, std::vector<float>& t, std::vector<uint8_t>& fault) const {
    if (num_samples > count) {
        num_samples = count;
    }

    ms.reserve(num_samples);
    t.reserve(num_samples);
    fault.reserve(num_samples);

    size_t start_index = (head + HISTORY_LEN - num_samples) % HISTORY_LEN;

    for (size_t i = 0; i < num_samples; i++) {
        size_t current_index = (start_index + i) % HISTORY_LEN;
        ms.push_back(buffer[current_index].ms);
        t.push_back(buffer[current_index].t_c);
        fault.push_back(buffer[current_index].fault);
    }
}

TempSample TelemetryBuffer::getLastSample() const {
    if (count == 0) {
        return {0, 0.0, 0};
    }
    size_t last_index = (head + HISTORY_LEN - 1) % HISTORY_LEN;
    return buffer[last_index];
}