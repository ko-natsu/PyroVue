#include <math.h>
#include <unity.h>
#include "protocol.h"
#include "RunManager.h"
#include "HistoryStore.h"
#include "SensorService.h"
#include "CoarseHistoryCodec.h"
#include <string.h>
#include "Crc32.h"
#include "RawLogCodec.h"
#include "EventJournalCodec.h"

// Each fixture owns its state; Unity still requires the per-test hook symbols.
void setUp() {}
void tearDown() {}

struct MemoryRunStore : IRunIdStore {
    uint32_t value = 0;
    int writes = 0;
    bool loadRunId(uint32_t& out) override { out = value; return value != 0; }
    bool saveRunId(uint32_t id) override { value = id; ++writes; return true; }
};

struct FakeReader : ISensorReader {
    RawReading next{0, 0};
    bool begin() override { return true; }
    bool read(RawReading& out) override { out = next; return true; }
};

struct FakeSink : ISampleSink {
    TempSample last{};
    int count = 0;
    void onSample(const TempSample& sample) override { last = sample; ++count; }
};

static void test_run_rollover_and_lifecycle() {
    MemoryRunStore store;
    RunManager run(&store);
    run.begin(0);
    TEST_ASSERT_EQUAL(RunManager::STOP_NOT_ACTIVE, run.stopRun());
    TEST_ASSERT_EQUAL(RunManager::START_STARTED, run.startRun(0xfffffff0UL));
    TEST_ASSERT_EQUAL_UINT32(1, run.runId());
    TEST_ASSERT_EQUAL_UINT32(1, run.nextSequence());
    TEST_ASSERT_EQUAL_UINT32(32, run.elapsedMs(0x10));
    TEST_ASSERT_EQUAL(RunManager::START_ALREADY_ACTIVE, run.startRun(10));
    TEST_ASSERT_EQUAL(RunManager::STOP_OK, run.stopRun());
    TEST_ASSERT_EQUAL_UINT32(32, run.elapsedMs(99));
    TEST_ASSERT_EQUAL(1, store.writes);
}

static void test_history_wrap_and_bucket_aggregation() {
    TempSample raw[3];
    CoarseSample coarse[4];
    HistoryStore history(raw, 3, coarse, 4, 1000);
    for (uint32_t i = 0; i < 5; ++i) {
        history.push({7, i + 1, i * 500, 10.0f + i, 0});
    }
    TEST_ASSERT_EQUAL_UINT32(3, history.rawCount());
    TempSample sample{};
    TEST_ASSERT_TRUE(history.rawAt(0, sample));
    TEST_ASSERT_EQUAL_UINT32(3, sample.seq);
    TEST_ASSERT_EQUAL_UINT32(5, history.latestRawSequence());
    TEST_ASSERT_EQUAL_UINT32(3, history.oldestRawSequence());
    CoarseSample bucket{};
    TEST_ASSERT_TRUE(history.coarseAt(0, bucket));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 10.5f, bucket.averageC);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 10.0f, bucket.minimumC);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 11.0f, bucket.maximumC);
}

static void test_idle_samples_update_latest_without_history() {
    TempSample raw[3];
    CoarseSample coarse[4];
    HistoryStore history(raw, 3, coarse, 4, 1000);
    TempSample latest{};
    TEST_ASSERT_FALSE(history.latestReading(latest));

    history.push({0, 0, 0, 20.5f, 0});
    TEST_ASSERT_TRUE(history.latestReading(latest));
    TEST_ASSERT_EQUAL_UINT32(0, latest.runId);
    TEST_ASSERT_EQUAL_UINT32(0, latest.seq);
    TEST_ASSERT_EQUAL_UINT32(0, latest.ms);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 20.5f, latest.tempC);
    TEST_ASSERT_EQUAL_UINT32(0, history.rawCount());
    TEST_ASSERT_EQUAL_UINT32(0, history.coarseCount());

    history.push({0, 0, 0, NAN, SENSOR_FAULT_NAN});
    TEST_ASSERT_TRUE(history.latestReading(latest));
    TEST_ASSERT_EQUAL_UINT32(0, latest.ms);
    TEST_ASSERT_TRUE(isnan(latest.tempC));
    TEST_ASSERT_EQUAL_UINT32(SENSOR_FAULT_NAN, latest.fault);
    TEST_ASSERT_EQUAL_UINT32(0, history.rawCount());
    TEST_ASSERT_EQUAL_UINT32(0, history.coarseCount());
}

