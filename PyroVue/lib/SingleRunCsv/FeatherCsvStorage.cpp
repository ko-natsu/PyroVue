#include "FeatherCsvStorage.h"

#if defined(PYROVUE_SINGLE_RUN_CSV) && defined(ARDUINO_ARCH_ESP32)

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "FFat.h"

namespace csvrun {

// C++11: constexpr static members used by value still want definitions.
constexpr const char* FeatherCsvStorage::kBasePath;
constexpr const char* FeatherCsvStorage::kPartitionLabel;
constexpr const char* FeatherCsvStorage::kRunPath;

FeatherCsvStorage::FeatherCsvStorage() : writeFd_(-1), readFd_(-1), mounted_(false) {}

bool FeatherCsvStorage::mount() {
    if (mounted_) {
        return true;  // already mounted; never re-format
    }
    // formatOnFail=false: an unmountable partition is NEVER auto-formatted.
    mounted_ = FFat.begin(false, kBasePath, 10, kPartitionLabel);
    return mounted_;
}

bool FeatherCsvStorage::mounted() const {
    return mounted_;
}

bool FeatherCsvStorage::eraseVolume() {
    closeWriter();
    closeReader();
    FFat.end();
    mounted_ = false;
    // FFat.format() requires the volume to be unmounted (checked above) and
    // erases the whole partition's filesystem; quick wipe destroys all files.
    if (!FFat.format()) {
        return false;
    }
    mounted_ = FFat.begin(false, kBasePath, 10, kPartitionLabel);
    return mounted_;
}

uint32_t FeatherCsvStorage::freeBytes() {
    if (!mounted_) {
        return 0;
    }
    return static_cast<uint32_t>(FFat.freeBytes());
}

bool FeatherCsvStorage::fileExists() {
    if (!mounted_) {
        return false;
    }
    struct stat st;
    return ::stat(kRunPath, &st) == 0;
}

bool FeatherCsvStorage::removeFile() {
    if (!mounted_) {
        return false;
    }
    return ::unlink(kRunPath) == 0;
}

bool FeatherCsvStorage::createExclusive() {
    if (!mounted_) {
        return false;
    }
    closeWriter();
    // No-overwrite is enforced twice: an explicit existence check (all calls
    // are loop-task serial, so no TOCTOU) and O_EXCL at the VFS level.
    if (fileExists()) {
        return false;
    }
    writeFd_ = ::open(kRunPath, O_CREAT | O_EXCL | O_WRONLY, 0644);
    return writeFd_ >= 0;
}

bool FeatherCsvStorage::writeAll(const uint8_t* data, size_t len) {
    if (writeFd_ < 0 || data == nullptr) {
        return false;
    }
    size_t written = 0;
    while (written < len) {
        const ssize_t n = ::write(writeFd_, data + written, len - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        written += static_cast<size_t>(n);
    }
    return true;
}

bool FeatherCsvStorage::sync() {
    return writeFd_ >= 0 && ::fsync(writeFd_) == 0;
}

void FeatherCsvStorage::closeWriter() {
    if (writeFd_ >= 0) {
        ::close(writeFd_);
        writeFd_ = -1;
    }
}

bool FeatherCsvStorage::openReader() {
    if (!mounted_) {
        return false;
    }
    closeReader();
    readFd_ = ::open(kRunPath, O_RDONLY);
    return readFd_ >= 0;
}

int32_t FeatherCsvStorage::read(uint8_t* dst, size_t cap) {
    if (readFd_ < 0 || dst == nullptr) {
        return -1;
    }
    if (cap == 0) {
        return 0;
    }
    if (cap > 0x7FFFFFFFu) {
        cap = 0x7FFFFFFFu;
    }
    for (;;) {
        const ssize_t n = ::read(readFd_, dst, cap);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        return static_cast<int32_t>(n);
    }
}

void FeatherCsvStorage::closeReader() {
    if (readFd_ >= 0) {
        ::close(readFd_);
        readFd_ = -1;
    }
}

}  // namespace csvrun

#endif  // PYROVUE_SINGLE_RUN_CSV && ARDUINO_ARCH_ESP32
