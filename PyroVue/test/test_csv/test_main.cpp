// Behavioral tests for the single-run CSV recorder (lib/SingleRunCsv).
//
// The tests run the REAL recorder core against a byte-accurate in-memory
// IStorage with fault injection (short writes, failed fsync, read errors,
// space exhaustion), so the on-disk format, the 3s selection rules, fault/gap
// accumulation, terminal/duration endings, boot recovery of the validated
// prefix and the no-overwrite/busy guards are all exercised end to end
// without Arduino.
//
// Run: pio test -e native -f test_csv   (from PyroVue/)

#include <math.h>
#include <string.h>
#include <unity.h>

#include <algorithm>
#include <string>
#include <vector>
#include <sstream>
#include <stdlib.h>

#include "SingleRunCsv.h"
#include "protocol.h"

using namespace csvrun;

void setUp() {}
void tearDown() {}

namespace {

// Real byte-storage semantics: exclusive create (no overwrite), checked
// appends (possibly short), checked sync, sequential bounded reads, plus
// injectable failures for the meaningful failure modes.
struct TestStorage : IStorage {
    bool mountOk = true;
    bool mountedFlag = false;
    bool filePresent = false;
    std::vector<uint8_t> file;
    uint32_t free = 8u * 1024 * 1024;

    // Fault injection.
    long writeBudget = -1;  // writable bytes before writes start failing
    bool syncOk = true;
    bool removeOk = true;
    bool eraseOk = true;
    bool failReads = false;
    long readBudget = -1;  // readable bytes before read returns an error

    size_t readPos = 0;
    bool writerOpen = false;
    bool readerOpen = false;

    bool mount() override {
        mountedFlag = mountOk;
        return mountOk;
    }
    bool mounted() const override { return mountedFlag; }
    bool eraseVolume() override {
        writerOpen = false;
        readerOpen = false;
        if (!eraseOk) return false;
        file.clear();
        filePresent = false;
        mountedFlag = mountOk;
        free = 8u * 1024 * 1024;
        return mountedFlag;
    }
    uint32_t freeBytes() override { return free; }
    bool fileExists() override { return filePresent; }
    bool removeFile() override {
        if (!removeOk || !filePresent) return false;
        file.clear();
        filePresent = false;
        return true;
    }
    bool createExclusive() override {
        if (writerOpen || filePresent || !mountedFlag) return false;
        writerOpen = true;
        filePresent = true;
        file.clear();
        return true;
    }
    bool writeAll(const uint8_t* data, size_t len) override {
        if (!writerOpen || data == nullptr) return false;
        size_t accept = len;
        if (writeBudget >= 0) {
            const size_t room = static_cast<size_t>(writeBudget);
            if (accept > room) accept = room;
            writeBudget -= static_cast<long>(accept);
        }
        file.insert(file.end(), data, data + accept);
        return accept == len;  // short write is a checked failure
    }
    bool sync() override { return writerOpen && syncOk; }
    void closeWriter() override { writerOpen = false; }
    bool openReader() override {
        if (readerOpen || !filePresent || !mountedFlag) return false;
        readerOpen = true;
        readPos = 0;
        return true;
    }
    int32_t read(uint8_t* dst, size_t cap) override {
        if (!readerOpen || dst == nullptr) return -1;
        if (readPos >= file.size()) return 0;
        if (failReads) return -1;
        size_t n = std::min(cap, file.size() - readPos);
        if (readBudget >= 0) {
            const size_t room = static_cast<size_t>(readBudget);
            if (n > room) n = room;
            readBudget -= static_cast<long>(n);
            if (n == 0) return -1;
        }
        memcpy(dst, file.data() + readPos, n);
        readPos += n;
        return static_cast<int32_t>(n);
    }
    void closeReader() override { readerOpen = false; }

    std::string text() const { return std::string(file.begin(), file.end()); }
};

TempSample sample(uint32_t runId, uint32_t seq, uint32_t ms, float tempC, uint8_t fault) {
    TempSample s;
    s.runId = runId;
    s.seq = seq;
    s.ms = ms;
    s.tempC = tempC;
    s.fault = fault;
    return s;
}

struct ParsedRow {
    uint32_t runId, logSeq, acqSeq, ms;
    float temp;
    uint32_t fault, intervalFault, flags;
};

std::vector<ParsedRow> dataRows(const std::string& text) {
    std::istringstream input(text);
    std::string line;
    std::vector<ParsedRow> result;
    while (std::getline(input, line)) {
        if (line.empty() || line[0] < '0' || line[0] > '9') continue;
        std::istringstream columns(line);
        std::string field[8];
        for (auto& value : field) TEST_ASSERT_TRUE(static_cast<bool>(std::getline(columns, value, ',')));
        std::string extra;
        TEST_ASSERT_FALSE(static_cast<bool>(std::getline(columns, extra, ',')));
        ParsedRow row;
        row.runId = strtoul(field[0].c_str(), nullptr, 10);
        row.logSeq = strtoul(field[1].c_str(), nullptr, 10);
        row.acqSeq = strtoul(field[2].c_str(), nullptr, 10);
        row.ms = strtoul(field[3].c_str(), nullptr, 10);
        row.temp = field[4].empty() ? NAN : strtof(field[4].c_str(), nullptr);
        row.fault = strtoul(field[5].c_str(), nullptr, 10);
        row.intervalFault = strtoul(field[6].c_str(), nullptr, 10);
        row.flags = strtoul(field[7].c_str(), nullptr, 10);
        result.push_back(row);
    }
    return result;
}

}  // namespace

// ---------------------------------------------------------------- format ---