static void test_sensor_service_cadence_and_fault_normalization() {
    MemoryRunStore store;
    RunManager run(&store);
    run.begin(0);
    run.startRun(0);
    FakeReader reader;
    FakeSink sink;
    SensorService service(reader, run, sink, 2);
    TEST_ASSERT_TRUE(service.begin());
    reader.next = {NAN, 0};
    TEST_ASSERT_FALSE(service.tick(100));
    TEST_ASSERT_TRUE(service.tick(500));
    TEST_ASSERT_EQUAL(1, sink.count);
    TEST_ASSERT_EQUAL_UINT32(1, sink.last.seq);
    TEST_ASSERT_TRUE((sink.last.fault & SENSOR_FAULT_NAN) != 0);
}

static void test_persistence_codec_recovers_valid_prefix() {
    uint8_t bytes[coarsecodec::kHeaderSize + 2 * coarsecodec::kRecordSize] = {};
    const coarsecodec::Header header{30000, 9};
    TEST_ASSERT_EQUAL_UINT32(coarsecodec::kHeaderSize,
                             coarsecodec::encodeHeader(bytes, sizeof(bytes), header));
    CoarseSample value{9, 30000, 12.5f, 11.0f, 14.0f, SENSOR_FAULT_OPEN};
    TEST_ASSERT_EQUAL_UINT32(coarsecodec::kRecordSize,
                             coarsecodec::encodeRecord(bytes + coarsecodec::kHeaderSize,
                                                       sizeof(bytes) - coarsecodec::kHeaderSize, value));
    TEST_ASSERT_EQUAL_UINT32(coarsecodec::kRecordSize,
                             coarsecodec::encodeRecord(bytes + coarsecodec::kHeaderSize + coarsecodec::kRecordSize,
                                                       coarsecodec::kRecordSize, value));
    bytes[coarsecodec::kHeaderSize + coarsecodec::kRecordSize + 2] ^= 0x40;
    coarsecodec::Header decoded{};
    coarsecodec::ScanResult scan{};
    TEST_ASSERT_TRUE(coarsecodec::scan(bytes, sizeof(bytes), decoded, scan));
    TEST_ASSERT_EQUAL_UINT32(1, scan.validCount);
    TEST_ASSERT_EQUAL_UINT32(coarsecodec::kHeaderSize + coarsecodec::kRecordSize, scan.appendOffset);
}
static void refreshCrc(uint8_t* bytes, size_t prefixBytes) {
    const uint32_t crc = persistence::crc32(bytes, prefixBytes);
    for (size_t i = 0; i < 4; ++i) bytes[prefixBytes + i] = static_cast<uint8_t>(crc >> (8 * i));
}

