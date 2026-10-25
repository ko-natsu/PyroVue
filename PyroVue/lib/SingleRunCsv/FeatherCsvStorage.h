#ifndef FEATHER_CSV_STORAGE_H
#define FEATHER_CSV_STORAGE_H

// Concrete IStorage backend for the ESP32 Feather: the EXISTING "ffat"
// partition, one application-owned run file at "/ffat/pyrovue-run.csv".
//
// Compiled only when the single-run CSV feature is enabled on an ESP32 Arduino
// target (PYROVUE_SINGLE_RUN_CSV && ARDUINO_ARCH_ESP32). Other targets see an
// empty header, so native tests, ESP8266 and the N16R8 envs never acquire a
// second FFat owner and the portable recorder core still compiles everywhere.
//
// Mounting NEVER auto-formats: FFat.begin(false, "/ffat", 10, "ffat") with
// formatOnFail=false, so a missing/corrupt filesystem simply fails to mount
// and the recorder reports error "mount_failed". The single destructive path
// is eraseVolume(), reachable only through SingleRunCsv::format() behind an
// explicit user confirmation: it unmounts, formats the whole partition and
// remounts it empty.
//
// All file operations use the checked POSIX VFS calls (open/write/fsync/read/
// unlink) - not the void File::flush - so every durable boundary is a real
// fsync with a checked result. No PSRAM, no dynamic allocation.

#if defined(PYROVUE_SINGLE_RUN_CSV) && defined(ARDUINO_ARCH_ESP32)

#include "SingleRunCsv.h"

namespace csvrun {

class FeatherCsvStorage : public IStorage {
public:
    FeatherCsvStorage();

    static constexpr const char* kBasePath = "/ffat";
    static constexpr const char* kPartitionLabel = "ffat";
    static constexpr const char* kRunPath = "/ffat/pyrovue-run.csv";

    // IStorage
    bool mount() override;
    bool mounted() const override;
    bool eraseVolume() override;
    uint32_t freeBytes() override;
    bool fileExists() override;
    bool removeFile() override;
    bool createExclusive() override;
    bool writeAll(const uint8_t* data, size_t len) override;
    bool sync() override;
    void closeWriter() override;
    bool openReader() override;
    int32_t read(uint8_t* dst, size_t cap) override;
    void closeReader() override;

private:
    int writeFd_;
    int readFd_;
    bool mounted_;
};

}  // namespace csvrun

#endif  // PYROVUE_SINGLE_RUN_CSV && ARDUINO_ARCH_ESP32
#endif  // FEATHER_CSV_STORAGE_H