static void test_selection_preserves_interval_faults() {
    TestStorage storage;
    SingleRunCsv csv(storage);
    TEST_ASSERT_TRUE(csv.begin());
    TEST_ASSERT_TRUE(csv.canStart());
    TEST_ASSERT_TRUE(csv.start(7));

    // First sample logged immediately; every acquired sample is consumed.
    csv.onSample(sample(7, 1, 0, 23.5f, 0));
    csv.onSample(sample(7, 2, 500, 23.6f, SENSOR_FAULT_OPEN));
    csv.onSample(sample(7, 3, 1000, NAN, SENSOR_FAULT_NAN));
    csv.onSample(sample(7, 4, 1500, 23.7f, 0));
    csv.onSample(sample(7, 5, 2000, 23.8f, 0));
    csv.onSample(sample(7, 6, 2500, 23.9f, 0));
    csv.onSample(sample(7, 7, 3000, 24.0f, 0));  // dt >= 3000 -> logged

    TEST_ASSERT_TRUE(csv.stop(3500));  // latest already selected -> no row

    const auto rows = dataRows(storage.text());
    TEST_ASSERT_EQUAL_UINT32(2, rows.size());
    TEST_ASSERT_EQUAL_UINT32(1, rows[0].acqSeq);
    TEST_ASSERT_EQUAL_UINT32(0, rows[0].ms);
    TEST_ASSERT_EQUAL_FLOAT(23.5f, rows[0].temp);
    TEST_ASSERT_EQUAL_UINT32(7, rows[1].acqSeq);
    TEST_ASSERT_EQUAL_UINT32(3000, rows[1].ms);
    TEST_ASSERT_EQUAL_FLOAT(24.0f, rows[1].temp);
    TEST_ASSERT_EQUAL_UINT32(0, rows[1].fault);
    TEST_ASSERT_EQUAL_UINT32(SENSOR_FAULT_OPEN | SENSOR_FAULT_NAN, rows[1].intervalFault);

    const Status st = csv.status();
    TEST_ASSERT_TRUE(st.mounted);
    TEST_ASSERT_TRUE(st.hasRun);
    TEST_ASSERT_FALSE(st.recording);
    TEST_ASSERT_EQUAL_UINT32(7, st.runId);
    TEST_ASSERT_EQUAL_UINT32(2, st.sampleCount);
    TEST_ASSERT_EQUAL_UINT32(storage.file.size(), st.bytes);
    TEST_ASSERT_EQUAL_STRING("complete", st.state);
    TEST_ASSERT_NULL(st.error);
}

static void test_float_formatting_round_trip() {
    TestStorage storage;
    SingleRunCsv csv(storage);
    TEST_ASSERT_TRUE(csv.begin());
    TEST_ASSERT_TRUE(csv.start(121));

    csv.onSample(sample(121, 1, 0, -0.0f, 0));
    for (uint32_t seq = 2; seq <= 6; ++seq) {
        csv.onSample(sample(121, seq, (seq - 1) * 500, 1.0f, 0));
    }
    csv.onSample(sample(121, 7, 3000, 1.40129846e-45f, 0));  // denormal min
    for (uint32_t seq = 8; seq <= 12; ++seq) {
        csv.onSample(sample(121, seq, (seq - 1) * 500, 1.0f, 0));
    }
    csv.onSample(sample(121, 13, 6000, 3.40282347e+38f, 0));  // float max
    TEST_ASSERT_TRUE(csv.stop(6100));

    const auto rows = dataRows(storage.text());
    TEST_ASSERT_EQUAL_UINT32(3, rows.size());
    const float expected[] = {-0.0f, 1.40129846e-45f, 3.40282347e38f};
    for (size_t i = 0; i < rows.size(); ++i) {
        TEST_ASSERT_EQUAL_MEMORY(&expected[i], &rows[i].temp, sizeof(float));
    }
    SingleRunCsv rebooted(storage);
    TEST_ASSERT_TRUE(rebooted.begin());
    TEST_ASSERT_EQUAL_UINT32(3, rebooted.status().sampleCount);
    TEST_ASSERT_EQUAL_UINT32(storage.file.size(), rebooted.status().bytes);
    TEST_ASSERT_EQUAL_STRING("complete", rebooted.status().state);
}

// ------------------------------------------------------- selection/edges ---

static void test_terminal_reading_when_latest_unselected() {
    TestStorage storage;
    SingleRunCsv csv(storage);
    TEST_ASSERT_TRUE(csv.begin());
    TEST_ASSERT_TRUE(csv.start(22));

    csv.onSample(sample(22, 1, 0, 5.0f, 0));    // logged
    csv.onSample(sample(22, 2, 500, 6.0f, 7));  // not yet due

    TEST_ASSERT_TRUE(csv.stop(600));  // selects the latest, then the footer

    const std::string expected =
        "# pyrovue-single-run-csv,1\n"
        "# run_id,22\n"
        "# acquire_interval_ms,500\n"
        "# log_interval_ms,3000\n"
        "# max_duration_ms,21600000\n"
        "# record_limit,7202\n"
        "run_id,log_seq,acq_seq,elapsed_ms,temp_c,fault,interval_fault,flags\n"
        "22,1,1,0,5,0,0,0\n"
        "22,2,2,500,6,7,7,1\n"  // terminal row preserves actual seq/ms/value
        "# end,reason=complete,rows=2,elapsed_ms=600\n";
    TEST_ASSERT_EQUAL_STRING(expected.c_str(), storage.text().c_str());
}

static void test_stop_after_selection_adds_no_row() {
    TestStorage storage;
    SingleRunCsv csv(storage);
    TEST_ASSERT_TRUE(csv.begin());
    TEST_ASSERT_TRUE(csv.start(23));

    csv.onSample(sample(23, 1, 0, 5.0f, 0));
    for (uint32_t seq = 2; seq <= 7; ++seq) {
        csv.onSample(sample(23, seq, (seq - 1) * 500, 6.0f + seq, 0));
    }
    // seq 7 at 3000ms is selected (dt >= 3000); the latest is already logged.
    TEST_ASSERT_TRUE(csv.stop(3100));

    TEST_ASSERT_EQUAL_UINT32(2, csv.status().sampleCount);
    TEST_ASSERT_TRUE(storage.text().find("23,2,7,3000,13,0,0,0\n") != std::string::npos);
    TEST_ASSERT_TRUE(storage.text().find("# end,reason=complete,rows=2,elapsed_ms=3100\n") !=
                     std::string::npos);
}

