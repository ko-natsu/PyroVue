#include "LittleFsHistoryPersistence.h"

#if defined(ARDUINO)
#include <FS.h>
#include <LittleFS.h>
#include "HistoryStore.h"
#include "CoarseHistoryCodec.h"

namespace {
const char* kPath = "/pyrovue-history.bin";
}

LittleFsHistoryPersistence::LittleFsHistoryPersistence() : mounted(false), activeRun(0) {}

bool LittleFsHistoryPersistence::begin(HistoryStore& history) {
    mounted = LittleFS.begin();
    if (!mounted || !LittleFS.exists(kPath)) return mounted;
    File file = LittleFS.open(kPath, "r");
    if (!file) return mounted;
    uint8_t headerBytes[coarsecodec::kHeaderSize];
    if (file.read(headerBytes, sizeof(headerBytes)) != sizeof(headerBytes)) {
        file.close();
        return mounted;
    }
    coarsecodec::Header header{};
    if (!coarsecodec::decodeHeader(headerBytes, sizeof(headerBytes), header)) {
        file.close();
        LittleFS.remove(kPath);
        return mounted;
    }
    activeRun = header.runId;
    history.clear(activeRun);
    uint8_t recordBytes[coarsecodec::kRecordSize];
    while (file.read(recordBytes, sizeof(recordBytes)) == sizeof(recordBytes)) {
        CoarseSample sample{};
        if (!coarsecodec::decodeRecord(recordBytes, sizeof(recordBytes), sample)) break;
        sample.runId = header.runId;
        history.restoreCoarse(sample);
    }
    return mounted;
}

bool LittleFsHistoryPersistence::startRun(uint32_t runId, uint32_t bucketMs) {
    if (!mounted) return false;
    LittleFS.remove(kPath);
    File file = LittleFS.open(kPath, "w");
    if (!file) return false;
    uint8_t bytes[coarsecodec::kHeaderSize];
    const coarsecodec::Header header{bucketMs, runId};
    const bool ok = coarsecodec::encodeHeader(bytes, sizeof(bytes), header) == sizeof(bytes) &&
                    file.write(bytes, sizeof(bytes)) == sizeof(bytes);
    file.flush();
    file.close();
    if (ok) activeRun = runId;
    return ok;
}

bool LittleFsHistoryPersistence::append(const CoarseSample& sample) {
    if (!mounted || sample.runId != activeRun) return false;
    File file = LittleFS.open(kPath, "a");
    if (!file) return false;
    uint8_t bytes[coarsecodec::kRecordSize];
    const bool ok = coarsecodec::encodeRecord(bytes, sizeof(bytes), sample) == sizeof(bytes) &&
                    file.write(bytes, sizeof(bytes)) == sizeof(bytes);
    file.flush();
    file.close();
    return ok;
}

bool LittleFsHistoryPersistence::flush() { return mounted; }
#endif
