// Behavioral tests for the portable run recorder (lib/RunLog/RunRecorder.cpp).
//
// The tests drive the REAL selection core with synthetic TempSample streams
// and assert the exact emitted LoggedSample records: 2s cadence from the first
// sample, fault/intervalFault retention, acquisition-gap flags, no catch-up
// interpolation, terminal finalization, run isolation and the 24h window.
//
// Run: pio test -e native -f test_recorder   (from PyroVue/)

#include <math.h>
#include <string.h>
#include <unity.h>

#include <stdint.h>

#include "RunLogTypes.h"
#include "RunRecorder.h"
#include "protocol.h"

void setUp() {}
void tearDown() {}

namespace {

TempSample sample(uint32_t runId, uint32_t seq, uint32_t ms, float tempC, uint8_t fault) {
    TempSample s;
    s.runId = runId;
    s.seq = seq;
    s.ms = ms;
    s.tempC = tempC;
    s.fault = fault;
    return s;
}

float fromBits(uint32_t bits) {
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

uint32_t toBits(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

void assertRecord(const LoggedSample& r, uint32_t logSeq, uint32_t acqSeq, uint32_t ms,
                  uint32_t tempBits, uint8_t fault, uint8_t intervalFault, uint8_t flags) {
    TEST_ASSERT_EQUAL_UINT32(logSeq, r.logSeq);
    TEST_ASSERT_EQUAL_UINT32(acqSeq, r.acqSeq);
    TEST_ASSERT_EQUAL_UINT32(ms, r.ms);
    TEST_ASSERT_EQUAL_UINT32(tempBits, toBits(r.tempC));
    TEST_ASSERT_EQUAL_UINT8(fault, r.fault);
    TEST_ASSERT_EQUAL_UINT8(intervalFault, r.intervalFault);
    TEST_ASSERT_EQUAL_UINT8(flags, r.flags);
}

}  // namespace

// -------------------------------------------------------------- cadence ---

static void test_cadence_first_immediate_then_log_interval() {
    RunRecorder rec;
    rec.reset(7);
    LoggedSample out;

    // Odd acquisitions carry the faults that must survive selection exactly.
    TEST_ASSERT_TRUE(rec.select(sample(7, 1, 1000, 23.5f, SENSOR_FAULT_OPEN), out));
    assertRecord(out, 1, 1, 1000, toBits(23.5f), SENSOR_FAULT_OPEN, SENSOR_FAULT_OPEN, 0);
    TEST_ASSERT_FALSE(rec.select(sample(7, 2, 2000, 23.6f, SENSOR_FAULT_SHORT_GND), out));
    TEST_ASSERT_TRUE(rec.select(sample(7, 3, 3000, 23.7f, SENSOR_FAULT_SHORT_VCC), out));
    // intervalFault ORs faults since the previous record, current included.
    assertRecord(out, 2, 3, 3000, toBits(23.7f), SENSOR_FAULT_SHORT_VCC,
                 SENSOR_FAULT_SHORT_GND | SENSOR_FAULT_SHORT_VCC, 0);
    TEST_ASSERT_FALSE(rec.select(sample(7, 4, 4000, 23.8f, 0), out));
    TEST_ASSERT_TRUE(rec.select(sample(7, 5, 5000, 23.9f, SENSOR_FAULT_OPEN | SENSOR_FAULT_NAN), out));
    assertRecord(out, 3, 5, 5000, toBits(23.9f), SENSOR_FAULT_OPEN | SENSOR_FAULT_NAN,
                 SENSOR_FAULT_OPEN | SENSOR_FAULT_NAN, 0);
    TEST_ASSERT_FALSE(rec.select(sample(7, 6, 6000, 24.0f, SENSOR_FAULT_NAN), out));

    TEST_ASSERT_TRUE(rec.finish(out));  // latest (acq 6) was never selected
    assertRecord(out, 4, 6, 6000, toBits(24.0f), SENSOR_FAULT_NAN, SENSOR_FAULT_NAN,
                 runlog::kTerminal);
    TEST_ASSERT_FALSE(rec.finish(out));
}

// ------------------------------------------------------------------ gaps ---

static void test_gap_flag_on_next_record_despite_normal_log_dt() {
    RunRecorder rec;
    rec.reset(9);
    LoggedSample out;

    TEST_ASSERT_TRUE(rec.select(sample(9, 1, 1000, 10.0f, 0), out));
    assertRecord(out, 1, 1, 1000, toBits(10.0f), 0, 0, 0);
    TEST_ASSERT_FALSE(rec.select(sample(9, 2, 2000, 11.0f, 0), out));
    // Acquired dt 2000 is a hole; the record is due with recorded dt 3000.
    TEST_ASSERT_TRUE(rec.select(sample(9, 3, 4000, 13.0f, 0), out));
    assertRecord(out, 2, 3, 4000, toBits(13.0f), 0, 0, runlog::kAcquisitionGap);
    // The flag rides on exactly the next record, then clears.
    TEST_ASSERT_FALSE(rec.select(sample(9, 4, 5000, 14.0f, 0), out));
    TEST_ASSERT_TRUE(rec.select(sample(9, 5, 6000, 15.0f, 0), out));
    assertRecord(out, 3, 5, 6000, toBits(15.0f), 0, 0, 0);
    TEST_ASSERT_FALSE(rec.finish(out));  // latest already selected
}

static void test_gap_threshold_exact_and_beyond() {
    RunRecorder rec;
    rec.reset(12);
    LoggedSample out;

    TEST_ASSERT_TRUE(rec.select(sample(12, 1, 1000, 20.0f, 0), out));
    assertRecord(out, 1, 1, 1000, toBits(20.0f), 0, 0, 0);
    // Exactly 1500ms is not a hole.
    TEST_ASSERT_FALSE(rec.select(sample(12, 2, 2500, 21.0f, 0), out));
    TEST_ASSERT_TRUE(rec.select(sample(12, 3, 4000, 22.0f, 0), out));
    assertRecord(out, 2, 3, 4000, toBits(22.0f), 0, 0, 0);
    TEST_ASSERT_FALSE(rec.select(sample(12, 4, 5500, 23.0f, 0), out));
    // 1501ms is a hole and flags the record it pushes out.
    TEST_ASSERT_TRUE(rec.select(sample(12, 5, 7001, 24.0f, 0), out));
    assertRecord(out, 3, 5, 7001, toBits(24.0f), 0, 0, runlog::kAcquisitionGap);
    TEST_ASSERT_FALSE(rec.finish(out));
}

static void test_jitter_cadence_never_flags() {
    RunRecorder rec;
    rec.reset(13);
    uint32_t ms = 1000;
    uint32_t records = 0;

    // Twelve readings alternating 900/1100ms apart: accumulated timing drift
    // never crosses the hole threshold and never breaks the 2s cadence.
    for (uint32_t seq = 1; seq <= 12; ++seq) {
        if (seq > 1) ms += (seq % 2 == 0) ? 900 : 1100;
        LoggedSample out;
        if (!rec.select(sample(13, seq, ms, 20.0f + seq, 0), out)) continue;
        ++records;
        TEST_ASSERT_EQUAL_UINT32(records, out.logSeq);
        TEST_ASSERT_EQUAL_UINT8(0, out.flags);
        TEST_ASSERT_EQUAL_UINT32(seq, out.acqSeq);
        TEST_ASSERT_EQUAL_UINT32(ms, out.ms);
    }
    TEST_ASSERT_EQUAL_UINT32(6, records);  // 1000,3000,5000,7000,9000,11000

    LoggedSample out;
    TEST_ASSERT_TRUE(rec.finish(out));
    assertRecord(out, 7, 12, 11900, toBits(32.0f), 0, 0, runlog::kTerminal);
}

static void test_identity_jump_consumed_without_invented_flag() {
    RunRecorder rec;
    rec.reset(14);
    LoggedSample out;

    TEST_ASSERT_TRUE(rec.select(sample(14, 1, 1000, 1.0f, 0), out));
    // An identity jump with tight timing is a valid acquisition; hole
    // detection is acquired-dt based, so no flag is invented here.
    TEST_ASSERT_FALSE(rec.select(sample(14, 5, 2000, 2.0f, 0), out));
    TEST_ASSERT_TRUE(rec.select(sample(14, 6, 3000, 3.0f, 0), out));
    assertRecord(out, 2, 6, 3000, toBits(3.0f), 0, 0, 0);
}

static void test_no_catchup_records_after_long_hole() {
    RunRecorder rec;
    rec.reset(21);
    LoggedSample out;

    TEST_ASSERT_TRUE(rec.select(sample(21, 1, 1000, 5.0f, 0), out));
    assertRecord(out, 1, 1, 1000, toBits(5.0f), 0, 0, 0);
    // 6000ms hole: exactly ONE record at the real acquisition (nothing
    // invented at 3000/5000) with the gap flag and no invented sensor fault.
    TEST_ASSERT_TRUE(rec.select(sample(21, 2, 7000, 9.0f, 0), out));
    assertRecord(out, 2, 2, 7000, toBits(9.0f), 0, 0, runlog::kAcquisitionGap);
    // Cadence re-anchors at 7000 with no extra records.
    TEST_ASSERT_FALSE(rec.select(sample(21, 3, 8000, 10.0f, 0), out));
    TEST_ASSERT_TRUE(rec.select(sample(21, 4, 9000, 11.0f, 0), out));
    assertRecord(out, 3, 4, 9000, toBits(11.0f), 0, 0, 0);
    TEST_ASSERT_FALSE(rec.finish(out));
}

// ---------------------------------------------------------- finalization ---

static void test_finish_terminal_only_when_latest_unselected() {
    // Latest already selected -> finish closes without a duplicate record.
    RunRecorder rec;
    rec.reset(22);
    LoggedSample out;
    TEST_ASSERT_TRUE(rec.select(sample(22, 1, 1000, 5.0f, 0), out));
    assertRecord(out, 1, 1, 1000, toBits(5.0f), 0, 0, 0);
    TEST_ASSERT_FALSE(rec.select(sample(22, 2, 2000, 5.5f, 0), out));
    TEST_ASSERT_TRUE(rec.select(sample(22, 3, 3000, 6.0f, 0), out));
    assertRecord(out, 2, 3, 3000, toBits(6.0f), 0, 0, 0);
    TEST_ASSERT_FALSE(rec.finish(out));
    TEST_ASSERT_FALSE(rec.select(sample(22, 4, 4000, 7.0f, 0), out));  // closed

    // Terminal takes the retained latest exactly once.
    RunRecorder rec2;
    rec2.reset(23);
    TEST_ASSERT_TRUE(rec2.select(sample(23, 1, 1000, 5.0f, 0), out));
    TEST_ASSERT_FALSE(rec2.select(sample(23, 2, 2000, 6.0f, 7), out));
    TEST_ASSERT_TRUE(rec2.finish(out));
    assertRecord(out, 2, 2, 2000, toBits(6.0f), 7, 7, runlog::kTerminal);
    TEST_ASSERT_FALSE(rec2.finish(out));                                 // idempotent
    TEST_ASSERT_FALSE(rec2.select(sample(23, 3, 4000, 8.0f, 0), out));   // closed
    TEST_ASSERT_FALSE(rec2.finish(out));
}

static void test_reset_isolates_runs_and_resets_selection() {
    RunRecorder rec;
    rec.reset(31);
    LoggedSample out;
    TEST_ASSERT_TRUE(rec.select(sample(31, 1, 1000, 1.0f, SENSOR_FAULT_OPEN), out));
    assertRecord(out, 1, 1, 1000, toBits(1.0f), SENSOR_FAULT_OPEN, SENSOR_FAULT_OPEN, 0);
    // A pending hole, accumulated fault and unselected latest all live here.
    TEST_ASSERT_FALSE(rec.select(sample(31, 2, 2600, 2.0f, SENSOR_FAULT_SHORT_GND), out));

    rec.reset(32);
    TEST_ASSERT_FALSE(rec.select(sample(31, 3, 3000, 3.0f, 0xFF), out));  // old run excluded
    TEST_ASSERT_TRUE(rec.select(sample(32, 1, 100, 4.0f, 0), out));
    assertRecord(out, 1, 1, 100, toBits(4.0f), 0, 0, 0);  // fresh logSeq, nothing carried
    TEST_ASSERT_FALSE(rec.select(sample(32, 2, 1100, 5.0f, 0), out));
    TEST_ASSERT_TRUE(rec.select(sample(32, 3, 2200, 6.0f, 0), out));
    assertRecord(out, 2, 3, 2200, toBits(6.0f), 0, 0, 0);  // old pending hole did not survive
    TEST_ASSERT_FALSE(rec.finish(out));                    // latest already selected

    // Repeated reset re-arms the same run id from logSeq 1 as well.
    rec.reset(32);
    TEST_ASSERT_TRUE(rec.select(sample(32, 1, 50, 7.0f, SENSOR_FAULT_NAN), out));
    assertRecord(out, 1, 1, 50, toBits(7.0f), SENSOR_FAULT_NAN, SENSOR_FAULT_NAN, 0);
    TEST_ASSERT_FALSE(rec.select(sample(32, 2, 100, 8.0f, 0), out));
    TEST_ASSERT_TRUE(rec.finish(out));
    assertRecord(out, 2, 2, 100, toBits(8.0f), 0, 0, runlog::kTerminal);
}

// --------------------------------------------------------------- filters ---

static void test_filters_exclude_without_corrupting_selection() {
    RunRecorder rec;  // default-constructed stays safely idle
    LoggedSample out;
    const TempSample idle = sample(0, 0, 0, 0.0f, 0);
    TEST_ASSERT_FALSE(rec.select(idle, out));
    TEST_ASSERT_FALSE(rec.select(sample(40, 1, 1000, 1.0f, 0), out));
    TEST_ASSERT_FALSE(rec.finish(out));

    rec.reset(40);
    TEST_ASSERT_FALSE(rec.select(idle, out));                          // idle sample
    TEST_ASSERT_FALSE(rec.select(sample(39, 1, 1000, 1.0f, 0), out));  // foreign run
    TEST_ASSERT_FALSE(rec.select(sample(41, 1, 1000, 1.0f, 0), out));  // foreign run
    TEST_ASSERT_FALSE(rec.select(sample(40, 0, 1000, 1.0f, 0), out));  // no seq
    TEST_ASSERT_TRUE(rec.select(sample(40, 1, 1000, 1.0f, 0), out));
    assertRecord(out, 1, 1, 1000, toBits(1.0f), 0, 0, 0);

    // Duplicates and non-increasing identities/timestamps are dropped and
    // never advance the consumed stream (identity checks compare against the
    // last consumed acquisition).
    TEST_ASSERT_FALSE(rec.select(sample(40, 1, 1000, 9.0f, 0x0F), out));  // duplicate
    TEST_ASSERT_FALSE(rec.select(sample(40, 1, 1100, 9.0f, 0x0F), out));  // duplicate identity
    TEST_ASSERT_FALSE(rec.select(sample(40, 0, 1200, 9.0f, 0x0F), out));  // no seq
    TEST_ASSERT_FALSE(rec.select(sample(40, 2, 1000, 9.0f, 0x0F), out));  // repeat timestamp
    TEST_ASSERT_FALSE(rec.select(sample(40, 3, 500, 9.0f, 0x0F), out));   // timestamp went back
    TEST_ASSERT_FALSE(rec.select(sample(39, 9, 99999, 9.0f, 0x0F), out)); // foreign
    TEST_ASSERT_FALSE(rec.select(sample(41, 9, 99999, 9.0f, 0x0F), out)); // foreign

    // The filtered traffic leaves cadence, holes and intervalFault untouched:
    // junk ms values and junk faults never reach the record stream.
    TEST_ASSERT_FALSE(rec.select(sample(40, 2, 1900, 2.0f, 0), out));
    TEST_ASSERT_TRUE(rec.select(sample(40, 3, 3000, 3.0f, SENSOR_FAULT_NAN), out));
    assertRecord(out, 2, 3, 3000, toBits(3.0f), SENSOR_FAULT_NAN, SENSOR_FAULT_NAN, 0);
}

// ------------------------------------------------------- duration window ---

static void test_duration_boundary_never_records_beyond_window() {
    RunRecorder rec;
    rec.reset(51);
    LoggedSample out;
    TEST_ASSERT_TRUE(rec.select(sample(51, 1, runlog::kMaxDurationMs - 2000, 20.0f, 0), out));
    assertRecord(out, 1, 1, runlog::kMaxDurationMs - 2000, toBits(20.0f), 0, 0, 0);
    TEST_ASSERT_FALSE(rec.select(sample(51, 2, runlog::kMaxDurationMs - 500, 21.0f, 0), out));
    // Out-of-window acquisitions never become records, never displace the
    // retained latest and never contribute faults.
    TEST_ASSERT_FALSE(rec.select(
        sample(51, 3, runlog::kMaxDurationMs + 1, NAN, SENSOR_FAULT_OPEN | SENSOR_FAULT_NAN), out));
    TEST_ASSERT_FALSE(rec.select(sample(51, 4, runlog::kMaxDurationMs + 2000, 22.0f, 0xFF), out));
    TEST_ASSERT_TRUE(rec.finish(out));
    assertRecord(out, 2, 2, runlog::kMaxDurationMs - 500, toBits(21.0f), 0, 0, runlog::kTerminal);

    // The boundary itself is in range and may be the terminal record.
    RunRecorder rec2;
    rec2.reset(52);
    TEST_ASSERT_TRUE(rec2.select(sample(52, 1, runlog::kMaxDurationMs - 1000, 30.0f, 0), out));
    TEST_ASSERT_FALSE(rec2.select(sample(52, 2, runlog::kMaxDurationMs, 31.0f, 0), out));
    TEST_ASSERT_FALSE(rec2.select(sample(52, 3, runlog::kMaxDurationMs + 500, 32.0f, 0), out));
    TEST_ASSERT_TRUE(rec2.finish(out));
    assertRecord(out, 2, 2, runlog::kMaxDurationMs, toBits(31.0f), 0, 0, runlog::kTerminal);
}

static void test_full_day_virtual_count_within_budget() {
    RunRecorder rec;
    rec.reset(101);
    uint32_t records = 0;

    // 24h at the 1s acquire cadence: first sample immediate, then every 2s.
    for (uint32_t seq = 1; seq <= 86400; ++seq) {
        LoggedSample out;
        if (!rec.select(sample(101, seq, seq * 1000, 5.0f, 0), out)) continue;
        ++records;
        TEST_ASSERT_EQUAL_UINT32(records, out.logSeq);
        TEST_ASSERT_EQUAL_UINT32(seq, out.acqSeq);
        TEST_ASSERT_TRUE(out.ms <= runlog::kMaxDurationMs);  // never beyond boundary
        if (records == 1) {
            TEST_ASSERT_EQUAL_UINT32(1000, out.ms);
        } else if (records == 43200) {
            TEST_ASSERT_EQUAL_UINT32(86399000, out.ms);
        }
    }
    TEST_ASSERT_EQUAL_UINT32(43200, records);

    LoggedSample out;
    TEST_ASSERT_TRUE(rec.finish(out));  // latest (86400000) not selected -> terminal
    ++records;
    assertRecord(out, 43201, 86400, runlog::kMaxDurationMs, toBits(5.0f), 0, 0, runlog::kTerminal);
    TEST_ASSERT_EQUAL_UINT32(43201, records);  // 43200 interval + 1 terminal
    TEST_ASSERT_TRUE(records <= 43202);        // 43200 + first + terminal budget
}

// ---------------------------------------------------------- float bits ---

static void test_nan_and_negative_zero_bits_preserved() {
    const uint32_t negZeroBits = 0x80000000u;
    const uint32_t nanPayloadBits = 0x7FC00001u;
    const uint32_t negNanBits = 0xFFC00002u;

    RunRecorder rec;
    rec.reset(61);
    LoggedSample out;
    TEST_ASSERT_TRUE(rec.select(sample(61, 1, 1000, fromBits(negZeroBits), 0), out));
    TEST_ASSERT_EQUAL_UINT32(negZeroBits, toBits(out.tempC));
    TEST_ASSERT_FALSE(rec.select(sample(61, 2, 1500, fromBits(nanPayloadBits), 0), out));
    TEST_ASSERT_TRUE(rec.select(sample(61, 3, 3000, fromBits(negNanBits), 0), out));
    TEST_ASSERT_EQUAL_UINT32(negNanBits, toBits(out.tempC));
    TEST_ASSERT_FALSE(rec.select(sample(61, 4, 3500, fromBits(nanPayloadBits), 0), out));
    TEST_ASSERT_TRUE(rec.finish(out));
    TEST_ASSERT_EQUAL_UINT32(nanPayloadBits, toBits(out.tempC));  // terminal keeps exact bits
}

void setup() {
    UNITY_BEGIN();
    RUN_TEST(test_cadence_first_immediate_then_log_interval);
    RUN_TEST(test_gap_flag_on_next_record_despite_normal_log_dt);
    RUN_TEST(test_gap_threshold_exact_and_beyond);
    RUN_TEST(test_jitter_cadence_never_flags);
    RUN_TEST(test_identity_jump_consumed_without_invented_flag);
    RUN_TEST(test_no_catchup_records_after_long_hole);
    RUN_TEST(test_finish_terminal_only_when_latest_unselected);
    RUN_TEST(test_reset_isolates_runs_and_resets_selection);
    RUN_TEST(test_filters_exclude_without_corrupting_selection);
    RUN_TEST(test_duration_boundary_never_records_beyond_window);
    RUN_TEST(test_full_day_virtual_count_within_budget);
    RUN_TEST(test_nan_and_negative_zero_bits_preserved);
    UNITY_END();
}

void loop() {}

#if defined(PYROVUE_NATIVE)
int main() {
    setup();
    return Unity.TestFailures == 0 ? 0 : 1;
}
#endif