static void test_invalid_samples_stop_without_claiming_complete() {
    const TempSample invalid[] = {
        sample(111, 2, 0, 2.0f, 0),
        sample(111, 1, 500, 3.0f, 0),
        sample(111, 0, 500, 3.0f, 0),
    };
    for (const auto& bad : invalid) {
        TestStorage storage;
        SingleRunCsv csv(storage);
        TEST_ASSERT_TRUE(csv.begin());
        TEST_ASSERT_TRUE(csv.start(111));
        csv.onSample(sample(111, 1, 0, 1.0f, 0));
        const std::string prefix = storage.text();
        csv.onSample(bad);
        TEST_ASSERT_FALSE(csv.status().recording);
        TEST_ASSERT_EQUAL_STRING("invalid_sample", csv.status().error);
        TEST_ASSERT_EQUAL_STRING(prefix.c_str(), storage.text().c_str());
        TEST_ASSERT_FALSE(csv.stop(600));
        SingleRunCsv rebooted(storage);
        TEST_ASSERT_TRUE(rebooted.begin());
        TEST_ASSERT_EQUAL_UINT32(1, rebooted.status().sampleCount);
        TEST_ASSERT_EQUAL_STRING("interrupted", rebooted.status().state);
    }
    TestStorage storage;
    SingleRunCsv csv(storage);
    TEST_ASSERT_TRUE(csv.begin());
    TEST_ASSERT_TRUE(csv.start(111));
    csv.onSample(sample(111, 0, 500, 3.0f, 0));
    TEST_ASSERT_FALSE(csv.status().recording);
    TEST_ASSERT_EQUAL_UINT32(0, csv.status().sampleCount);
    TEST_ASSERT_EQUAL_STRING("invalid_sample", csv.status().error);
    SingleRunCsv rebooted(storage);
    TEST_ASSERT_TRUE(rebooted.begin());
    TEST_ASSERT_EQUAL_UINT32(0, rebooted.status().sampleCount);
    TEST_ASSERT_EQUAL_UINT32(csv.status().bytes, rebooted.status().bytes);
}

// ------------------------------------------------------------------ gaps ---

static void test_gap_hidden_hole_flags_next_row() {
    TestStorage storage;
    SingleRunCsv csv(storage);
    TEST_ASSERT_TRUE(csv.begin());
    TEST_ASSERT_TRUE(csv.start(9));

    csv.onSample(sample(9, 1, 0, 10.0f, 0));     // logged (never gap-flagged)
    csv.onSample(sample(9, 2, 500, 11.0f, 0));
    csv.onSample(sample(9, 3, 1000, 12.0f, 0));
    csv.onSample(sample(9, 4, 2000, 13.0f, 0));  // 1000ms hole -> sticky gap
    csv.onSample(sample(9, 5, 2500, 13.5f, 0));
    csv.onSample(sample(9, 6, 3000, 14.0f, 0));  // logged (net dt looks normal)

    csv.onSample(sample(9, 7, 3500, 15.0f, 0));
    csv.onSample(sample(9, 8, 4000, 16.0f, 0));
    csv.onSample(sample(9, 9, 4500, 16.0f, 0));
    csv.onSample(sample(9, 10, 5000, 16.0f, 0));
    csv.onSample(sample(9, 11, 5500, 16.0f, 0));
    csv.onSample(sample(9, 12, 6000, 16.0f, 0));  // logged, gap already consumed
    TEST_ASSERT_TRUE(csv.stop(6100));

    const std::string expected =
        "# pyrovue-single-run-csv,1\n"
        "# run_id,9\n"
        "# acquire_interval_ms,500\n"
        "# log_interval_ms,3000\n"
        "# max_duration_ms,21600000\n"
        "# record_limit,7202\n"
        "run_id,log_seq,acq_seq,elapsed_ms,temp_c,fault,interval_fault,flags\n"
        "9,1,1,0,10,0,0,0\n"
        "9,2,6,3000,14,0,0,2\n"  // hole between rows 1-2 hidden in net dt
        "9,3,12,6000,16,0,0,0\n"  // flag consumed by row 2, then reset
        "# end,reason=complete,rows=3,elapsed_ms=6100\n";
    TEST_ASSERT_EQUAL_STRING(expected.c_str(), storage.text().c_str());
}

static void test_gap_jitter_cadence_never_flags() {
    TestStorage storage;
    SingleRunCsv csv(storage);
    TEST_ASSERT_TRUE(csv.begin());
    TEST_ASSERT_TRUE(csv.start(12));

    // Six readings 550ms apart: 300ms accumulated drift, but no gap ever.
    for (uint32_t seq = 1; seq <= 7; ++seq) {
        csv.onSample(sample(12, seq, (seq - 1) * 550, 20.0f + seq, 0));
    }
    TEST_ASSERT_TRUE(csv.stop(3400));

    const std::string expected =
        "# pyrovue-single-run-csv,1\n"
        "# run_id,12\n"
        "# acquire_interval_ms,500\n"
        "# log_interval_ms,3000\n"
        "# max_duration_ms,21600000\n"
        "# record_limit,7202\n"
        "run_id,log_seq,acq_seq,elapsed_ms,temp_c,fault,interval_fault,flags\n"
        "12,1,1,0,21,0,0,0\n"
        "12,2,7,3300,27,0,0,0\n"
        "# end,reason=complete,rows=2,elapsed_ms=3400\n";
    TEST_ASSERT_EQUAL_STRING(expected.c_str(), storage.text().c_str());
}