static void test_raw_prefix_preserves_values_and_truncated_tail() {
    uint8_t bytes[3 * rawlogcodec::kRecordSize]{};
    const LoggedSample samples[] = {
        {1, 1, 1000, -0.0f, 0, 0, 0},
        {2, 3, 3000, 123.25f, 0, SENSOR_FAULT_OPEN, runlog::kAcquisitionGap},
        {3, 5, 5000, NAN, SENSOR_FAULT_NAN, SENSOR_FAULT_NAN, runlog::kTerminal}
    };
    for (size_t i = 0; i < 3; ++i) {
        TEST_ASSERT_EQUAL_UINT32(rawlogcodec::kRecordSize,
            rawlogcodec::encodeRecord(bytes + i * rawlogcodec::kRecordSize, rawlogcodec::kRecordSize, samples[i]));
    }
    rawlogcodec::ScanCursor cursor{};
    auto prefix = rawlogcodec::scanRecords(bytes, sizeof(bytes) - 1, cursor);
    TEST_ASSERT_EQUAL_UINT32(2, prefix.validCount);
    TEST_ASSERT_EQUAL_UINT32(48, prefix.validBytes);
    TEST_ASSERT_EQUAL_UINT32(3, cursor.nextLogSeq);
    LoggedSample decoded{};
    TEST_ASSERT_TRUE(rawlogcodec::decodeRecord(bytes, 24, decoded));
    TEST_ASSERT_TRUE(signbit(decoded.tempC));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, decoded.tempC);
    TEST_ASSERT_TRUE(rawlogcodec::decodeRecord(bytes + 24, 24, decoded));
    TEST_ASSERT_EQUAL_FLOAT(123.25f, decoded.tempC);
    TEST_ASSERT_EQUAL_UINT8(SENSOR_FAULT_OPEN, decoded.intervalFault);
    TEST_ASSERT_EQUAL_UINT8(runlog::kAcquisitionGap, decoded.flags);
    auto tail = rawlogcodec::scanRecords(bytes + 48, 24, cursor);
    TEST_ASSERT_EQUAL_UINT32(1, tail.validCount);
    TEST_ASSERT_TRUE(cursor.terminalSeen);
    TEST_ASSERT_TRUE(rawlogcodec::decodeRecord(bytes + 48, 24, decoded));
    TEST_ASSERT_TRUE(isnan(decoded.tempC));
    TEST_ASSERT_EQUAL_UINT8(SENSOR_FAULT_NAN, decoded.fault);
}

static void test_raw_scan_rejects_bad_sequence_crc_and_cross_page_order() {
    uint8_t bytes[48]{};
    LoggedSample first{1, 10, 1000, 11.5f, 0, 0, 0};
    LoggedSample second{3, 12, 3000, 22.5f, 0, 0, 0};
    rawlogcodec::encodeRecord(bytes, 24, first);
    rawlogcodec::encodeRecord(bytes + 24, 24, second);
    rawlogcodec::ScanCursor cursor{};
    TEST_ASSERT_EQUAL_UINT32(1, rawlogcodec::scanRecords(bytes, sizeof(bytes), cursor).validCount);
    TEST_ASSERT_EQUAL_UINT32(2, cursor.nextLogSeq);
    second.logSeq = 2;
    second.acqSeq = first.acqSeq;
    rawlogcodec::encodeRecord(bytes + 24, 24, second);
    TEST_ASSERT_EQUAL_UINT32(0, rawlogcodec::scanRecords(bytes + 24, 24, cursor).validCount);
    second.acqSeq = 12;
    second.ms = first.ms;
    rawlogcodec::encodeRecord(bytes + 24, 24, second);
    TEST_ASSERT_EQUAL_UINT32(0, rawlogcodec::scanRecords(bytes + 24, 24, cursor).validCount);
    second.ms = 3000;
    rawlogcodec::encodeRecord(bytes + 24, 24, second);
    bytes[24 + 12] ^= 1;
    TEST_ASSERT_EQUAL_UINT32(0, rawlogcodec::scanRecords(bytes + 24, 24, cursor).validCount);
    TEST_ASSERT_EQUAL_UINT32(10, cursor.lastAcqSeq);
    TEST_ASSERT_EQUAL_UINT32(1000, cursor.lastMs);
}

