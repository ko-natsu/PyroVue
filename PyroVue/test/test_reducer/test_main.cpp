// Behavioral tests for the marker reducer (lib/RunLog/MarkerReducer).
//
// The tests drive the REAL reducer over byte-accurate PVE1 journals built
// with EventJournalCodec and JSON text payloads: bounded incremental ingest,
// frame-boundary prefix extension, marker lifecycle/notes-after-end, exact
// original operation lookups after later edits, Unicode/limit handling,
// real hash-collision resolution, and every rejection class (lifecycle,
// revision, base transitions, duplicate operations, missing fields, bad
// numeric types, CRC, truncated frames, invalid escaped Unicode, NUL) with
// the previous valid prefix preserved and errors latched.
//
// Run: pio test -e native -f test_reducer   (from PyroVue/)

#include <string.h>
#include <unity.h>

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

#include "EventJournalCodec.h"
#include "MarkerReducer.h"

void setUp() {}
void tearDown() {}

namespace {

constexpr uint32_t kRunId = 0x00C0FFEEu;  // distinctive header identity

std::string num(uint32_t v) { return std::to_string(v); }

// JSON string quoting for fixture text: escapes quote/backslash/control
// characters and passes raw UTF-8 bytes through unchanged.
std::string jsonQuote(const std::string& raw) {
  std::string out = "\"";
  for (size_t i = 0; i < raw.size(); ++i) {
    const char c = raw[i];
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out += c;
        break;
    }
  }
  out += "\"";
  return out;
}

std::string started(uint32_t revision, uint32_t ms = 0,
                    const std::string& requestId = "req-start") {
  return "{\"revision\":" + num(revision) +
         ",\"type\":\"run.started\",\"requestId\":" + jsonQuote(requestId) +
         ",\"ms\":" + num(ms) +
         ",\"status\":\"active\",\"errorCode\":null,\"droppedAcquired\":0}";
}

std::string ended(uint32_t revision, uint32_t ms = 100,
                  const std::string& status = "complete",
                  const std::string& errorCodeJson = "null",
                  uint32_t dropped = 0) {
  return "{\"revision\":" + num(revision) +
         ",\"type\":\"run.ended\",\"requestId\":\"req-stop\",\"ms\":" + num(ms) +
         ",\"status\":" + jsonQuote(status) + ",\"errorCode\":" + errorCodeJson +
         ",\"droppedAcquired\":" + num(dropped) + "}";
}

std::string createdJson(uint32_t revision, const std::string& op,
                    const std::string& id, const std::string& msJson,
                    const std::string& noteJson, const std::string& timeBasis,
                    const std::string& baseRevision = "0") {
  return "{\"revision\":" + num(revision) +
         ",\"type\":\"marker.created\",\"operationId\":" + jsonQuote(op) +
         ",\"id\":" + jsonQuote(id) + ",\"baseRevision\":" + baseRevision +
         ",\"ms\":" + msJson + ",\"note\":" + noteJson +
         ",\"timeBasis\":" + jsonQuote(timeBasis) + "}";
}

std::string created(uint32_t revision, const std::string& op,
                    const std::string& id, const std::string& msJson,
                    const std::string& note, const std::string& timeBasis,
                    const std::string& baseRevision = "0") {
  return createdJson(revision, op, id, msJson, jsonQuote(note), timeBasis, baseRevision);
}

std::string updated(uint32_t revision, const std::string& op,
                    const std::string& id, const std::string& baseRevision,
                    const std::string& msJson, const std::string& note,
                    const std::string& timeBasis) {
  return "{\"revision\":" + num(revision) +
         ",\"type\":\"marker.updated\",\"operationId\":" + jsonQuote(op) +
         ",\"id\":" + jsonQuote(id) + ",\"baseRevision\":" + baseRevision +
         ",\"ms\":" + msJson + ",\"note\":" + jsonQuote(note) +
         ",\"timeBasis\":" + jsonQuote(timeBasis) + "}";
}

std::string deleted(uint32_t revision, const std::string& op,
                    const std::string& id, const std::string& baseRevision) {
  return "{\"revision\":" + num(revision) +
         ",\"type\":\"marker.deleted\",\"operationId\":" + jsonQuote(op) +
         ",\"id\":" + jsonQuote(id) + ",\"baseRevision\":" + baseRevision + "}";
}