static void test_gap_sequence_jump_flags() {
    TestStorage storage;
    SingleRunCsv csv(storage);
    TEST_ASSERT_TRUE(csv.begin());
    TEST_ASSERT_TRUE(csv.start(13));

    csv.onSample(sample(13, 1, 0, 1.0f, 0));    // logged
    csv.onSample(sample(13, 5, 500, 2.0f, 0));  // seq jump -> sticky gap
    for (uint32_t seq = 6; seq <= 10; ++seq) {
        csv.onSample(sample(13, seq, (seq - 4) * 500, 3.0f, 0));
    }
    TEST_ASSERT_TRUE(csv.stop(3100));

    const std::string text = storage.text();
    TEST_ASSERT_TRUE(text.find("13,1,1,0,1,0,0,0\n") != std::string::npos);
    TEST_ASSERT_TRUE(text.find("13,2,10,3000,3,0,0,2\n") != std::string::npos);
}

// ------------------------------------------------------- duration limits ---

static void test_duration_limit_from_sample_boundary() {
    TestStorage storage;
    SingleRunCsv csv(storage);
    TEST_ASSERT_TRUE(csv.begin());
    TEST_ASSERT_TRUE(csv.start(31));

    // The run joins near the boundary; the first sample has no gap baseline.
    csv.onSample(sample(31, 1, 21599500, 20.0f, 0));   // logged
    csv.onSample(sample(31, 2, 21600000, 21.0f, 0));   // boundary -> terminal

    const Status st = csv.status();
    TEST_ASSERT_FALSE(st.recording);
    TEST_ASSERT_EQUAL_STRING("duration_limit", st.state);
    TEST_ASSERT_EQUAL_UINT32(2, st.sampleCount);
    TEST_ASSERT_NULL(st.error);

    const std::string text = storage.text();
    TEST_ASSERT_TRUE(text.find("31,1,1,21599500,20,0,0,0\n") != std::string::npos);
    TEST_ASSERT_TRUE(text.find("31,2,2,21600000,21,0,0,1\n") != std::string::npos);
    TEST_ASSERT_TRUE(text.find("# end,reason=duration_limit,rows=2,elapsed_ms=21600000\n") !=
                     std::string::npos);

    // Samples after the boundary are ignored.
    csv.onSample(sample(31, 3, 21600500, 22.0f, 0));
    TEST_ASSERT_EQUAL_UINT32(2, csv.status().sampleCount);
    TEST_ASSERT_FALSE(csv.stop(21601000));
}

static void test_stop_at_duration_cap_marks_duration_limit() {
    TestStorage storage;
    SingleRunCsv csv(storage);
    TEST_ASSERT_TRUE(csv.begin());
    TEST_ASSERT_TRUE(csv.start(41));

    csv.onSample(sample(41, 1, 0, 9.0f, 0));
    // The caller enforces the cap independently of samples.
    TEST_ASSERT_TRUE(csv.stop(21600000));

    const Status st = csv.status();
    TEST_ASSERT_EQUAL_STRING("duration_limit", st.state);
    TEST_ASSERT_TRUE(storage.text().find("# end,reason=duration_limit,rows=1,elapsed_ms=21600000\n") !=
                     std::string::npos);
}

static void test_duration_tail_excludes_out_of_window_faults_and_clamps_footer() {
    TestStorage storage;
    SingleRunCsv csv(storage);
    TEST_ASSERT_TRUE(csv.begin());
    TEST_ASSERT_TRUE(csv.start(31));
    csv.onSample(sample(31, 1, maxDurationMs - 1000, 1.0f, 0));
    csv.onSample(sample(31, 2, maxDurationMs - 500, 2.0f, 0));
    csv.onSample(sample(31, 3, maxDurationMs + 1000, NAN, SENSOR_FAULT_OPEN | SENSOR_FAULT_NAN));
    const auto rows = dataRows(storage.text());
    TEST_ASSERT_EQUAL_UINT32(2, rows.size());
    TEST_ASSERT_EQUAL_UINT32(maxDurationMs - 500, rows[1].ms);
    TEST_ASSERT_EQUAL_UINT32(0, rows[1].intervalFault);
    TEST_ASSERT_EQUAL_UINT32(kFlagTerminal, rows[1].flags);
    SingleRunCsv rebooted(storage);
    TEST_ASSERT_TRUE(rebooted.begin());
    TEST_ASSERT_EQUAL_STRING("duration_limit", rebooted.status().state);

    TestStorage delayedStorage;
    SingleRunCsv delayed(delayedStorage);
    TEST_ASSERT_TRUE(delayed.begin());
    TEST_ASSERT_TRUE(delayed.start(32));
    delayed.onSample(sample(32, 1, maxDurationMs - 500, 2.0f, 0));
    TEST_ASSERT_TRUE(delayed.stop(maxDurationMs + 1000));
    SingleRunCsv delayedBoot(delayedStorage);
    TEST_ASSERT_TRUE(delayedBoot.begin());
    TEST_ASSERT_EQUAL_STRING("duration_limit", delayedBoot.status().state);
    TEST_ASSERT_EQUAL_UINT32(delayedStorage.file.size(), delayedBoot.status().bytes);
}

static void test_full_duration_capacity_within_budget() {
    TestStorage storage;
    SingleRunCsv csv(storage);
    TEST_ASSERT_TRUE(csv.begin());
    TEST_ASSERT_TRUE(csv.start(101));

    // Full 6h at the 500ms acquire cadence: exactly the record budget.
    for (uint32_t seq = 1; seq <= 43201; ++seq) {
        csv.onSample(sample(101, seq, (seq - 1) * 500, 5.0f, 0));
    }

    const Status st = csv.status();
    TEST_ASSERT_EQUAL_STRING("duration_limit", st.state);
    TEST_ASSERT_EQUAL_UINT32(7201, st.sampleCount);  // 7200 interval + 1 terminal
    TEST_ASSERT_TRUE(st.sampleCount <= recordLimit);
    TEST_ASSERT_EQUAL_UINT32(storage.file.size(), st.bytes);
    TEST_ASSERT_TRUE(st.bytes <= startRequiredBytes);
    TEST_ASSERT_TRUE(storage.text().find("101,7201,43201,21600000,5,0,0,1\n") != std::string::npos);
    TEST_ASSERT_TRUE(storage.text().find("# end,reason=duration_limit,rows=7201,elapsed_ms=21600000\n") !=
                     std::string::npos);
}

