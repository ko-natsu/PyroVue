#include <math.h>
#include <unity.h>
#include "protocol.h"
#include "RunManager.h"
#include "HistoryStore.h"
#include "SensorService.h"
#include "CoarseHistoryCodec.h"

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
    TEST_ASSERT_EQUAL_UINT32(3, history.coarseCount());
    CoarseSample bucket{};
    TEST_ASSERT_TRUE(history.coarseAt(0, bucket));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 10.5f, bucket.averageC);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 10.0f, bucket.minimumC);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 11.0f, bucket.maximumC);
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

void setup() {
    UNITY_BEGIN();
    RUN_TEST(test_run_rollover_and_lifecycle);
    RUN_TEST(test_history_wrap_and_bucket_aggregation);
    RUN_TEST(test_sensor_service_cadence_and_fault_normalization);
    RUN_TEST(test_persistence_codec_recovers_valid_prefix);
    UNITY_END();
}

void loop() {}