// Real binary framing: PVE1 header + CRC-framed UTF-8 JSON payloads.
// frameEnds[i] is the offset just after frame i (a legal prefix boundary).
std::vector<uint8_t> journal(uint32_t runId,
                             const std::vector<std::string>& payloads,
                             std::vector<size_t>* frameEnds = nullptr) {
  std::vector<uint8_t> bytes(eventjournalcodec::kHeaderSize);
  eventjournalcodec::Header header;
  header.runId = runId;
  TEST_ASSERT_EQUAL_UINT32(
      eventjournalcodec::kHeaderSize,
      eventjournalcodec::encodeHeader(bytes.data(), bytes.size(), header));
  for (size_t i = 0; i < payloads.size(); ++i) {
    const std::string& payload = payloads[i];
    const size_t at = bytes.size();
    bytes.resize(at + eventjournalcodec::kFrameOverheadBytes + payload.size());
    const size_t n = eventjournalcodec::encodeFrame(
        bytes.data() + at, bytes.size() - at,
        reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
    TEST_ASSERT_TRUE(n > 0);
    bytes.resize(at + n);
    if (frameEnds != nullptr) {
      frameEnds->push_back(bytes.size());
    }
  }
  return bytes;
}

// Reduces the whole claimed prefix in bounded steps (8 frames per call).
ArchiveError reduceFully(MarkerReducer& reducer, size_t maxPer = 8) {
  for (int guard = 0; guard < 100000; ++guard) {
    if (reducer.ready()) {
      return ArchiveError::NONE;
    }
    const ArchiveError err = reducer.ingestNext(maxPer);
    if (err != ArchiveError::NONE) {
      return err;
    }
  }
  TEST_FAIL_MESSAGE("ingestNext made no progress while not ready");
  return ArchiveError::NONE;
}

void assertError(ArchiveError expected, ArchiveError actual) {
  TEST_ASSERT_EQUAL_INT(static_cast<int>(expected), static_cast<int>(actual));
}

// Builds a journal whose last payload is invalid and asserts the reducer
// rejects it without publishing: the previous valid prefix's revision and
// annotation count stay intact, readiness stays false and the error latches.
void expectRejected(uint32_t runId, const std::vector<std::string>& payloads,
                    uint32_t expectedRevision, uint32_t expectedCount) {
  MarkerReducer reducer;
  const std::vector<uint8_t> bytes = journal(runId, payloads);
  reducer.reset(bytes.data(), bytes.size());
  assertError(ArchiveError::CORRUPT_RUN, reduceFully(reducer));
  TEST_ASSERT_FALSE(reducer.ready());
  TEST_ASSERT_EQUAL_UINT32(expectedRevision, reducer.revision());
  TEST_ASSERT_EQUAL_UINT32(expectedCount, reducer.annotationCount());
  // Latched: further progress attempts report the same error without moving.
  assertError(ArchiveError::CORRUPT_RUN, reducer.ingestNext(8));
  TEST_ASSERT_EQUAL_UINT32(expectedRevision, reducer.revision());
  TEST_ASSERT_EQUAL_UINT32(expectedCount, reducer.annotationCount());
}

// Must match idHash() in MarkerReducer.cpp (documented index contract:
// FNV-1a 32 over the decoded id bytes; slot = hash & 4095, hash >> 18 kept).
uint32_t idHash(const char* id) {
  uint32_t h = 2166136261u;
  for (const uint8_t* p = reinterpret_cast<const uint8_t*>(id); *p; ++p) {
    h ^= *p;
    h *= 16777619u;
  }
  return h;
}

// Finds two distinct short ids whose index entries are indistinguishable
// (same probe slot and same stored hash bits), forcing the reducer's decoded
// id comparison to resolve the collision.
void findCollidingIds(std::string& a, std::string& b) {
  static const char kChars[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-";
  const size_t alphabet = strlen(kChars);
  std::unordered_map<uint64_t, std::string> seen;
  std::string id;
  for (int len = 1; len <= 3 && a.empty(); ++len) {
    id.assign(static_cast<size_t>(len), kChars[0]);
    for (;;) {
      const uint32_t h = idHash(id.c_str());
      const uint64_t key =
          static_cast<uint64_t>(h & 0xFFFu) |
          (static_cast<uint64_t>((h >> 18) & 0x3FFFu) << 12);
      const auto it = seen.find(key);
      if (it != seen.end() && it->second != id) {
        a = it->second;
        b = id;
        return;
      }
      seen.emplace(key, id);
      size_t pos = 0;
      for (; pos < id.size(); ++pos) {
        const size_t index = strchr(kChars, id[pos]) - kChars;
        if (index + 1 < alphabet) {
          id[pos] = kChars[index + 1];
          break;
        }
        id[pos] = kChars[0];
      }
      if (pos == id.size()) {
        break;
      }
    }
  }
}

void fillBytes(void* target, size_t bytes, uint8_t pattern) {
  memset(target, pattern, bytes);
}

}  // namespace

// -------------------------------------------------- ingest mechanics ------

static void test_ingest_is_bounded_per_call() {
  // 1 started + 25 created + 1 ended + 1 deleted = 28 frames.
  std::vector<std::string> payloads = {started(1)};
  for (uint32_t i = 0; i < 25; ++i) {
    payloads.push_back(created(2 + i, "op-" + num(i), "m-" + num(i), "5",
                               "note " + num(i), "estimated"));
  }
  payloads.push_back(ended(27));
  payloads.push_back(deleted(28, "op-del", "m-0", "2"));

  MarkerReducer reducer;
  const std::vector<uint8_t> bytes = journal(kRunId, payloads);
  reducer.reset(bytes.data(), bytes.size());
  TEST_ASSERT_FALSE(reducer.ready());

  // maxFrames 0 makes no progress at all.
  assertError(ArchiveError::NONE, reducer.ingestNext(0));
  TEST_ASSERT_EQUAL_UINT32(0, reducer.revision());
  TEST_ASSERT_EQUAL_UINT32(0, reducer.annotationCount());
  TEST_ASSERT_FALSE(reducer.ready());

  // Bounded to min(maxFrames, 8); the header is not a frame step.
  assertError(ArchiveError::NONE, reducer.ingestNext(8));
  TEST_ASSERT_EQUAL_UINT32(8, reducer.revision());
  assertError(ArchiveError::NONE, reducer.ingestNext(8));
  TEST_ASSERT_EQUAL_UINT32(16, reducer.revision());
  assertError(ArchiveError::NONE, reducer.ingestNext(1));
  TEST_ASSERT_EQUAL_UINT32(17, reducer.revision());
  assertError(ArchiveError::NONE, reducer.ingestNext(20));
  TEST_ASSERT_EQUAL_UINT32(25, reducer.revision());
  TEST_ASSERT_FALSE(reducer.ready());
  assertError(ArchiveError::NONE, reducer.ingestNext(8));
  TEST_ASSERT_TRUE(reducer.ready());
  TEST_ASSERT_EQUAL_UINT32(28, reducer.revision());
  TEST_ASSERT_EQUAL_UINT32(24, reducer.annotationCount());

  // Already reduced: further calls are quiet no-ops.
  assertError(ArchiveError::NONE, reducer.ingestNext(8));
  TEST_ASSERT_EQUAL_UINT32(28, reducer.revision());
}

static void test_prefix_extension_reduces_appended_frames() {
  std::vector<size_t> frameEnds;
  const std::vector<std::string> payloads = {
      started(1),
      created(2, "op-a", "m1", "10", "first", "estimated"),
      created(3, "op-b", "m2", "null", "late", "untimed"),
      updated(4, "op-c", "m1", "2", "20", "second", "manual"),
      ended(5)};
  const std::vector<uint8_t> bytes = journal(kRunId, payloads, &frameEnds);

  MarkerReducer reducer;
  reducer.reset(bytes.data(), frameEnds[1]);  // header + 2 frames
  assertError(ArchiveError::NONE, reduceFully(reducer));
  TEST_ASSERT_TRUE(reducer.ready());
  TEST_ASSERT_EQUAL_UINT32(2, reducer.revision());
  TEST_ASSERT_EQUAL_UINT32(1, reducer.annotationCount());

  // Extension claims more of the SAME immutable bytes: not ready until the
  // appended frames are reduced.
  assertError(ArchiveError::NONE, reducer.extendValidatedPrefix(frameEnds[3]));
  TEST_ASSERT_FALSE(reducer.ready());
  assertError(ArchiveError::NONE, reducer.ingestNext(8));
  TEST_ASSERT_TRUE(reducer.ready());
  TEST_ASSERT_EQUAL_UINT32(4, reducer.revision());
  TEST_ASSERT_EQUAL_UINT32(2, reducer.annotationCount());

  MarkerState state;
  TEST_ASSERT_TRUE(reducer.lookupMarker("m1", state));
  TEST_ASSERT_EQUAL_STRING("second", state.note);
  TEST_ASSERT_EQUAL_UINT32(4, state.revision);

  assertError(ArchiveError::NONE, reducer.extendValidatedPrefix(bytes.size()));
  assertError(ArchiveError::NONE, reduceFully(reducer));
  TEST_ASSERT_EQUAL_UINT32(5, reducer.revision());
}

static void test_chunked_cache_load_with_frame_boundaries() {
  // Simulates the store's bounded cache reads: chunks may end mid-frame, so
  // the claimed prefix may only grow to complete frame boundaries while
  // ingest steps run in between.
  std::vector<size_t> frameEnds;
  std::vector<std::string> payloads = {started(1)};
  for (uint32_t i = 0; i < 60; ++i) {
    payloads.push_back(created(2 + i, "op-" + num(i), "m-" + num(i), "1000",
                               "note " + num(i), "estimated"));
  }
  payloads.push_back(ended(62));
  const std::vector<uint8_t> bytes = journal(kRunId, payloads, &frameEnds);

  MarkerReducer reducer;
  reducer.reset(bytes.data(), 0);
  size_t fileRead = 0;
  size_t claimed = 0;
  size_t nextBoundary = 0;
  int guard = 0;
  while (fileRead < bytes.size() || !reducer.ready()) {
    TEST_ASSERT_TRUE_MESSAGE(++guard < 10000, "load loop stalled");
    if (fileRead < bytes.size()) {
      fileRead = std::min(bytes.size(), fileRead + 700);
      size_t claim = claimed;
      while (nextBoundary < frameEnds.size() &&
             frameEnds[nextBoundary] <= fileRead) {
        claim = frameEnds[nextBoundary];
        ++nextBoundary;
      }
      if (claim > claimed) {
        assertError(ArchiveError::NONE,
                    reducer.extendValidatedPrefix(claim));
        claimed = claim;
      }
    }
    if (fileRead >= bytes.size() || !reducer.ready()) {
      assertError(ArchiveError::NONE, reducer.ingestNext(8));
    }
  }
  TEST_ASSERT_EQUAL_UINT32(62, reducer.revision());
  TEST_ASSERT_EQUAL_UINT32(60, reducer.annotationCount());
  MarkerState state;
  TEST_ASSERT_TRUE(reducer.lookupMarker("m-59", state));
  TEST_ASSERT_EQUAL_STRING("note 59", state.note);
}

// -------------------------------------------------------- marker flow -----

static void test_create_update_delete_counts_and_states() {
  const std::vector<std::string> payloads = {
      started(1),
      created(2, "op-a", "m1", "250", "birth", "estimated"),
      updated(3, "op-b", "m1", "2", "null", "revised", "untimed"),
      deleted(4, "op-c", "m1", "3"),
  };
  MarkerReducer reducer;
  const std::vector<uint8_t> bytes = journal(kRunId, payloads);
  reducer.reset(bytes.data(), bytes.size());
  assertError(ArchiveError::NONE, reduceFully(reducer));

  TEST_ASSERT_EQUAL_UINT32(4, reducer.revision());
  TEST_ASSERT_EQUAL_UINT32(0, reducer.annotationCount());

  MarkerState state;
  TEST_ASSERT_TRUE(reducer.lookupMarker("m1", state));
  TEST_ASSERT_TRUE(state.deleted);
  TEST_ASSERT_EQUAL_UINT32(4, state.revision);
  // Tombstones carry no invented timestamp or note.
  TEST_ASSERT_FALSE(state.hasMs);
  TEST_ASSERT_EQUAL_UINT32(0, state.ms);
  TEST_ASSERT_EQUAL_STRING("", state.note);

  // Live state before the delete is still reachable through its operation.
  MarkerMutation mutation;
  MutationResult result;
  TEST_ASSERT_TRUE(reducer.lookupOperation("op-a", mutation, result));
  TEST_ASSERT_EQUAL_STRING("birth", mutation.note);
  TEST_ASSERT_FALSE(result.deleted);
}

static void test_markers_and_notes_after_run_end() {
  const std::string note = "post-fire annotation";
  const std::vector<std::string> payloads = {
      started(1),
      ended(2),
      created(3, "op-a", "m1", "5000", note, "manual"),
      updated(4, "op-b", "m1", "3", "null", "edited after end", "untimed"),
      deleted(5, "op-c", "m1", "4"),
      created(6, "op-d", "m2", "null", "", "untimed"),
  };
  MarkerReducer reducer;
  const std::vector<uint8_t> bytes = journal(kRunId, payloads);
  reducer.reset(bytes.data(), bytes.size());
  assertError(ArchiveError::NONE, reduceFully(reducer));

  TEST_ASSERT_EQUAL_UINT32(6, reducer.revision());
  TEST_ASSERT_EQUAL_UINT32(1, reducer.annotationCount());
  MarkerState state;
  TEST_ASSERT_TRUE(reducer.lookupMarker("m2", state));
  TEST_ASSERT_FALSE(state.deleted);
  TEST_ASSERT_FALSE(state.hasMs);

  // A second run.ended is illegal even after successful marker activity.
  expectRejected(kRunId, {started(1), ended(2), ended(3, 200, "interrupted")},
                 2, 0);
}

static void test_operation_lookup_returns_exact_original() {
  const std::string spaced = "  hello \"x\"\nsecond,line  ";
  const std::vector<std::string> payloads = {
      started(1),
      created(2, "op-a", "m1", "250", spaced, "estimated"),
      updated(3, "op-b", "m1", "2", "null", "after", "untimed"),
      updated(4, "op-c", "m1", "3", "0", "", "manual"),
      deleted(5, "op-d", "m1", "4"),
  };
  MarkerReducer reducer;
  const std::vector<uint8_t> bytes = journal(kRunId, payloads);
  reducer.reset(bytes.data(), bytes.size());
  assertError(ArchiveError::NONE, reduceFully(reducer));

  MarkerMutation mutation;
  MutationResult result;

  TEST_ASSERT_TRUE(reducer.lookupOperation("op-a", mutation, result));
  TEST_ASSERT_TRUE(mutation.kind == MarkerMutationKind::Create);
  TEST_ASSERT_EQUAL_STRING("op-a", mutation.operationId);
  TEST_ASSERT_EQUAL_STRING("m1", mutation.id);
  TEST_ASSERT_EQUAL_UINT32(0, mutation.baseRevision);
  TEST_ASSERT_TRUE(mutation.hasMs);
  TEST_ASSERT_EQUAL_UINT32(250, mutation.ms);
  // The stored note is exact: whitespace and punctuation are never trimmed.
  TEST_ASSERT_EQUAL_STRING(spaced.c_str(), mutation.note);
  TEST_ASSERT_TRUE(mutation.timeBasis == TimeBasis::Estimated);
  TEST_ASSERT_EQUAL_UINT32(kRunId, result.runId);  // from the journal header
  TEST_ASSERT_EQUAL_UINT32(2, result.revision);
  TEST_ASSERT_TRUE(result.persisted);
  TEST_ASSERT_FALSE(result.deleted);

  TEST_ASSERT_TRUE(reducer.lookupOperation("op-b", mutation, result));
  TEST_ASSERT_TRUE(mutation.kind == MarkerMutationKind::Update);
  TEST_ASSERT_EQUAL_UINT32(2, mutation.baseRevision);
  TEST_ASSERT_FALSE(mutation.hasMs);
  TEST_ASSERT_EQUAL_STRING("after", mutation.note);
  TEST_ASSERT_TRUE(mutation.timeBasis == TimeBasis::Untimed);
  TEST_ASSERT_EQUAL_UINT32(3, result.revision);
  TEST_ASSERT_FALSE(result.deleted);

  TEST_ASSERT_TRUE(reducer.lookupOperation("op-c", mutation, result));
  TEST_ASSERT_TRUE(mutation.hasMs);
  TEST_ASSERT_EQUAL_UINT32(0, mutation.ms);  // zero timestamp preserved
  TEST_ASSERT_TRUE(mutation.timeBasis == TimeBasis::Manual);
  TEST_ASSERT_EQUAL_STRING("", mutation.note);
  TEST_ASSERT_EQUAL_UINT32(4, result.revision);

  TEST_ASSERT_TRUE(reducer.lookupOperation("op-d", mutation, result));
  TEST_ASSERT_TRUE(mutation.kind == MarkerMutationKind::Delete);
  TEST_ASSERT_EQUAL_UINT32(4, mutation.baseRevision);
  TEST_ASSERT_FALSE(mutation.hasMs);
  TEST_ASSERT_EQUAL_UINT32(0, mutation.ms);
  TEST_ASSERT_EQUAL_STRING("", mutation.note);
  TEST_ASSERT_TRUE(mutation.timeBasis == TimeBasis::Untimed);
  TEST_ASSERT_EQUAL_UINT32(5, result.revision);
  TEST_ASSERT_TRUE(result.persisted);
  TEST_ASSERT_TRUE(result.deleted);
}

static void test_lookup_missing_leaves_outputs_untouched() {
  const std::vector<std::string> payloads = {
      started(1),
      created(2, "op-a", "m1", "1", "keep", "estimated"),
  };
  MarkerReducer reducer;
  const std::vector<uint8_t> bytes = journal(kRunId, payloads);
  reducer.reset(bytes.data(), bytes.size());
  assertError(ArchiveError::NONE, reduceFully(reducer));

  MarkerState state;
  fillBytes(&state, sizeof(state), 0x5A);
  const MarkerState before = state;
  TEST_ASSERT_FALSE(reducer.lookupMarker("missing", state));
  TEST_ASSERT_EQUAL_MEMORY(&before, &state, sizeof(state));
  TEST_ASSERT_FALSE(reducer.lookupMarker("", state));
  TEST_ASSERT_EQUAL_MEMORY(&before, &state, sizeof(state));

  MarkerMutation mutation;
  MutationResult result;
  fillBytes(&mutation, sizeof(mutation), 0x3C);
  fillBytes(&result, sizeof(result), 0xC3);
  const MarkerMutation beforeMutation = mutation;
  const MutationResult beforeResult = result;
  TEST_ASSERT_FALSE(reducer.lookupOperation("missing", mutation, result));
  TEST_ASSERT_EQUAL_MEMORY(&beforeMutation, &mutation, sizeof(mutation));
  TEST_ASSERT_EQUAL_MEMORY(&beforeResult, &result, sizeof(result));
}

// ---------------------------------------------------- notes and limits ----

static void test_note_preserves_unicode_quotes_newlines() {
  const std::string emoji = "\xF0\x9F\x98\x80";  // U+1F600
  const std::string note = "  he said \"hi\"\nline2\ttab,cjk:\xE6\x97\xA5\xE6\x9C\xAC " + emoji + "  ";
  const std::vector<std::string> payloads = {
      started(1),
      created(2, "op-a", "m1", "7", note, "estimated"),
  };
  MarkerReducer reducer;
  const std::vector<uint8_t> bytes = journal(kRunId, payloads);
  reducer.reset(bytes.data(), bytes.size());
  assertError(ArchiveError::NONE, reduceFully(reducer));

  MarkerState state;
  TEST_ASSERT_TRUE(reducer.lookupMarker("m1", state));
  TEST_ASSERT_EQUAL_STRING(note.c_str(), state.note);

  // Escaped ASCII decodes to the same stored text as its literal form.
  const std::vector<std::string> escaped = {
      started(1),
      createdJson(2, "op-a", "m1", "7", "\"\\u0041\\u0042\"", "estimated"),
  };
  MarkerReducer reducer2;
  const std::vector<uint8_t> bytes2 = journal(kRunId, escaped);
  reducer2.reset(bytes2.data(), bytes2.size());
  assertError(ArchiveError::NONE, reduceFully(reducer2));
  TEST_ASSERT_TRUE(reducer2.lookupMarker("m1", state));
  TEST_ASSERT_EQUAL_STRING("AB", state.note);
}

static void test_note_limits_500_scalars_and_2000_bytes() {
  const std::string emoji = "\xF0\x9F\x98\x80";  // 4 UTF-8 bytes each

  // 500 emoji == 500 scalars == 2000 bytes: exactly at both limits.
  std::string note500;
  for (int i = 0; i < 500; ++i) {
    note500 += emoji;
  }
  const std::vector<std::string> ok = {
      started(1),
      created(2, "op-a", "m1", "1", note500, "estimated"),
      created(3, "op-b", "m2", "1", std::string(500, 'x'),
              "estimated"),
  };
  MarkerReducer reducer;
  const std::vector<uint8_t> bytes = journal(kRunId, ok);
  reducer.reset(bytes.data(), bytes.size());
  assertError(ArchiveError::NONE, reduceFully(reducer));
  TEST_ASSERT_EQUAL_UINT32(2, reducer.annotationCount());
  MarkerState state;
  TEST_ASSERT_TRUE(reducer.lookupMarker("m1", state));
  TEST_ASSERT_EQUAL_UINT32(2000, strlen(state.note));
  TEST_ASSERT_TRUE(reducer.lookupMarker("m2", state));
  TEST_ASSERT_EQUAL_UINT32(500, strlen(state.note));

  // 501 emoji: over the scalar limit (and over 2000 bytes).
  std::string note501;
  for (int i = 0; i < 501; ++i) {
    note501 += emoji;
  }
  expectRejected(kRunId, {started(1), created(2, "op-a", "m1", "1",
                                             note501, "estimated")},
                 1, 0);
  // 501 ASCII scalars within the byte limit: still rejected.
  expectRejected(kRunId, {started(1),
                          created(2, "op-a", "m1", "1",
                                  std::string(501, 'x'),
                                  "estimated")},
                 1, 0);
}

static void test_zero_timestamp_and_untimed_forms() {
  const std::vector<std::string> ok = {
      started(1),
      created(2, "op-a", "m1", "0", "zero", "estimated"),
      created(3, "op-b", "m2", "null", "no time", "untimed"),
      updated(4, "op-c", "m1", "2", "86400000", "edge", "manual"),
  };
  MarkerReducer reducer;
  const std::vector<uint8_t> bytes = journal(kRunId, ok);
  reducer.reset(bytes.data(), bytes.size());
  assertError(ArchiveError::NONE, reducer.ingestNext(3));

  MarkerState state;
  TEST_ASSERT_TRUE(reducer.lookupMarker("m1", state));
  TEST_ASSERT_TRUE(state.hasMs);
  TEST_ASSERT_EQUAL_UINT32(0, state.ms);
  TEST_ASSERT_TRUE(state.timeBasis == TimeBasis::Estimated);
  TEST_ASSERT_TRUE(reducer.lookupMarker("m2", state));
  TEST_ASSERT_FALSE(state.hasMs);
  TEST_ASSERT_TRUE(state.timeBasis == TimeBasis::Untimed);
  assertError(ArchiveError::NONE, reduceFully(reducer));
  TEST_ASSERT_TRUE(reducer.lookupMarker("m1", state));
  TEST_ASSERT_EQUAL_UINT32(86400000, state.ms);
  TEST_ASSERT_TRUE(state.timeBasis == TimeBasis::Manual);

  // untimed requires ms:null and vice versa; ranges are enforced.
  expectRejected(kRunId, {started(1),
                          created(2, "op-a", "m1", "0", "x", "untimed")},
                 1, 0);
  expectRejected(kRunId, {started(1),
                          created(2, "op-a", "m1", "null", "x", "estimated")},
                 1, 0);
  expectRejected(kRunId, {started(1),
                          created(2, "op-a", "m1", "86400001", "x", "manual")},
                 1, 0);
  expectRejected(kRunId,
                 {started(1), created(2, "op-a", "m1", "5", "x", "sideways")},
                 1, 0);
}

// ---------------------------------------------------------- collisions ----

static void test_hash_collision_ids_resolve_independently() {
  std::string a;
  std::string b;
  findCollidingIds(a, b);
  TEST_ASSERT_FALSE(a.empty());
  TEST_ASSERT_FALSE(b.empty());
  TEST_ASSERT_TRUE(a != b);
  // The duplicated hash agrees: the two ids are indistinguishable to the
  // (offset,hash) entries and must be separated by decoded id comparison.
  TEST_ASSERT_EQUAL_UINT32(idHash(a.c_str()) & 0xFFFu,
                           idHash(b.c_str()) & 0xFFFu);
  TEST_ASSERT_EQUAL_UINT32((idHash(a.c_str()) >> 18) & 0x3FFFu,
                           (idHash(b.c_str()) >> 18) & 0x3FFFu);

  const std::vector<std::string> payloads = {
      started(1),
      created(2, "op-a", a, "1", "A-note", "estimated"),
      created(3, "op-b", b, "2", "B-note", "estimated"),
  };
  MarkerReducer reducer;
  const std::vector<uint8_t> bytes = journal(kRunId, payloads);
  reducer.reset(bytes.data(), bytes.size());
  assertError(ArchiveError::NONE, reduceFully(reducer));

  MarkerState state;
  TEST_ASSERT_TRUE(reducer.lookupMarker(a.c_str(), state));
  TEST_ASSERT_EQUAL_STRING("A-note", state.note);
  TEST_ASSERT_EQUAL_UINT32(2, state.revision);
  TEST_ASSERT_TRUE(reducer.lookupMarker(b.c_str(), state));
  TEST_ASSERT_EQUAL_STRING("B-note", state.note);
  TEST_ASSERT_EQUAL_UINT32(3, state.revision);

  // Editing one colliding id must not disturb the other; a stale base on the
  // colliding sibling is rejected without touching the prefix state.
  expectRejected(kRunId,
                 {started(1), created(2, "op-a", a, "1", "A", "estimated"),
                  created(3, "op-b", b, "2", "B", "estimated"),
                  updated(4, "op-c", b, "99", "3", "B2", "estimated")},
                 3, 2);

  const std::vector<std::string> chain = {
      started(1),
      created(2, "op-a", a, "1", "A-note", "estimated"),
      created(3, "op-b", b, "2", "B-note", "estimated"),
      updated(4, "op-c", a, "2", "3", "A2", "manual"),
      deleted(5, "op-d", a, "4"),
  };
  MarkerReducer reducer2;
  const std::vector<uint8_t> bytes2 = journal(kRunId, chain);
  reducer2.reset(bytes2.data(), bytes2.size());
  assertError(ArchiveError::NONE, reduceFully(reducer2));
  TEST_ASSERT_TRUE(reducer2.lookupMarker(a.c_str(), state));
  TEST_ASSERT_TRUE(state.deleted);
  TEST_ASSERT_EQUAL_UINT32(5, state.revision);
  TEST_ASSERT_TRUE(reducer2.lookupMarker(b.c_str(), state));
  TEST_ASSERT_FALSE(state.deleted);
  TEST_ASSERT_EQUAL_STRING("B-note", state.note);
  TEST_ASSERT_EQUAL_UINT32(3, state.revision);
  TEST_ASSERT_EQUAL_UINT32(1, reducer2.annotationCount());
}

static void test_many_markers_survive_accidental_collisions() {
  std::vector<size_t> frameEnds;
  std::vector<std::string> payloads = {started(1)};
  for (uint32_t i = 0; i < 500; ++i) {
    const std::string id = "m" + num(i);
    payloads.push_back(
        created(2 + i, "op-" + num(i), id, "9", "note " + id, "estimated"));
  }
  for (uint32_t i = 0; i < 500; i += 2) {
    payloads.push_back(
        deleted(502 + i / 2, "del-" + num(i), "m" + num(i), num(2 + i)));
  }
  MarkerReducer reducer;
  const std::vector<uint8_t> bytes = journal(kRunId, payloads, &frameEnds);
  TEST_ASSERT_TRUE(bytes.size() < 262144);
  reducer.reset(bytes.data(), bytes.size());
  assertError(ArchiveError::NONE, reduceFully(reducer));

  TEST_ASSERT_EQUAL_UINT32(751, reducer.revision());
  TEST_ASSERT_EQUAL_UINT32(250, reducer.annotationCount());
  MarkerState state;
  for (uint32_t i = 0; i < 500; ++i) {
    const std::string id = "m" + num(i);
    TEST_ASSERT_TRUE(reducer.lookupMarker(id.c_str(), state));
    if (i % 2 == 0) {
      TEST_ASSERT_TRUE(state.deleted);
      TEST_ASSERT_EQUAL_UINT32(502 + i / 2, state.revision);
    } else {
      TEST_ASSERT_FALSE(state.deleted);
      TEST_ASSERT_EQUAL_STRING(("note " + id).c_str(), state.note);
      TEST_ASSERT_EQUAL_UINT32(2 + i, state.revision);
    }
  }
}

// ---------------------------------------------------------- rejections ----

static void test_revision_sequence_enforced() {
  expectRejected(kRunId, {started(1), created(3, "op-a", "m1", "1", "x",
                                         "estimated")},
                 1, 0);  // gap
  expectRejected(kRunId,
                 {started(1), created(2, "op-a", "m1", "1", "x", "estimated"),
                  created(2, "op-b", "m2", "1", "x", "estimated")},
                 2, 1);  // repeated revision
  expectRejected(kRunId, {created(1, "op-a", "m1", "1", "x", "estimated")},
                 0, 0);  // journal must open with run.started
  expectRejected(kRunId,
                 {started(1), started(2, 5), ended(3)}, 1,
                 0);  // run.started only at revision 1
  expectRejected(kRunId, {started(2)}, 0, 0);  // revision 1 only
}

static void test_lifecycle_field_validation() {
  // run.started must carry active status, null error and zero dropped.
  expectRejected(kRunId,
                 {"{\"revision\":1,\"type\":\"run.started\",\"requestId\":\"r\","
                  "\"ms\":0,\"status\":\"complete\",\"errorCode\":null,"
                  "\"droppedAcquired\":0}"},
                 0, 0);
  expectRejected(kRunId,
                 {"{\"revision\":1,\"type\":\"run.started\",\"requestId\":\"r\","
                  "\"ms\":0,\"status\":\"active\",\"errorCode\":\"WRITE_FAILED\","
                  "\"droppedAcquired\":0}"},
                 0, 0);
  expectRejected(kRunId,
                 {"{\"revision\":1,\"type\":\"run.started\",\"requestId\":\"r\","
                  "\"ms\":0,\"status\":\"active\",\"errorCode\":null,"
                  "\"droppedAcquired\":3}"},
                 0, 0);
  expectRejected(kRunId,
                 {"{\"revision\":1,\"type\":\"run.started\",\"requestId\":\"r\","
                  "\"ms\":0,\"status\":\"active\",\"errorCode\":null}"},
                 0, 0);  // missing droppedAcquired

  // run.ended: terminal status, known errorCode or null, any dropped count.
  expectRejected(kRunId, {started(1), ended(2, 100, "active")}, 1, 0);
  expectRejected(kRunId, {started(1), ended(2, 100, "complete", "\"BOGUS\"")},
                 1, 0);
  expectRejected(kRunId, {started(1), ended(2, 100, "complete", "7")}, 1, 0);
  expectRejected(kRunId,
                 {"{\"revision\":1,\"type\":\"run.started\",\"requestId\":\"r\","
                  "\"ms\":0,\"status\":\"active\",\"errorCode\":null,"
                  "\"droppedAcquired\":0}",
                  "{\"revision\":2,\"type\":\"run.ended\",\"requestId\":\"r\","
                  "\"ms\":9,\"status\":\"storage_error\"}"},
                 1, 0);  // missing errorCode

  const std::vector<std::string> good = {
      started(1), ended(2, 100, "storage_error", "\"WRITE_FAILED\"", 17)};
  MarkerReducer reducer;
  const std::vector<uint8_t> bytes = journal(kRunId, good);
  reducer.reset(bytes.data(), bytes.size());
  assertError(ArchiveError::NONE, reduceFully(reducer));
  TEST_ASSERT_TRUE(reducer.ready());
  TEST_ASSERT_EQUAL_UINT32(2, reducer.revision());
}

static void test_marker_base_revision_transitions() {
  // created always carries baseRevision 0 for a never-seen id.
  expectRejected(kRunId,
                 {started(1), created(2, "op-a", "m1", "1", "x", "estimated",
                                      "1")},
                 1, 0);
  expectRejected(kRunId,
                 {started(1), created(2, "op-a", "m1", "1", "x", "estimated"),
                  created(3, "op-b", "m1", "1", "x", "estimated")},
                 2, 1);  // id already exists
  // updated/deleted require the marker's latest mutation revision.
  expectRejected(kRunId,
                 {started(1), created(2, "op-a", "m1", "1", "x", "estimated"),
                  updated(3, "op-b", "m1", "1", "2", "x", "manual")},
                 2, 1);  // stale base (current revision is 2)
  expectRejected(kRunId,
                 {started(1), created(2, "op-a", "m1", "1", "x", "estimated"),
                  updated(3, "op-b", "zz", "1", "2", "x", "manual")},
                 2, 1);  // unknown marker
  expectRejected(kRunId,
                 {started(1), created(2, "op-a", "m1", "1", "x", "estimated"),
                  deleted(3, "op-b", "m1", "1")},
                 2, 1);  // stale delete base
  expectRejected(kRunId,
                 {started(1), created(2, "op-a", "m1", "1", "x", "estimated"),
                  deleted(3, "op-b", "zz", "1")},
                 2, 1);  // delete of unknown marker
  // Deleted markers stay deleted: no resurrection or further mutation.
  expectRejected(kRunId,
                 {started(1), created(2, "op-a", "m1", "1", "x", "estimated"),
                  deleted(3, "op-b", "m1", "2"),
                  created(4, "op-c", "m1", "1", "x", "estimated")},
                 3, 0);  // create over tombstone
  expectRejected(kRunId,
                 {started(1), created(2, "op-a", "m1", "1", "x", "estimated"),
                  deleted(3, "op-b", "m1", "2"),
                  deleted(4, "op-c", "m1", "3")},
                 3, 0);  // delete of tombstone
  expectRejected(kRunId,
                 {started(1), created(2, "op-a", "m1", "1", "x", "estimated"),
                  deleted(3, "op-b", "m1", "2"),
                  updated(4, "op-c", "m1", "3", "2", "x", "manual")},
                 3, 0);  // update of tombstone
}

static void test_duplicate_operation_ids_rejected() {
  expectRejected(kRunId,
                 {started(1), created(2, "op-a", "m1", "1", "x", "estimated"),
                  created(3, "op-a", "m2", "1", "x", "estimated")},
                 2, 1);
  expectRejected(kRunId,
                 {started(1), created(2, "op-a", "m1", "1", "x", "estimated"),
                  deleted(3, "op-a", "m1", "2")},
                 2, 1);
}

static void test_missing_fields_and_bad_types_rejected() {
  // marker.updated without note.
  expectRejected(kRunId,
                 {started(1), created(2, "op-a", "m1", "1", "x", "estimated"),
                  "{\"revision\":3,\"type\":\"marker.updated\","
                  "\"operationId\":\"op-b\",\"id\":\"m1\",\"baseRevision\":2,"
                  "\"ms\":null,\"timeBasis\":\"untimed\"}"},
                 2, 1);
  // Numeric fields must be integral JSON numbers, not strings or fractions.
  expectRejected(kRunId,
                 {started(1), created(2, "op-a", "m1", "\"5\"", "x",
                                      "estimated")},
                 1, 0);
  expectRejected(kRunId,
                 {started(1), created(2, "op-a", "m1", "1.5", "x", "estimated")},
                 1, 0);
  expectRejected(kRunId,
                 {"{\"revision\":\"1\",\"type\":\"run.started\","
                  "\"requestId\":\"r\",\"ms\":0,\"status\":\"active\","
                  "\"errorCode\":null,\"droppedAcquired\":0}"},
                 0, 0);
  expectRejected(kRunId,
                 {started(1), created(2, "op-a", "m1", "1", "x", "estimated",
                                      "-1")},
                 1, 0);
  // Non-string note and unknown type are rejected.
  expectRejected(kRunId,
                 {started(1), createdJson(2, "op-a", "m1", "1", "7", "estimated")},
                 1, 0);
  expectRejected(kRunId,
                 {"{\"revision\":1,\"type\":\"marker.moved\","
                  "\"operationId\":\"op-a\",\"id\":\"m1\",\"baseRevision\":0,"
                  "\"ms\":null,\"note\":\"x\",\"timeBasis\":\"untimed\"}"},
                 0, 0);
  // marker.deleted must not invent ms/note/timeBasis fields.
  expectRejected(kRunId,
                 {started(1), created(2, "op-a", "m1", "1", "x", "estimated"),
                  "{\"revision\":3,\"type\":\"marker.deleted\","
                  "\"operationId\":\"op-b\",\"id\":\"m1\",\"baseRevision\":2,"
                  "\"note\":\"\"}"},
                 2, 1);
  // Identifiers are 1..80 ASCII [A-Za-z0-9_-].
  expectRejected(kRunId,
                 {started(1), created(2, "op-a", "m 1", "1", "x", "estimated")},
                 1, 0);
  expectRejected(kRunId,
                 {started(1), created(2, "", "m1", "1", "x", "estimated")},
                 1, 0);
}

static void test_crc_and_truncated_tail_preserve_prefix() {
  std::vector<size_t> frameEnds;
  const std::vector<std::string> payloads = {
      started(1),
      created(2, "op-a", "m1", "1", "kept", "estimated"),
      created(3, "op-b", "m2", "1", "kept", "estimated"),
      created(4, "op-c", "m3", "1", "lost", "estimated"),
  };

  // Flip one payload byte in the last frame: CRC rejects it as a bad tail.
  std::vector<uint8_t> badCrc = journal(kRunId, payloads, &frameEnds);
  badCrc[frameEnds[2] + 6] ^= 0xFF;
  MarkerReducer reducer;
  reducer.reset(badCrc.data(), badCrc.size());
  assertError(ArchiveError::CORRUPT_RUN, reduceFully(reducer));
  TEST_ASSERT_FALSE(reducer.ready());
  TEST_ASSERT_EQUAL_UINT32(3, reducer.revision());
  TEST_ASSERT_EQUAL_UINT32(2, reducer.annotationCount());
  MarkerState state;
  TEST_ASSERT_TRUE(reducer.lookupMarker("m1", state));
  TEST_ASSERT_FALSE(state.deleted);

  // A truncated final frame inside the claimed prefix is a bad tail too.
  std::vector<size_t> ends2;
  const std::vector<uint8_t> whole = journal(kRunId, payloads, &ends2);
  std::vector<uint8_t> truncated(whole.begin(),
                                 whole.begin() + (ends2[2] + 5));
  MarkerReducer reducer2;
  reducer2.reset(truncated.data(), truncated.size());
  assertError(ArchiveError::CORRUPT_RUN, reduceFully(reducer2));
  TEST_ASSERT_EQUAL_UINT32(3, reducer2.revision());
  TEST_ASSERT_EQUAL_UINT32(2, reducer2.annotationCount());

  // A corrupt header reports before any event can be claimed.
  std::vector<uint8_t> badHeader = journal(kRunId, payloads);
  badHeader[8] ^= 0xFF;
  MarkerReducer reducer3;
  reducer3.reset(badHeader.data(), badHeader.size());
  assertError(ArchiveError::CORRUPT_RUN, reduceFully(reducer3));
  TEST_ASSERT_EQUAL_UINT32(0, reducer3.revision());
  TEST_ASSERT_EQUAL_UINT32(0, reducer3.annotationCount());
}

static void test_invalid_escapes_and_nul_rejected() {
  // Lone surrogate escapes are invalid Unicode; the client rejects them.
  expectRejected(kRunId,
                 {started(1),
                  createdJson(2, "op-a", "m1", "1", "\"a\\ud800b\"", "estimated")},
                 1, 0);
  expectRejected(kRunId,
                 {started(1),
                  createdJson(2, "op-a", "m1", "1", "\"a\\ude00b\"", "estimated")},
                 1, 0);
  expectRejected(kRunId,
                 {started(1),
                  createdJson(2, "op-a", "m1", "1", "\"\\ud800\"", "estimated")},
                 1, 0);
  // Escaped NUL is rejected exactly like a raw one.
  expectRejected(kRunId,
                 {started(1),
                  createdJson(2, "op-a", "m1", "1", "\"a\\u0000b\"", "estimated")},
                 1, 0);
  {
    // Raw NUL byte inside the note string.
    std::string payload = "{\"revision\":2,\"type\":\"marker.created\","
                          "\"operationId\":\"op-a\",\"id\":\"m1\","
                          "\"baseRevision\":0,\"ms\":1,\"note\":\"a";
    payload += static_cast<char>(0);
    payload += "b\",\"timeBasis\":\"estimated\"}";
    expectRejected(kRunId, {started(1), payload}, 1, 0);
  }
  {
    // Raw control character inside the note string (must be escaped).
    std::string payload = "{\"revision\":2,\"type\":\"marker.created\","
                          "\"operationId\":\"op-a\",\"id\":\"m1\","
                          "\"baseRevision\":0,\"ms\":1,\"note\":\"a";
    payload += static_cast<char>(1);
    payload += "b\",\"timeBasis\":\"estimated\"}";
    expectRejected(kRunId, {started(1), payload}, 1, 0);
  }
  // A properly paired surrogate escape is valid and preserved exactly.
  const std::vector<std::string> ok = {
      started(1),
      createdJson(2, "op-a", "m1", "1", "\"\\ud83d\\ude00\"", "estimated"),
  };
  MarkerReducer reducer;
  const std::vector<uint8_t> bytes = journal(kRunId, ok);
  reducer.reset(bytes.data(), bytes.size());
  assertError(ArchiveError::NONE, reduceFully(reducer));
  MarkerState state;
  TEST_ASSERT_TRUE(reducer.lookupMarker("m1", state));
  TEST_ASSERT_EQUAL_STRING("\xF0\x9F\x98\x80", state.note);
}

static void test_trailing_garbage_and_bad_numbers_rejected() {
  // Trailing bytes after the JSON value are not a valid event payload.
  expectRejected(kRunId,
                 {started(1),
                  created(2, "op-a", "m1", "1", "x", "estimated") + " junk"},
                 1, 0);
  // Strict JSON numbers only: no leading zeros, signs or bare fractions.
  expectRejected(kRunId,
                 {started(1), created(2, "op-a", "m1", "01", "x", "estimated")},
                 1, 0);
  expectRejected(kRunId,
                 {started(1), created(2, "op-a", "m1", "+1", "x", "estimated")},
                 1, 0);
  expectRejected(kRunId,
                 {started(1), created(2, "op-a", "m1", ".5", "x", "estimated")},
                 1, 0);
  expectRejected(kRunId,
                 {started(1), created(2, "op-a", "m1", "1.", "x", "estimated")},
                 1, 0);
  // Duplicate member names are rejected instead of silently resolved.
  expectRejected(kRunId,
                 {started(1),
                  "{\"revision\":2,\"type\":\"marker.created\","
                  "\"operationId\":\"op-a\",\"id\":\"m1\",\"baseRevision\":0,"
                  "\"ms\":1,\"note\":\"x\",\"note\":\"y\","
                  "\"timeBasis\":\"estimated\"}"},
                 1, 0);
}

// ------------------------------------------------------ lifecycle of ------

static void test_reset_lifecycle_growth_shrink() {
  const std::vector<std::string> payloadsA = {
      started(1),
      created(2, "op-a", "m1", "1", "A", "estimated"),
      created(3, "op-b", "m2", "1", "B", "estimated"),
  };
  MarkerReducer reducer;
  const std::vector<uint8_t> bytesA = journal(7, payloadsA);
  reducer.reset(bytesA.data(), bytesA.size());
  assertError(ArchiveError::NONE, reduceFully(reducer));
  TEST_ASSERT_EQUAL_UINT32(3, reducer.revision());
  TEST_ASSERT_EQUAL_UINT32(2, reducer.annotationCount());
  MarkerMutation mutation;
  MutationResult result;
  TEST_ASSERT_TRUE(reducer.lookupOperation("op-a", mutation, result));
  TEST_ASSERT_EQUAL_UINT32(7, result.runId);

  // reset drops all state and binds different stable bytes.
  const std::vector<std::string> payloadsB = {
      started(1),
      created(2, "op-x", "z1", "null", "B-run", "untimed"),
      updated(3, "op-y", "z1", "2", "5", "B2", "manual"),
  };
  const std::vector<uint8_t> bytesB = journal(9, payloadsB);
  reducer.reset(bytesB.data(), bytesB.size());
  TEST_ASSERT_EQUAL_UINT32(0, reducer.revision());
  TEST_ASSERT_EQUAL_UINT32(0, reducer.annotationCount());
  TEST_ASSERT_FALSE(reducer.ready());
  MarkerState state;
  fillBytes(&state, sizeof(state), 0x5A);
  const MarkerState before = state;
  TEST_ASSERT_FALSE(reducer.lookupMarker("m1", state));  // old run is gone
  TEST_ASSERT_EQUAL_MEMORY(&before, &state, sizeof(state));

  assertError(ArchiveError::NONE, reduceFully(reducer));
  TEST_ASSERT_EQUAL_UINT32(3, reducer.revision());
  TEST_ASSERT_EQUAL_UINT32(1, reducer.annotationCount());
  TEST_ASSERT_TRUE(reducer.lookupMarker("z1", state));
  TEST_ASSERT_EQUAL_STRING("B2", state.note);
  TEST_ASSERT_FALSE(reducer.lookupMarker("m1", state));
  TEST_ASSERT_FALSE(reducer.lookupOperation("op-a", mutation, result));
  TEST_ASSERT_TRUE(reducer.lookupOperation("op-x", mutation, result));
  TEST_ASSERT_EQUAL_UINT32(9, result.runId);  // new journal header identity

  // Partial binding then growth: reset to two frames, extend to all three.
  std::vector<size_t> endsB;
  const std::vector<uint8_t> bytesB2 = journal(11, payloadsB, &endsB);
  reducer.reset(bytesB2.data(), endsB[1]);
  assertError(ArchiveError::NONE, reduceFully(reducer));
  TEST_ASSERT_EQUAL_UINT32(2, reducer.revision());
  assertError(ArchiveError::NONE, reducer.extendValidatedPrefix(endsB[2]));
  assertError(ArchiveError::NONE, reduceFully(reducer));
  TEST_ASSERT_EQUAL_UINT32(3, reducer.revision());

  // Shrinking, over-budget and unbound extensions are refused without state
  // changes; identical re-extension is a no-op.
  assertError(ArchiveError::INVALID_REQUEST,
              reducer.extendValidatedPrefix(endsB[1]));
  TEST_ASSERT_EQUAL_UINT32(3, reducer.revision());
  assertError(ArchiveError::NONE, reducer.extendValidatedPrefix(endsB[2]));
  assertError(ArchiveError::INVALID_REQUEST,
              reducer.extendValidatedPrefix(262145));
  TEST_ASSERT_EQUAL_UINT32(3, reducer.revision());

  // A header-only journal reduces to revision 0 and is ready.
  const std::vector<uint8_t> headerOnly = journal(12, {});
  reducer.reset(headerOnly.data(), headerOnly.size());
  assertError(ArchiveError::NONE, reduceFully(reducer));
  TEST_ASSERT_TRUE(reducer.ready());
  TEST_ASSERT_EQUAL_UINT32(0, reducer.revision());

  // Unbound and empty bindings make no progress and answer no lookups.
  reducer.reset(nullptr, 0);
  TEST_ASSERT_FALSE(reducer.ready());
  assertError(ArchiveError::NONE, reducer.ingestNext(8));
  assertError(ArchiveError::NONE, reducer.ingestNext(0));
  TEST_ASSERT_FALSE(reducer.lookupMarker("z1", state));
  assertError(ArchiveError::INVALID_REQUEST,
              reducer.extendValidatedPrefix(16));  // nothing bound
  reducer.reset(nullptr, 5);  // bytes without a buffer is invalid input
  assertError(ArchiveError::INVALID_REQUEST, reducer.ingestNext(8));
  TEST_ASSERT_FALSE(reducer.ready());
}

static void test_reset_rebind_drops_prior_index_entries() {
  // Identical frame layout in a second run: every frame offset from the first
  // binding is also a valid frame offset in the second. The old run's index
  // entries must not leak into the new binding, or "m1" would look like an
  // existing marker and the same-layout create would be wrongly rejected.
  const std::vector<std::string> payloadsA = {
      started(1), created(2, "op-1", "m1", "1", "A", "estimated")};
  const std::vector<std::string> payloadsB = {
      started(1), created(2, "op-2", "m1", "1", "B", "estimated")};
  MarkerReducer reducer;
  const std::vector<uint8_t> bytesA = journal(21, payloadsA);
  reducer.reset(bytesA.data(), bytesA.size());
  assertError(ArchiveError::NONE, reduceFully(reducer));
  MarkerState state;
  TEST_ASSERT_TRUE(reducer.lookupMarker("m1", state));
  TEST_ASSERT_EQUAL_STRING("A", state.note);

  const std::vector<uint8_t> bytesB = journal(22, payloadsB);
  TEST_ASSERT_EQUAL_UINT32(bytesA.size(), bytesB.size());
  reducer.reset(bytesB.data(), bytesB.size());
  TEST_ASSERT_FALSE(reducer.lookupMarker("m1", state));  // prior run is gone
  assertError(ArchiveError::NONE, reduceFully(reducer));  // create is legal
  TEST_ASSERT_EQUAL_UINT32(2, reducer.revision());
  TEST_ASSERT_EQUAL_UINT32(1, reducer.annotationCount());
  TEST_ASSERT_TRUE(reducer.lookupMarker("m1", state));
  TEST_ASSERT_EQUAL_STRING("B", state.note);
  MarkerMutation mutation;
  MutationResult result;
  TEST_ASSERT_FALSE(reducer.lookupOperation("op-1", mutation, result));
  TEST_ASSERT_TRUE(reducer.lookupOperation("op-2", mutation, result));
  TEST_ASSERT_EQUAL_UINT32(22, result.runId);
}

static void test_error_latches_until_reset() {
  const std::vector<std::string> payloads = {
      started(1),
      created(2, "op-a", "m1", "1", "kept", "estimated"),
      created(4, "op-b", "m2", "1", "bad revision", "estimated"),
      created(5, "op-c", "m3", "1", "never", "estimated"),
  };
  MarkerReducer reducer;
  const std::vector<uint8_t> bytes = journal(kRunId, payloads);
  reducer.reset(bytes.data(), bytes.size());
  assertError(ArchiveError::CORRUPT_RUN, reduceFully(reducer));
  TEST_ASSERT_EQUAL_UINT32(2, reducer.revision());
  TEST_ASSERT_EQUAL_UINT32(1, reducer.annotationCount());

  // Latched everywhere: ingest, extension and readiness stay frozen while
  // the valid prefix remains queryable.
  assertError(ArchiveError::CORRUPT_RUN, reducer.ingestNext(8));
  assertError(ArchiveError::CORRUPT_RUN,
              reducer.extendValidatedPrefix(bytes.size()));
  TEST_ASSERT_FALSE(reducer.ready());
  TEST_ASSERT_EQUAL_UINT32(2, reducer.revision());
  MarkerState state;
  TEST_ASSERT_TRUE(reducer.lookupMarker("m1", state));
  TEST_ASSERT_EQUAL_STRING("kept", state.note);

  // reset clears the latch.
  reducer.reset(bytes.data(), eventjournalcodec::kHeaderSize);  // header only
  assertError(ArchiveError::NONE, reduceFully(reducer));
  TEST_ASSERT_TRUE(reducer.ready());
}

void setup() {
  UNITY_BEGIN();
  RUN_TEST(test_ingest_is_bounded_per_call);
  RUN_TEST(test_prefix_extension_reduces_appended_frames);
  RUN_TEST(test_chunked_cache_load_with_frame_boundaries);
  RUN_TEST(test_create_update_delete_counts_and_states);
  RUN_TEST(test_markers_and_notes_after_run_end);
  RUN_TEST(test_operation_lookup_returns_exact_original);
  RUN_TEST(test_lookup_missing_leaves_outputs_untouched);
  RUN_TEST(test_note_preserves_unicode_quotes_newlines);
  RUN_TEST(test_note_limits_500_scalars_and_2000_bytes);
  RUN_TEST(test_zero_timestamp_and_untimed_forms);
  RUN_TEST(test_hash_collision_ids_resolve_independently);
  RUN_TEST(test_many_markers_survive_accidental_collisions);
  RUN_TEST(test_revision_sequence_enforced);
  RUN_TEST(test_lifecycle_field_validation);
  RUN_TEST(test_marker_base_revision_transitions);
  RUN_TEST(test_duplicate_operation_ids_rejected);
  RUN_TEST(test_missing_fields_and_bad_types_rejected);
  RUN_TEST(test_crc_and_truncated_tail_preserve_prefix);
  RUN_TEST(test_invalid_escapes_and_nul_rejected);
  RUN_TEST(test_trailing_garbage_and_bad_numbers_rejected);
  RUN_TEST(test_reset_lifecycle_growth_shrink);
  RUN_TEST(test_reset_rebind_drops_prior_index_entries);
  RUN_TEST(test_error_latches_until_reset);
  UNITY_END();
}

void loop() {}

#if defined(PYROVUE_NATIVE)
int main() {
  setup();
  return Unity.TestFailures == 0 ? 0 : 1;
}
#endif