// ------------------------------------------------------ storage failures ---

static void test_write_failure_keeps_validated_prefix() {
    TestStorage storage;
    SingleRunCsv csv(storage);
    TEST_ASSERT_TRUE(csv.begin());
    TEST_ASSERT_TRUE(csv.start(51));

    csv.onSample(sample(51, 1, 0, 1.0f, 0));
    const uint32_t prefixBytes = csv.status().bytes;

    storage.writeBudget = 5;  // short write in the middle of row 2
    csv.onSample(sample(51, 2, 3000, 2.0f, 0));

    const Status st = csv.status();
    TEST_ASSERT_FALSE(st.recording);
    TEST_ASSERT_EQUAL_STRING("error", st.state);
    TEST_ASSERT_EQUAL_STRING("write_failed", st.error);
    TEST_ASSERT_EQUAL_UINT32(1, st.sampleCount);
    TEST_ASSERT_EQUAL_UINT32(prefixBytes, st.bytes);  // excludes the partial row
    TEST_ASSERT_TRUE(storage.file.size() > prefixBytes);  // garbage tail stays

    // Reboot: the identical validated prefix survives, nothing is discarded.
    storage.closeWriter();
    SingleRunCsv rebooted(storage);
    TEST_ASSERT_TRUE(rebooted.begin());
    const Status back = rebooted.status();
    TEST_ASSERT_EQUAL_STRING("interrupted", back.state);
    TEST_ASSERT_EQUAL_UINT32(51, back.runId);
    TEST_ASSERT_EQUAL_UINT32(1, back.sampleCount);
    TEST_ASSERT_EQUAL_UINT32(prefixBytes, back.bytes);

    // Download is bounded to the prefix even though the file is longer.
    TEST_ASSERT_TRUE(rebooted.openDownload(51));
    std::vector<uint8_t> out(storage.file.size() + 16);
    size_t total = 0;
    for (;;) {
        const size_t n = rebooted.readDownload(out.data() + total, 7);
        if (n == 0) break;
        total += n;
    }
    rebooted.closeDownload();
    TEST_ASSERT_EQUAL_UINT32(prefixBytes, total);
    TEST_ASSERT_EQUAL_MEMORY(storage.file.data(), out.data(), prefixBytes);
}

static void test_sync_failure_keeps_accounted_row() {
    TestStorage storage;
    SingleRunCsv csv(storage);
    TEST_ASSERT_TRUE(csv.begin());
    TEST_ASSERT_TRUE(csv.start(61));

    csv.onSample(sample(61, 1, 0, 1.0f, 0));
    storage.syncOk = false;
    csv.onSample(sample(61, 2, 3000, 2.0f, 0));

    const Status st = csv.status();
    TEST_ASSERT_FALSE(st.recording);
    TEST_ASSERT_EQUAL_STRING("error", st.state);
    TEST_ASSERT_EQUAL_STRING("sync_failed", st.error);
    // The row is fully in the file and validates: it stays in the prefix.
    TEST_ASSERT_EQUAL_UINT32(2, st.sampleCount);
    TEST_ASSERT_EQUAL_UINT32(storage.file.size(), st.bytes);

    storage.closeWriter();
    storage.syncOk = true;
    SingleRunCsv rebooted(storage);
    TEST_ASSERT_TRUE(rebooted.begin());
    const Status back = rebooted.status();
    TEST_ASSERT_EQUAL_STRING("interrupted", back.state);
    TEST_ASSERT_EQUAL_UINT32(2, back.sampleCount);
    TEST_ASSERT_EQUAL_UINT32(st.bytes, back.bytes);  // reboot-identical
}

static void test_capacity_failure_stops_recording() {
    TestStorage storage;
    SingleRunCsv csv(storage);
    TEST_ASSERT_TRUE(csv.begin());
    TEST_ASSERT_TRUE(csv.start(71));

    csv.onSample(sample(71, 1, 0, 1.0f, 0));
    const uint32_t bytesBefore = csv.status().bytes;

    storage.free = 4096;  // below the conservative floor
    csv.onSample(sample(71, 2, 3000, 2.0f, 0));

    const Status st = csv.status();
    TEST_ASSERT_FALSE(st.recording);
    TEST_ASSERT_EQUAL_STRING("error", st.state);
    TEST_ASSERT_EQUAL_STRING("storage_full", st.error);
    TEST_ASSERT_EQUAL_UINT32(1, st.sampleCount);
    TEST_ASSERT_EQUAL_UINT32(bytesBefore, st.bytes);
    TEST_ASSERT_EQUAL_UINT32(storage.file.size(), st.bytes);  // no partial row
}

static void test_remove_failure_sets_error_and_retry() {
    TestStorage storage;
    SingleRunCsv csv(storage);
    TEST_ASSERT_TRUE(csv.begin());
    TEST_ASSERT_TRUE(csv.start(81));
    csv.onSample(sample(81, 1, 0, 1.0f, 0));
    TEST_ASSERT_TRUE(csv.stop(100));

    storage.removeOk = false;
    TEST_ASSERT_FALSE(csv.erase(81));
    TEST_ASSERT_EQUAL_STRING("error", csv.status().state);
    TEST_ASSERT_EQUAL_STRING("remove_failed", csv.status().error);
    TEST_ASSERT_TRUE(csv.status().hasRun);

    storage.removeOk = true;
    TEST_ASSERT_TRUE(csv.erase(81));
    TEST_ASSERT_FALSE(csv.status().hasRun);
    TEST_ASSERT_EQUAL_STRING("idle", csv.status().state);
    TEST_ASSERT_NULL(csv.status().error);
}

// --------------------------------------------------------- boot recovery ---