static void test_raw_terminal_and_record_boundaries() {
    uint8_t bytes[48]{};
    LoggedSample terminal{1, 1, runlog::kMaxDurationMs, 900.0f, 0, 0, runlog::kTerminal};
    rawlogcodec::encodeRecord(bytes, 24, terminal);
    LoggedSample later{2, 2, runlog::kMaxDurationMs, 901.0f, 0, 0, 0};
    rawlogcodec::encodeRecord(bytes + 24, 24, later);
    rawlogcodec::ScanCursor cursor{};
    TEST_ASSERT_EQUAL_UINT32(1, rawlogcodec::scanRecords(bytes, 48, cursor).validCount);
    TEST_ASSERT_EQUAL_UINT32(0, rawlogcodec::scanRecords(bytes + 24, 24, cursor).validCount);
    ++terminal.ms;
    TEST_ASSERT_EQUAL_UINT32(0, rawlogcodec::encodeRecord(bytes, 24, terminal));
    terminal.ms = 10;
    terminal.flags = 4;
    TEST_ASSERT_EQUAL_UINT32(0, rawlogcodec::encodeRecord(bytes, 24, terminal));
    bytes[19] = 1;
    refreshCrc(bytes, 20);
    LoggedSample unchanged{99, 99, 99, 99.0f, 0, 0, 0};
    TEST_ASSERT_FALSE(rawlogcodec::decodeRecord(bytes, 24, unchanged));
    TEST_ASSERT_EQUAL_UINT32(99, unchanged.logSeq);
}

static void test_headers_reject_unsupported_or_corrupt_formats() {
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926UL,
        persistence::crc32(reinterpret_cast<const uint8_t*>("123456789"), 9));
    uint8_t raw[32]{};
    rawlogcodec::Header header{9};
    TEST_ASSERT_EQUAL_UINT32(32, rawlogcodec::encodeHeader(raw, sizeof(raw), header));
    rawlogcodec::Header parsed{};
    TEST_ASSERT_TRUE(rawlogcodec::decodeHeader(raw, sizeof(raw), parsed));
    TEST_ASSERT_EQUAL_UINT32(9, parsed.runId);
    TEST_ASSERT_EQUAL_UINT32(2000, parsed.logIntervalMs);
    TEST_ASSERT_FALSE(rawlogcodec::decodeHeader(raw, 31, parsed));
    raw[24] = 1;
    refreshCrc(raw, 28);
    TEST_ASSERT_FALSE(rawlogcodec::decodeHeader(raw, 32, parsed));
    rawlogcodec::encodeHeader(raw, sizeof(raw), header);
    raw[4] = 2;
    refreshCrc(raw, 28);
    TEST_ASSERT_FALSE(rawlogcodec::decodeHeader(raw, 32, parsed));
    header.logIntervalMs = 5000;
    TEST_ASSERT_EQUAL_UINT32(0, rawlogcodec::encodeHeader(raw, sizeof(raw), header));
    uint8_t journal[16]{};
    TEST_ASSERT_EQUAL_UINT32(16, eventjournalcodec::encodeHeader(journal, sizeof(journal), {9}));
    eventjournalcodec::Header journalHeader{};
    TEST_ASSERT_TRUE(eventjournalcodec::decodeHeader(journal, 16, journalHeader));
    TEST_ASSERT_EQUAL_UINT32(9, journalHeader.runId);
    journal[0] ^= 1;
    TEST_ASSERT_FALSE(eventjournalcodec::decodeHeader(journal, 16, journalHeader));
}

static void test_journal_preserves_unicode_and_checks_bounds_crc() {
    const uint8_t payload[] = "{\"note\":\"smoke,\\n\\\"stable\\\" \xF0\x9F\x94\xA5\"}";
    uint8_t frame[128]{};
    const size_t size = eventjournalcodec::encodeFrame(frame, sizeof(frame), payload, sizeof(payload) - 1);
    TEST_ASSERT_EQUAL_UINT32(sizeof(payload) - 1 + 8, size);
    eventjournalcodec::FrameView view{};
    TEST_ASSERT_TRUE(eventjournalcodec::decodeFrame(frame, size, view));
    TEST_ASSERT_EQUAL_MEMORY(payload, view.payload, sizeof(payload) - 1);
    TEST_ASSERT_EQUAL_PTR(frame + 4, view.payload);
    TEST_ASSERT_FALSE(eventjournalcodec::decodeFrame(frame, size - 1, view));
    frame[4] ^= 1;
    TEST_ASSERT_FALSE(eventjournalcodec::decodeFrame(frame, size, view));
    uint8_t oversized[4] = {1, 16, 0, 0}; // 4097 bytes, rejected before reading payload.
    TEST_ASSERT_FALSE(eventjournalcodec::decodeFrame(oversized, sizeof(oversized), view));
}

