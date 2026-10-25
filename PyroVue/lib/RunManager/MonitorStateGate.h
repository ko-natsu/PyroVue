#ifndef MONITOR_STATE_GATE_H
#define MONITOR_STATE_GATE_H

#include <atomic>
#if defined(ARDUINO_ARCH_ESP8266)
#include <Arduino.h>
#endif

// Serialize run/preset mutations with legacy callback-based history replay.
// Never wait in AsyncTCP, and never disable interrupts across file/NVS work.
class MonitorStateGate {
public:
    bool tryEnter() {
#if defined(ARDUINO_ARCH_ESP8266)
        noInterrupts();
        const bool available = !busy.load(std::memory_order_relaxed);
        if (available) busy.store(true, std::memory_order_release);
        interrupts();
        return available;
#else
        bool expected = false;
        return busy.compare_exchange_strong(expected, true, std::memory_order_acquire,
                                            std::memory_order_relaxed);
#endif
    }
    void leave() { busy.store(false, std::memory_order_release); }

private:
    std::atomic<bool> busy{false};
};

#endif