static void test_boot_recovers_complete_run() {
    TestStorage storage;
    SingleRunCsv csv(storage);
    TEST_ASSERT_TRUE(csv.begin());
    TEST_ASSERT_TRUE(csv.start(91));
    csv.onSample(sample(91, 1, 0, 1.0f, 0));
    csv.onSample(sample(91, 7, 3000, 2.0f, 0));
    TEST_ASSERT_TRUE(csv.stop(3500));
    const std::string before = storage.text();

    storage.closeWriter();
    SingleRunCsv rebooted(storage);
    TEST_ASSERT_TRUE(rebooted.begin());
    const Status st = rebooted.status();
    TEST_ASSERT_EQUAL_STRING("complete", st.state);
    TEST_ASSERT_EQUAL_UINT32(91, st.runId);
    TEST_ASSERT_EQUAL_UINT32(2, st.sampleCount);
    TEST_ASSERT_EQUAL_UINT32(before.size(), st.bytes);  // identical bytes
    TEST_ASSERT_EQUAL_STRING(before.c_str(), storage.text().c_str());
}

static void test_boot_recovers_interrupted_prefix() {
    TestStorage storage;
    SingleRunCsv csv(storage);
    TEST_ASSERT_TRUE(csv.begin());
    TEST_ASSERT_TRUE(csv.start(92));
    csv.onSample(sample(92, 1, 0, 1.0f, 0));
    csv.onSample(sample(92, 7, 3000, 2.0f, 0));
    const uint32_t prefixBytes = csv.status().bytes;

    // Power loss mid-row: partial trailing bytes, no footer.
    storage.closeWriter();
    const char* partial = "92,3,";
    storage.file.insert(storage.file.end(), partial, partial + strlen(partial));

    SingleRunCsv rebooted(storage);
    TEST_ASSERT_TRUE(rebooted.begin());
    const Status st = rebooted.status();
    TEST_ASSERT_EQUAL_STRING("interrupted", st.state);
    TEST_ASSERT_EQUAL_UINT32(92, st.runId);
    TEST_ASSERT_EQUAL_UINT32(2, st.sampleCount);
    TEST_ASSERT_EQUAL_UINT32(prefixBytes, st.bytes);
}

static void test_boot_footer_must_be_valid_last_line() {
    // Variant A: a corrupt row truncates the prefix before it.
    TestStorage storageA;
    SingleRunCsv csvA(storageA);
    TEST_ASSERT_TRUE(csvA.begin());
    TEST_ASSERT_TRUE(csvA.start(93));
    csvA.onSample(sample(93, 1, 0, 1.0f, 0));
    const uint32_t row1End = csvA.status().bytes;
    csvA.onSample(sample(93, 7, 3000, 2.0f, 0));
    TEST_ASSERT_TRUE(csvA.stop(3500));
    storageA.closeWriter();
    storageA.file[row1End + 2] ^= 0x30;  // break row 2

    SingleRunCsv rebootedA(storageA);
    TEST_ASSERT_TRUE(rebootedA.begin());
    TEST_ASSERT_EQUAL_STRING("interrupted", rebootedA.status().state);
    TEST_ASSERT_EQUAL_UINT32(1, rebootedA.status().sampleCount);
    TEST_ASSERT_EQUAL_UINT32(row1End, rebootedA.status().bytes);

    // Variant B: footer row count mismatch -> footer invalid.
    TestStorage storageB;
    SingleRunCsv csvB(storageB);
    TEST_ASSERT_TRUE(csvB.begin());
    TEST_ASSERT_TRUE(csvB.start(94));
    csvB.onSample(sample(94, 1, 0, 1.0f, 0));
    csvB.onSample(sample(94, 7, 3000, 2.0f, 0));
    TEST_ASSERT_TRUE(csvB.stop(3500));
    storageB.closeWriter();
    std::string textB = storageB.text();
    const size_t rowsPos = textB.rfind("rows=2");
    TEST_ASSERT_TRUE(rowsPos != std::string::npos);
    storageB.file[rowsPos + 5] = '3';
    const uint32_t rowsEndB = csvB.status().bytes;  // includes the footer...

    SingleRunCsv rebootedB(storageB);
    TEST_ASSERT_TRUE(rebootedB.begin());
    TEST_ASSERT_EQUAL_STRING("interrupted", rebootedB.status().state);
    TEST_ASSERT_EQUAL_UINT32(2, rebootedB.status().sampleCount);
    TEST_ASSERT_TRUE(rebootedB.status().bytes < rowsEndB);  // footer excluded

    // Variant C: data after the footer -> footer is not the last line.
    TestStorage storageC;
    SingleRunCsv csvC(storageC);
    TEST_ASSERT_TRUE(csvC.begin());
    TEST_ASSERT_TRUE(csvC.start(95));
    csvC.onSample(sample(95, 1, 0, 1.0f, 0));
    csvC.onSample(sample(95, 7, 3000, 2.0f, 0));
    TEST_ASSERT_TRUE(csvC.stop(3500));
    storageC.closeWriter();
    const uint32_t rowsEndC = csvC.status().bytes;  // with footer
    const char* extra = "\n95,9,9,9,1,0,0,0\n";
    storageC.file.insert(storageC.file.end(), extra, extra + strlen(extra));

    SingleRunCsv rebootedC(storageC);
    TEST_ASSERT_TRUE(rebootedC.begin());
    TEST_ASSERT_EQUAL_STRING("interrupted", rebootedC.status().state);
    TEST_ASSERT_EQUAL_UINT32(2, rebootedC.status().sampleCount);
    TEST_ASSERT_TRUE(rebootedC.status().bytes < rowsEndC);
}