static void test_journal_utf8_and_maximum_frame_boundary() {
    uint8_t frame[eventjournalcodec::kMaxFrameBytes]{};
    uint8_t payload[eventjournalcodec::kMaxPayloadBytes + 1];
    memset(payload, 'x', sizeof(payload));
    TEST_ASSERT_EQUAL_UINT32(4104, eventjournalcodec::encodeFrame(frame, sizeof(frame), payload, 4096));
    eventjournalcodec::FrameView view{};
    TEST_ASSERT_TRUE(eventjournalcodec::decodeFrame(frame, sizeof(frame), view));
    TEST_ASSERT_EQUAL_UINT32(4096, view.payloadBytes);
    TEST_ASSERT_EQUAL_UINT32(0, eventjournalcodec::encodeFrame(frame, sizeof(frame), payload, 4097));
    const uint8_t invalid[][4] = {{0xC0, 0xAF, 0, 0}, {0xED, 0xA0, 0x80, 0}, {0xF4, 0x90, 0x80, 0x80}};
    const size_t lengths[] = {2, 3, 4};
    for (size_t i = 0; i < 3; ++i) {
        TEST_ASSERT_EQUAL_UINT32(0, eventjournalcodec::encodeFrame(frame, sizeof(frame), invalid[i], lengths[i]));
    }
    // A valid CRC must not admit malformed UTF-8 from an external journal.
    frame[0] = 2; frame[1] = 0; frame[2] = 0; frame[3] = 0;
    frame[4] = 0xC0; frame[5] = 0xAF;
    refreshCrc(frame, 6);
    TEST_ASSERT_FALSE(eventjournalcodec::decodeFrame(frame, 10, view));
}

static void test_journal_in_place_encoding_preserves_payload() {
    uint8_t frame[64] = "{\"note\":\"pressure +0.1\"}";
    const uint8_t original[] = "{\"note\":\"pressure +0.1\"}";
    const size_t size = eventjournalcodec::encodeFrame(frame, sizeof(frame), frame, sizeof(original) - 1);
    eventjournalcodec::FrameView view{};
    TEST_ASSERT_TRUE(eventjournalcodec::decodeFrame(frame, size, view));
    TEST_ASSERT_EQUAL_MEMORY(original, view.payload, sizeof(original) - 1);
}

void setup() {
    UNITY_BEGIN();
    RUN_TEST(test_run_rollover_and_lifecycle);
    RUN_TEST(test_history_wrap_and_bucket_aggregation);
    RUN_TEST(test_idle_samples_update_latest_without_history);
    RUN_TEST(test_sensor_service_cadence_and_fault_normalization);
    RUN_TEST(test_persistence_codec_recovers_valid_prefix);
    RUN_TEST(test_raw_prefix_preserves_values_and_truncated_tail);
    RUN_TEST(test_raw_scan_rejects_bad_sequence_crc_and_cross_page_order);
    RUN_TEST(test_raw_terminal_and_record_boundaries);
    RUN_TEST(test_headers_reject_unsupported_or_corrupt_formats);
    RUN_TEST(test_journal_preserves_unicode_and_checks_bounds_crc);
    RUN_TEST(test_journal_utf8_and_maximum_frame_boundary);
    RUN_TEST(test_journal_in_place_encoding_preserves_payload);
    UNITY_END();
}

void loop() {}

#if defined(PYROVUE_NATIVE)
int main() {
    setup();
    return Unity.TestFailures == 0 ? 0 : 1;
}
#endif