static void test_recovery_rejects_post_terminal_rows_and_inconsistent_end_time() {
    TestStorage storage;
    SingleRunCsv csv(storage);
    TEST_ASSERT_TRUE(csv.begin());
    TEST_ASSERT_TRUE(csv.start(22));
    csv.onSample(sample(22, 1, 0, 1.0f, 0));
    csv.onSample(sample(22, 2, 500, 2.0f, 0));
    TEST_ASSERT_TRUE(csv.stop(600));
    const std::string text = storage.text();
    const std::string prefix = text.substr(0, text.find("# end,"));
    const std::string tails[] = {
        "22,3,3,1000,3,0,0,0\n# end,reason=complete,rows=3,elapsed_ms=1100\n",
        "# end,reason=complete,rows=2,elapsed_ms=0\n",
        "# end,reason=duration_limit,rows=2,elapsed_ms=500\n",
    };
    for (const auto& tail : tails) {
        const std::string invalid = prefix + tail;
        storage.file.assign(invalid.begin(), invalid.end());
        SingleRunCsv rebooted(storage);
        TEST_ASSERT_TRUE(rebooted.begin());
        TEST_ASSERT_EQUAL_STRING("interrupted", rebooted.status().state);
        TEST_ASSERT_EQUAL_UINT32(2, rebooted.status().sampleCount);
        TEST_ASSERT_EQUAL_UINT32(prefix.size(), rebooted.status().bytes);
    }
}

static void test_corrupt_saved_run_blocks_and_erase_zero() {
    TestStorage storage;
    storage.filePresent = true;
    const char* garbage = "hello\nworld\n";
    storage.file.assign(garbage, garbage + strlen(garbage));

    SingleRunCsv csv(storage);
    TEST_ASSERT_TRUE(csv.begin());
    const Status st = csv.status();
    TEST_ASSERT_TRUE(st.mounted);       // mount is fine; content is not
    TEST_ASSERT_TRUE(st.hasRun);        // corrupt file stays visible
    TEST_ASSERT_EQUAL_STRING("error", st.state);
    TEST_ASSERT_EQUAL_STRING("corrupt_saved_run", st.error);
    TEST_ASSERT_EQUAL_UINT32(0, st.runId);  // identity unrecoverable
    TEST_ASSERT_EQUAL_UINT32(0, st.bytes);
    TEST_ASSERT_FALSE(csv.canStart());
    TEST_ASSERT_FALSE(csv.start(5));
    TEST_ASSERT_FALSE(csv.openDownload(0));  // no valid CSV prefix

    // Only the explicit id-0 erase may delete the unknown-identity file.
    TEST_ASSERT_FALSE(csv.erase(5));
    TEST_ASSERT_TRUE(csv.erase(0));
    TEST_ASSERT_FALSE(csv.status().hasRun);
    TEST_ASSERT_EQUAL_STRING("idle", csv.status().state);
    TEST_ASSERT_NULL(csv.status().error);
    TEST_ASSERT_TRUE(csv.canStart());

    // Incomplete metadata (e.g. failed start + reboot) classifies the same.
    TestStorage storage2;
    storage2.filePresent = true;
    const char* partial = "# pyrovue-single-run-csv,1\n# run_id,9\n";
    storage2.file.assign(partial, partial + strlen(partial));
    SingleRunCsv csv2(storage2);
    TEST_ASSERT_TRUE(csv2.begin());
    TEST_ASSERT_EQUAL_STRING("corrupt_saved_run", csv2.status().error);
    TEST_ASSERT_EQUAL_UINT32(0, csv2.status().runId);
    TEST_ASSERT_TRUE(csv2.status().hasRun);
    TEST_ASSERT_TRUE(csv2.erase(0));
}

// -------------------------------------------------------------- guards ---

static void test_no_overwrite_and_reader_busy_guards() {
    TestStorage storage;
    SingleRunCsv csv(storage);
    TEST_ASSERT_TRUE(csv.begin());
    TEST_ASSERT_TRUE(csv.start(91));
    csv.onSample(sample(91, 1, 0, 1.0f, 0));
    TEST_ASSERT_TRUE(csv.stop(100));

    // A saved run blocks start until explicitly erased; never overwritten.
    TEST_ASSERT_FALSE(csv.canStart());
    TEST_ASSERT_FALSE(csv.start(92));
    TEST_ASSERT_TRUE(csv.status().hasRun);
    TEST_ASSERT_EQUAL_UINT32(91, csv.status().runId);
    TEST_ASSERT_TRUE(csv.erase(91));
    TEST_ASSERT_TRUE(csv.start(92));
    csv.onSample(sample(92, 1, 0, 1.0f, 0));

    // While recording: no erase/format/download, no second start.
    TEST_ASSERT_FALSE(csv.start(93));
    TEST_ASSERT_FALSE(csv.erase(92));
    TEST_ASSERT_FALSE(csv.format());
    TEST_ASSERT_FALSE(csv.openDownload(92));

    TEST_ASSERT_TRUE(csv.stop(100));

    // While the download reader is open: busy everywhere.
    TEST_ASSERT_FALSE(csv.openDownload(93));  // stale id
    TEST_ASSERT_TRUE(csv.openDownload(92));
    TEST_ASSERT_FALSE(csv.erase(92));
    TEST_ASSERT_FALSE(csv.format());
    TEST_ASSERT_FALSE(csv.start(93));
    TEST_ASSERT_FALSE(csv.openDownload(92));  // already open
    csv.closeDownload();

    TEST_ASSERT_TRUE(csv.erase(92));
    TEST_ASSERT_EQUAL_STRING("idle", csv.status().state);
}

static void test_format_guards_and_full_erase() {
    TestStorage storage;
    SingleRunCsv csv(storage);
    TEST_ASSERT_TRUE(csv.begin());
    TEST_ASSERT_TRUE(csv.start(97));
    csv.onSample(sample(97, 1, 0, 1.0f, 0));
    TEST_ASSERT_TRUE(csv.stop(100));

    TEST_ASSERT_FALSE(csv.format());  // saved run present
    TEST_ASSERT_TRUE(csv.erase(97));
    TEST_ASSERT_TRUE(csv.format());   // explicit full-volume erase
    TEST_ASSERT_FALSE(csv.status().hasRun);
    TEST_ASSERT_TRUE(csv.status().mounted);
    TEST_ASSERT_EQUAL_STRING("idle", csv.status().state);
    TEST_ASSERT_TRUE(storage.text().empty());
    TEST_ASSERT_FALSE(storage.filePresent);

    // Recording or an open reader prevents format.
    TEST_ASSERT_TRUE(csv.start(98));
    TEST_ASSERT_FALSE(csv.format());
    TEST_ASSERT_TRUE(csv.stop(100));
    TEST_ASSERT_TRUE(csv.openDownload(98));
    TEST_ASSERT_FALSE(csv.format());
    csv.closeDownload();
    TEST_ASSERT_TRUE(csv.erase(98));
}

static void test_mount_failure_and_format_recovery() {
    TestStorage storage;
    storage.mountOk = false;
    SingleRunCsv csv(storage);
    TEST_ASSERT_FALSE(csv.begin());
    const Status st = csv.status();
    TEST_ASSERT_FALSE(st.mounted);
    TEST_ASSERT_EQUAL_STRING("error", st.state);
    TEST_ASSERT_EQUAL_STRING("mount_failed", st.error);
    TEST_ASSERT_FALSE(st.hasRun);  // contents unknown
    TEST_ASSERT_FALSE(csv.canStart());
    TEST_ASSERT_FALSE(csv.start(5));
    TEST_ASSERT_FALSE(csv.openDownload(5));

    // Format remains reachable for explicit provisioning of a bad volume.
    TEST_ASSERT_FALSE(csv.format());  // remount after format still fails
    TEST_ASSERT_EQUAL_STRING("format_failed", csv.status().error);

    storage.mountOk = true;
    TEST_ASSERT_TRUE(csv.format());
    TEST_ASSERT_TRUE(csv.status().mounted);
    TEST_ASSERT_EQUAL_STRING("idle", csv.status().state);
    TEST_ASSERT_NULL(csv.status().error);
    TEST_ASSERT_TRUE(csv.canStart());
}

static void test_start_requires_capacity() {
    TestStorage storage;
    storage.free = startRequiredBytes - 1;
    SingleRunCsv csv(storage);
    TEST_ASSERT_TRUE(csv.begin());
    TEST_ASSERT_FALSE(csv.canStart());
    TEST_ASSERT_FALSE(csv.start(5));
    TEST_ASSERT_FALSE(csv.status().hasRun);
    TEST_ASSERT_NULL(csv.status().error);  // refusal, not a failure

    storage.free = startRequiredBytes;
    TEST_ASSERT_TRUE(csv.canStart());
    TEST_ASSERT_TRUE(csv.start(5));
}

// ------------------------------------------------------------- downloads ---

static void test_download_bounded_and_read_error() {
    TestStorage storage;
    SingleRunCsv csv(storage);
    TEST_ASSERT_TRUE(csv.begin());
    TEST_ASSERT_TRUE(csv.start(99));
    csv.onSample(sample(99, 1, 0, 1.0f, 0));
    csv.onSample(sample(99, 7, 3000, 2.0f, 0));
    TEST_ASSERT_TRUE(csv.stop(3500));
    const uint32_t bytes = csv.status().bytes;

    TEST_ASSERT_FALSE(csv.openDownload(98));
    TEST_ASSERT_TRUE(csv.openDownload(99));

    std::vector<uint8_t> out(bytes + 8);
    size_t total = 0;
    for (;;) {
        const size_t n = csv.readDownload(out.data() + total, 4);
        if (n == 0) break;
        total += n;
        TEST_ASSERT_TRUE(total <= bytes);
    }
    csv.closeDownload();
    TEST_ASSERT_EQUAL_UINT32(bytes, total);
    TEST_ASSERT_EQUAL_MEMORY(storage.file.data(), out.data(), bytes);

    // A read error terminates the stream at the validated prefix: no garbage.
    TEST_ASSERT_TRUE(csv.openDownload(99));
    storage.readBudget = 3;
    size_t first = csv.readDownload(out.data(), 100);
    TEST_ASSERT_EQUAL_UINT32(3, first);
    TEST_ASSERT_EQUAL_UINT32(0, csv.readDownload(out.data(), 100));
    csv.closeDownload();
}

void setup() {
    UNITY_BEGIN();
    RUN_TEST(test_selection_preserves_interval_faults);
    RUN_TEST(test_float_formatting_round_trip);
    RUN_TEST(test_terminal_reading_when_latest_unselected);
    RUN_TEST(test_stop_after_selection_adds_no_row);
    RUN_TEST(test_invalid_samples_stop_without_claiming_complete);
    RUN_TEST(test_gap_hidden_hole_flags_next_row);
    RUN_TEST(test_gap_jitter_cadence_never_flags);
    RUN_TEST(test_gap_sequence_jump_flags);
    RUN_TEST(test_duration_limit_from_sample_boundary);
    RUN_TEST(test_stop_at_duration_cap_marks_duration_limit);
    RUN_TEST(test_duration_tail_excludes_out_of_window_faults_and_clamps_footer);
    RUN_TEST(test_full_duration_capacity_within_budget);
    RUN_TEST(test_write_failure_keeps_validated_prefix);
    RUN_TEST(test_sync_failure_keeps_accounted_row);
    RUN_TEST(test_capacity_failure_stops_recording);
    RUN_TEST(test_remove_failure_sets_error_and_retry);
    RUN_TEST(test_boot_recovers_complete_run);
    RUN_TEST(test_boot_recovers_interrupted_prefix);
    RUN_TEST(test_boot_footer_must_be_valid_last_line);
    RUN_TEST(test_recovery_rejects_post_terminal_rows_and_inconsistent_end_time);
    RUN_TEST(test_corrupt_saved_run_blocks_and_erase_zero);
    RUN_TEST(test_no_overwrite_and_reader_busy_guards);
    RUN_TEST(test_format_guards_and_full_erase);
    RUN_TEST(test_mount_failure_and_format_recovery);
    RUN_TEST(test_start_requires_capacity);
    RUN_TEST(test_download_bounded_and_read_error);
    UNITY_END();
}

void loop() {}

#if defined(PYROVUE_NATIVE)
int main() {
    setup();
    return Unity.TestFailures == 0 ? 0 : 1;
}
#endif
