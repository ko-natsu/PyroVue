// MarkerReducer: bounded incremental reducer over one caller-owned PVE1 event
// journal (see MarkerReducer.h for the full contract).
//
// Memory design: exactly ONE lazily allocated block (explicit PSRAM via
// heap_caps_malloc(MALLOC_CAP_SPIRAM) on ESP32, portable malloc elsewhere)
// holds both fixed 4096-entry compact indexes and the reusable ArduinoJson
// scratch arena. Entries store only (frame offset, hash); id comparisons and
// note materialization re-decode the immutable journal bytes on demand. The
// arena is a bump allocator rewound after each document parse, so event
// ingestion performs no per-event heap allocation. reset() keeps the block.
//
// The reducer re-validates everything it consumes (defense in depth): framing
// via EventJournalCodec (bounds/CRC/UTF-8), JSON encoding via the strict
// scanner below (RFC 8259 strings/escapes/numbers/unique keys; raw or escaped
// NUL and unpaired surrogate escapes are rejected), then exact journal
// semantics via ArduinoJson following the client run-protocol.js parseEvent
// conventions plus the transition rules documented in the header.

#include "MarkerReducer.h"

#include <ArduinoJson.h>
#include <math.h>
#include <new>
#include <stdlib.h>
#include <string.h>

#include "EventJournalCodec.h"

#if defined(ARDUINO_ARCH_ESP32)
#include <esp_heap_caps.h>
#endif

namespace markerreducer_detail {

// One journal cache is bounded by RUN_EVENT_BUDGET_BYTES; the reducer never
// binds more than this, which also bounds every index frame offset to 18 bits.
constexpr size_t kMaxJournalBytes = 262144;

constexpr size_t kIndexSlots = 4096;   // plan-sized: covers the journal budget
constexpr uint32_t kIndexMask = 4095;
constexpr size_t kArenaBytes = 24576;  // bounded JSON scratch (one event at a time)
constexpr size_t kMaxIngestFrames = 8;
constexpr size_t kMaxIdChars = 80;
constexpr uint32_t kMaxEventMs = 86400000;  // client MAX_RUN_MS
constexpr int kMaxJsonDepth = 8;            // events are flat objects
constexpr size_t kMaxObjectMembers = 24;    // bounded duplicate-key check

// ---------------------------------------------------------------- memory ---

void* allocBlock(size_t bytes) {
#if defined(ARDUINO_ARCH_ESP32)
  // Explicit PSRAM: the index/scratch block must never land in internal RAM.
  return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
  return malloc(bytes);
#endif
}

void freeBlock(void* ptr) {
#if defined(ARDUINO_ARCH_ESP32)
  heap_caps_free(ptr);
#else
  free(ptr);
#endif
}

// Bounded bump allocator for the one reusable ArduinoJson arena. deallocate()
// is a no-op because the whole arena is rewound per parsed event; reallocate()
// resizes the most recent allocation in place when possible and otherwise
// copies (the stale copy is reclaimed by the next rewind). Each allocation is
// preceded by a 16-byte header recording its requested size.
class ArenaAllocator : public ArduinoJson::Allocator {
 public:
  void bind(uint8_t* buffer, size_t bytes) {
    buffer_ = buffer;
    capacity_ = bytes;
    used_ = 0;
  }

  void rewind() { used_ = 0; }

  void* allocate(size_t size) override {
    if (buffer_ == nullptr) {
      return nullptr;
    }
    const size_t need = kHeader + roundUp(size);
    if (need > capacity_ - used_) {
      return nullptr;
    }
    uint8_t* block = buffer_ + used_;
    used_ += need;
    memcpy(block, &size, sizeof(size));
    return block + kHeader;
  }

  void deallocate(void*) override {}

  void* reallocate(void* ptr, size_t new_size) override {
    if (ptr == nullptr) {
      return allocate(new_size);
    }
    uint8_t* block = static_cast<uint8_t*>(ptr) - kHeader;
    size_t old_size = 0;
    memcpy(&old_size, block, sizeof(old_size));
    const size_t old_need = kHeader + roundUp(old_size);
    const size_t new_need = kHeader + roundUp(new_size);
    if (block + old_need == buffer_ + used_) {
      // Most recent allocation: grow or shrink in place.
      if (new_need <= old_need || new_need - old_need <= capacity_ - used_) {
        used_ += new_need - old_need;
        memcpy(block, &new_size, sizeof(new_size));
        return ptr;
      }
    }
    void* fresh = allocate(new_size);
    if (fresh == nullptr) {
      return nullptr;
    }
    memcpy(fresh, ptr, old_size < new_size ? old_size : new_size);
    return fresh;
  }

 private:
  static const size_t kHeader = 16;  // size header, keeps payloads 16-aligned

  static size_t roundUp(size_t size) {
    return (size + 15) & ~static_cast<size_t>(15);
  }

  uint8_t* buffer_ = nullptr;
  size_t capacity_ = 0;
  size_t used_ = 0;
};

// -------------------------------------------------------------- id hash ----

// FNV-1a 32 over the DECODED id bytes (ids are 1..80 ASCII). Documented index
// contract: slot = hash & 4095 and the entry stores hash >> 18. The reducer
// test suite duplicates this function to construct real hash collisions.
uint32_t idHash(const char* id) {
  uint32_t h = 2166136261u;
  for (const uint8_t* p = reinterpret_cast<const uint8_t*>(id); *p; ++p) {
    h ^= *p;
    h *= 16777619u;
  }
  return h;
}

// ------------------------------------------------ strict JSON encoding ------

// Validates the payload as exactly one strict RFC 8259 value. This is encoding
// validation (comparable to the framing codec's UTF-8 check), not a second
// semantic parser: kinds/fields/types/ranges are validated with ArduinoJson.
// ArduinoJson is lenient where the client JSON.parse is not (lone surrogate
// escapes are silently dropped, \u0000 is decoded, non-strict numbers and
// trailing bytes are accepted), so those are rejected here.
class JsonEncodingValidator {
 public:
  JsonEncodingValidator(const uint8_t* data, size_t size)
      : data_(data), size_(size) {}

  bool validateDocument() {
    skipWs();
    if (!value(0)) {
      return false;
    }
    skipWs();
    return pos_ == size_;
  }

 private:
  bool value(int depth) {
    if (depth > kMaxJsonDepth) {
      return false;
    }
    skipWs();
    if (pos_ >= size_) {
      return false;
    }
    switch (data_[pos_]) {
      case '{':
        return object(depth + 1);
      case '[':
        return array(depth + 1);
      case '"':
        return string();
      case 't':
        return literal("true", 4);
      case 'f':
        return literal("false", 5);
      case 'n':
        return literal("null", 4);
      default:
        return number();
    }
  }

  bool object(int depth) {
    ++pos_;  // '{'
    skipWs();
    if (peek() == '}') {
      ++pos_;
      return true;
    }
    size_t keys[kMaxObjectMembers];
    size_t keyEnds[kMaxObjectMembers];
    size_t keyCount = 0;
    for (;;) {
      if (peek() != '"') {
        return false;
      }
      const size_t keyStart = pos_;
      if (!string()) {
        return false;
      }
      const size_t keyEnd = pos_;
      for (size_t i = 0; i < keyCount; ++i) {
        if (keyEnds[i] - keys[i] == keyEnd - keyStart &&
            memcmp(data_ + keys[i], data_ + keyStart, keyEnd - keyStart) == 0) {
          return false;  // duplicate member name
        }
      }
      if (keyCount == kMaxObjectMembers) {
        return false;
      }
      keys[keyCount] = keyStart;
      keyEnds[keyCount] = keyEnd;
      ++keyCount;
      skipWs();
      if (peek() != ':') {
        return false;
      }
      ++pos_;
      if (!value(depth)) {
        return false;
      }
      skipWs();
      const char c = peek();
      if (c == ',') {
        ++pos_;
        skipWs();
        continue;
      }
      if (c == '}') {
        ++pos_;
        return true;
      }
      return false;
    }
  }

  bool array(int depth) {
    ++pos_;  // '['
    skipWs();
    if (peek() == ']') {
      ++pos_;
      return true;
    }
    for (;;) {
      if (!value(depth)) {
        return false;
      }
      skipWs();
      const char c = peek();
      if (c == ',') {
        ++pos_;
        continue;
      }
      if (c == ']') {
        ++pos_;
        return true;
      }
      return false;
    }
  }

  bool string() {
    ++pos_;  // opening quote
    for (;;) {
      if (pos_ >= size_) {
        return false;
      }
      const uint8_t c = data_[pos_];
      if (c == '"') {
        ++pos_;
        return true;
      }
      if (c == '\\') {
        if (pos_ + 1 >= size_) {
          return false;
        }
        const uint8_t e = data_[pos_ + 1];
        if (e == '"' || e == '\\' || e == '/' || e == 'b' || e == 'f' ||
            e == 'n' || e == 'r' || e == 't') {
          pos_ += 2;
          continue;
        }
        if (e != 'u') {
          return false;  // rejects \' and unknown escapes (strict JSON)
        }
        if (pos_ + 6 > size_) {
          return false;
        }
        uint32_t code = 0;
        if (!hex4(pos_ + 2, code)) {
          return false;
        }
        if (code == 0) {
          return false;  // escaped NUL is never accepted
        }
        if (code >= 0xD800 && code <= 0xDBFF) {
          // A high surrogate escape must be paired with a low surrogate
          // escape; nothing else can carry the low half (the payload is
          // already validated UTF-8). The client rejects lone surrogates.
          if (pos_ + 12 > size_ || data_[pos_ + 6] != '\\' ||
              data_[pos_ + 7] != 'u') {
            return false;
          }
          uint32_t low = 0;
          if (!hex4(pos_ + 8, low) || low < 0xDC00 || low > 0xDFFF) {
            return false;
          }
          pos_ += 12;
          continue;
        }
        if (code >= 0xDC00 && code <= 0xDFFF) {
          return false;  // lone low surrogate escape
        }
        pos_ += 6;
        continue;
      }
      if (c < 0x20) {
        return false;  // raw control character (including NUL) in string
      }
      ++pos_;
    }
  }

  bool number() {
    if (peek() == '-') {
      ++pos_;
    }
    if (pos_ >= size_) {
      return false;
    }
    if (data_[pos_] == '0') {
      ++pos_;
    } else if (data_[pos_] >= '1' && data_[pos_] <= '9') {
      while (pos_ < size_ && data_[pos_] >= '0' && data_[pos_] <= '9') {
        ++pos_;
      }
    } else {
      return false;  // rejects '+1', '.5', bare '-', etc.
    }
    if (pos_ < size_ && data_[pos_] == '.') {
      ++pos_;
      if (pos_ >= size_ || data_[pos_] < '0' || data_[pos_] > '9') {
        return false;  // rejects '1.'
      }
      while (pos_ < size_ && data_[pos_] >= '0' && data_[pos_] <= '9') {
        ++pos_;
      }
    }
    if (pos_ < size_ && (data_[pos_] == 'e' || data_[pos_] == 'E')) {
      ++pos_;
      if (pos_ < size_ && (data_[pos_] == '+' || data_[pos_] == '-')) {
        ++pos_;
      }
      if (pos_ >= size_ || data_[pos_] < '0' || data_[pos_] > '9') {
        return false;
      }
      while (pos_ < size_ && data_[pos_] >= '0' && data_[pos_] <= '9') {
        ++pos_;
      }
    }
    return true;
  }

  bool literal(const char* text, size_t len) {
    if (len > size_ - pos_ || memcmp(data_ + pos_, text, len) != 0) {
      return false;
    }
    pos_ += len;
    return true;
  }

  bool hex4(size_t at, uint32_t& out) {
    if (at + 4 > size_) {
      return false;
    }
    uint32_t value = 0;
    for (size_t i = 0; i < 4; ++i) {
      const uint8_t c = data_[at + i];
      uint32_t digit;
      if (c >= '0' && c <= '9') {
        digit = c - '0';
      } else if (c >= 'a' && c <= 'f') {
        digit = c - 'a' + 10;
      } else if (c >= 'A' && c <= 'F') {
        digit = c - 'A' + 10;
      } else {
        return false;
      }
      value = (value << 4) | digit;
    }
    out = value;
    return true;
  }

  void skipWs() {
    while (pos_ < size_) {
      const uint8_t c = data_[pos_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++pos_;
      } else {
        break;
      }
    }
  }

  char peek() const {
    return pos_ < size_ ? static_cast<char>(data_[pos_]) : '\0';
  }

  const uint8_t* data_;
  size_t size_;
  size_t pos_ = 0;
};

// ------------------------------------------------------ decoded text -------

// Strict UTF-8 walk of DECODED string bytes: rejects NUL, invalid sequences,
// overlongs, surrogates and out-of-range code points while counting Unicode
// scalar values (the client counts UTF-16 pairs the same way).
bool validNote(const char* data, size_t len) {
  if (len > runlog::kMaxNoteBytes) {
    return false;
  }
  size_t scalars = 0;
  size_t i = 0;
  while (i < len) {
    const uint8_t c = static_cast<uint8_t>(data[i]);
    size_t extra = 0;
    uint32_t cp = 0;
    if (c == 0) {
      return false;
    } else if (c < 0x80) {
      cp = c;
    } else if ((c & 0xE0) == 0xC0) {
      cp = c & 0x1F;
      extra = 1;
    } else if ((c & 0xF0) == 0xE0) {
      cp = c & 0x0F;
      extra = 2;
    } else if ((c & 0xF8) == 0xF0) {
      cp = c & 0x07;
      extra = 3;
    } else {
      return false;
    }
    if (i + extra >= len) {
      return false;  // truncated sequence
    }
    for (size_t k = 1; k <= extra; ++k) {
      const uint8_t cont = static_cast<uint8_t>(data[i + k]);
      if ((cont & 0xC0) != 0x80) {
        return false;
      }
      cp = (cp << 6) | (cont & 0x3F);
    }
    if ((extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) ||
        (extra == 3 && cp < 0x10000) || cp > 0x10FFFF ||
        (cp >= 0xD800 && cp <= 0xDFFF)) {
      return false;
    }
    ++scalars;
    if (scalars > runlog::kMaxNoteScalars) {
      return false;
    }
    i += extra + 1;
  }
  return true;
}

bool isIdentifier(const char* text, size_t len) {
  if (len < 1 || len > kMaxIdChars) {
    return false;
  }
  for (size_t i = 0; i < len; ++i) {
    const char c = text[i];
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == '-';
    if (!ok) {
      return false;
    }
  }
  return true;
}

bool textEquals(ArduinoJson::JsonString s, const char* lit, size_t len) {
  return s.size() == len && memcmp(s.c_str(), lit, len) == 0;
}

bool errorCodeKnown(ArduinoJson::JsonString s) {
  static const char* const kCodes[] = {
      "INVALID_REQUEST",     "RUN_NOT_FOUND",
      "MARKER_NOT_FOUND",    "ARCHIVE_UNAVAILABLE",
      "RUN_ACTIVE",          "RUN_ID_CONFLICT",
      "REVISION_CONFLICT",   "OPERATION_ID_CONFLICT",
      "EVENT_TIME_OUT_OF_RANGE", "CORRUPT_RUN",
      "STALE_COMMAND",       "DEVICE_ID_MISMATCH",
      "BODY_TOO_LARGE",      "INVALID_CURSOR",
      "BUSY",                "STORAGE_UNAVAILABLE",
      "WRITE_FAILED",        "INSUFFICIENT_STORAGE",
      "EVENT_BUDGET_EXHAUSTED", "RUN_ID_SAVE_FAILED",
      "RUN_ID_EXHAUSTED"};
  for (size_t i = 0; i < sizeof(kCodes) / sizeof(kCodes[0]); ++i) {
    const size_t len = strlen(kCodes[i]);
    if (textEquals(s, kCodes[i], len)) {
      return true;
    }
  }
  return false;
}

// --------------------------------------------------------- scratch --------

struct Tables {
  Tables() : doc(&arena) {
    arena.bind(arenaBuffer, sizeof(arenaBuffer));
    memset(markerIndex, 0, sizeof(markerIndex));
    memset(opIndex, 0, sizeof(opIndex));
  }

  // Resets the document and rewinds the arena. Only called between parses:
  // after this the previous document (and its borrowed strings) is invalid.
  void clearScratch() {
    doc.clear();
    arena.rewind();
  }

  ArenaAllocator arena;
  ArduinoJson::JsonDocument doc;
  uint32_t markerIndex[kIndexSlots];
  uint32_t opIndex[kIndexSlots];
  uint8_t arenaBuffer[kArenaBytes];
};

// ------------------------------------------------------- event decode ------

struct ParsedEvent {
  enum Kind : uint8_t { RunStarted, RunEnded, MarkerCreated, MarkerUpdated, MarkerDeleted };

  Kind kind = RunStarted;
  uint32_t revision = 0;
  uint32_t baseRevision = 0;
  uint32_t ms = 0;
  bool hasMs = false;
  TimeBasis timeBasis = TimeBasis::Untimed;
  char requestId[81] = {0};
  char operationId[81] = {0};
  char id[81] = {0};
  ArduinoJson::JsonString note;  // borrows the scratch document
};

bool readUint32(ArduinoJson::JsonVariantConst v, double lo, double hi,
                uint32_t& out) {
  if (!v.is<double>()) {
    return false;
  }
  const double d = v.as<double>();
  // Mirrors the client requireInt: an integral JSON number inside [lo, hi].
  if (!(d >= lo && d <= hi) || d != floor(d)) {
    return false;
  }
  out = static_cast<uint32_t>(d);
  return true;
}

bool readIdentifier(ArduinoJson::JsonVariantConst v, char out[81]) {
  if (!v.is<const char*>()) {
    return false;
  }
  ArduinoJson::JsonString s = v.as<ArduinoJson::JsonString>();
  if (!isIdentifier(s.c_str(), s.size())) {
    return false;
  }
  memcpy(out, s.c_str(), s.size());
  out[s.size()] = '\0';
  return true;
}

// Presence check that distinguishes an explicit JSON null from an absent
// member (both look "null" through a variant). Iterating avoids the deprecated
// JsonObjectConst::containsKey; keys carry no NUL (encoding-validated).
bool hasMember(ArduinoJson::JsonObjectConst obj, const char* key) {
  const size_t keyLen = strlen(key);
  for (ArduinoJson::JsonPairConst pair : obj) {
    const ArduinoJson::JsonString name = pair.key();
    if (name.size() == keyLen && memcmp(name.c_str(), key, keyLen) == 0) {
      return true;
    }
  }
  return false;
}

// Extracts and validates one event's exact fields (the client parseEvent
// rules). Each call resets the scratch document first; on success `out.note`
// borrows it until the next extractEvent. The payload's ENCODING must already
// be validated (or be benign for probe decoding of previously validated
// events).
bool extractEvent(const uint8_t* payload, size_t payloadBytes, Tables& tables,
                  ParsedEvent& out) {
  tables.clearScratch();
  ArduinoJson::DeserializationError parseError = ArduinoJson::deserializeJson(
      tables.doc, reinterpret_cast<const char*>(payload), payloadBytes);
  if (parseError) {
    return false;  // never expose a partially parsed document
  }
  ArduinoJson::JsonObjectConst obj =
      tables.doc.as<ArduinoJson::JsonObjectConst>();
  if (obj.isNull()) {
    return false;
  }

  if (!hasMember(obj, "revision") || !hasMember(obj, "type")) {
    return false;
  }
  uint32_t revision = 0;
  if (!readUint32(obj["revision"], 1.0, 4294967295.0, revision)) {
    return false;
  }
  if (!obj["type"].is<const char*>()) {
    return false;
  }
  ArduinoJson::JsonString type = obj["type"].as<ArduinoJson::JsonString>();

  out.revision = revision;
  out.requestId[0] = '\0';
  out.operationId[0] = '\0';
  out.id[0] = '\0';
  out.note = ArduinoJson::JsonString();
  out.baseRevision = 0;
  out.ms = 0;
  out.hasMs = false;
  out.timeBasis = TimeBasis::Untimed;

  if (textEquals(type, "run.started", 11)) {
    out.kind = ParsedEvent::RunStarted;
    if (!readIdentifier(obj["requestId"], out.requestId)) {
      return false;
    }
    if (!hasMember(obj, "ms") ||
        !readUint32(obj["ms"], 0.0, static_cast<double>(kMaxEventMs), out.ms)) {
      return false;
    }
    if (!hasMember(obj, "status") || !obj["status"].is<const char*>() ||
        !textEquals(obj["status"].as<ArduinoJson::JsonString>(), "active", 6)) {
      return false;
    }
    if (!hasMember(obj, "errorCode") || !obj["errorCode"].isNull()) {
      return false;
    }
    uint32_t dropped = 1;
    if (!hasMember(obj, "droppedAcquired") ||
        !readUint32(obj["droppedAcquired"], 0.0, 4294967295.0, dropped) ||
        dropped != 0) {
      return false;
    }
    return true;
  }

  if (textEquals(type, "run.ended", 9)) {
    out.kind = ParsedEvent::RunEnded;
    if (!readIdentifier(obj["requestId"], out.requestId)) {
      return false;
    }
    if (!hasMember(obj, "ms") ||
        !readUint32(obj["ms"], 0.0, static_cast<double>(kMaxEventMs), out.ms)) {
      return false;
    }
    if (!hasMember(obj, "status") || !obj["status"].is<const char*>()) {
      return false;
    }
    ArduinoJson::JsonString status = obj["status"].as<ArduinoJson::JsonString>();
    const bool statusOk =
        textEquals(status, "complete", 8) ||
        textEquals(status, "interrupted", 11) ||
        textEquals(status, "duration_limit", 14) ||
        textEquals(status, "storage_error", 13) ||
        textEquals(status, "corrupt", 7);
    if (!statusOk) {
      return false;
    }
    if (!hasMember(obj, "errorCode")) {
      return false;
    }
    if (!obj["errorCode"].isNull()) {
      if (!obj["errorCode"].is<const char*>() ||
          !errorCodeKnown(obj["errorCode"].as<ArduinoJson::JsonString>())) {
        return false;
      }
    }
    uint32_t dropped = 0;
    return hasMember(obj, "droppedAcquired") &&
           readUint32(obj["droppedAcquired"], 0.0, 4294967295.0, dropped);
  }

  if (textEquals(type, "marker.deleted", 14)) {
    out.kind = ParsedEvent::MarkerDeleted;
    // Tombstones carry no timestamp or note; none may be invented later.
    if (hasMember(obj, "ms") || hasMember(obj, "note") ||
        hasMember(obj, "timeBasis")) {
      return false;
    }
    if (!readIdentifier(obj["operationId"], out.operationId) ||
        !readIdentifier(obj["id"], out.id)) {
      return false;
    }
    return hasMember(obj, "baseRevision") &&
           readUint32(obj["baseRevision"], 1.0, 4294967295.0, out.baseRevision);
  }

  if (textEquals(type, "marker.created", 14) ||
      textEquals(type, "marker.updated", 14)) {
    out.kind = textEquals(type, "marker.created", 14)
                   ? ParsedEvent::MarkerCreated
                   : ParsedEvent::MarkerUpdated;
    if (!readIdentifier(obj["operationId"], out.operationId) ||
        !readIdentifier(obj["id"], out.id)) {
      return false;
    }
    if (!hasMember(obj, "baseRevision") ||
        !readUint32(obj["baseRevision"], 0.0, 4294967295.0, out.baseRevision)) {
      return false;
    }
    if (out.kind == ParsedEvent::MarkerCreated && out.baseRevision != 0) {
      return false;
    }
    if (out.kind == ParsedEvent::MarkerUpdated && out.baseRevision < 1) {
      return false;
    }
    if (!hasMember(obj, "timeBasis") || !obj["timeBasis"].is<const char*>()) {
      return false;
    }
    ArduinoJson::JsonString basis =
        obj["timeBasis"].as<ArduinoJson::JsonString>();
    if (textEquals(basis, "estimated", 9)) {
      out.timeBasis = TimeBasis::Estimated;
    } else if (textEquals(basis, "manual", 6)) {
      out.timeBasis = TimeBasis::Manual;
    } else if (textEquals(basis, "untimed", 7)) {
      out.timeBasis = TimeBasis::Untimed;
    } else {
      return false;
    }
    if (!hasMember(obj, "ms")) {
      return false;
    }
    if (obj["ms"].isNull()) {
      if (out.timeBasis != TimeBasis::Untimed) {
        return false;  // only untimed carries ms:null
      }
      out.hasMs = false;
    } else {
      if (out.timeBasis == TimeBasis::Untimed) {
        return false;  // untimed marker events must carry ms:null
      }
      if (!readUint32(obj["ms"], 0.0, static_cast<double>(kMaxEventMs),
                      out.ms)) {
        return false;
      }
      out.hasMs = true;
    }
    if (!hasMember(obj, "note") || !obj["note"].is<const char*>()) {
      return false;
    }
    out.note = obj["note"].as<ArduinoJson::JsonString>();
    return validNote(out.note.c_str(), out.note.size());
  }

  return false;  // unknown event type
}

// --------------------------------------------------------- index access ----

struct SlotLookup {
  int slot = -1;   // occupied slot when found, reserved empty slot otherwise
  bool found = false;
  bool ok = true;  // false: indexed bytes failed to decode (corruption)
};

// Linear probe over the compact (offset,hash) index. Hash collisions are
// resolved by decoding the candidate event's id field and comparing it.
// Results: found=true -> occupied slot; found=false with slot>=0 -> an empty
// slot reserved for insertion (reserveEmpty only); found=false with slot<0 ->
// absent, or index exhausted when insertion was requested.
SlotLookup findSlot(const uint32_t* index, const char* key,
                    const uint8_t* journal, size_t available,
                    Tables& tables, bool wantOperation,
                    bool reserveEmpty) {
  SlotLookup result;
  const uint32_t h = idHash(key);
  const uint32_t hash14 = (h >> 18) & 0x3FFF;
  const size_t start = h & kIndexMask;
  for (size_t probe = 0; probe < kIndexSlots; ++probe) {
    const size_t slot = (start + probe) & kIndexMask;
    const uint32_t entry = index[slot];
    if (entry == 0) {
      if (reserveEmpty) {
        result.slot = static_cast<int>(slot);
      }
      return result;
    }
    if (((entry >> 18) & 0x3FFF) != hash14) {
      continue;
    }
    const size_t frameOffset = entry & 0x3FFFF;
    eventjournalcodec::FrameView view;
    if (frameOffset >= available ||
        !eventjournalcodec::decodeFrame(journal + frameOffset,
                                        available - frameOffset, view)) {
      result.ok = false;
      return result;
    }
    ParsedEvent candidate;
    if (!extractEvent(view.payload, view.payloadBytes, tables, candidate)) {
      result.ok = false;
      return result;
    }
    const char* candidateKey =
        wantOperation ? candidate.operationId : candidate.id;
    if (strcmp(candidateKey, key) == 0) {
      result.slot = static_cast<int>(slot);
      result.found = true;
      return result;
    }
  }
  // Probe exhausted without an empty slot: within the journal byte budget the
  // 4096-entry index cannot fill (every valid event payload is >= 64 bytes),
  // so this is resource exhaustion rather than a semantic violation.
  return result;
}

uint32_t makeEntry(size_t frameOffset, const char* key) {
  return static_cast<uint32_t>(frameOffset) |
         (((idHash(key) >> 18) & 0x3FFF) << 18);
}

// ------------------------------------------------------------- helpers -----

bool copyNote(ArduinoJson::JsonString note, char out[2001]) {
  const size_t n = note.size();
  if (n > runlog::kMaxNoteBytes) {
    return false;
  }
  if (n == 0) {
    out[0] = '\0';
    return true;
  }
  memcpy(out, note.c_str(), n);
  out[n] = '\0';
  return true;
}

}  // namespace markerreducer_detail

using namespace markerreducer_detail;

// ------------------------------------------------------ MarkerReducer ------

MarkerReducer::~MarkerReducer() {
  if (tables_ != nullptr) {
    tables_->~Tables();
    freeBlock(tables_);
    tables_ = nullptr;
  }
}

ArchiveError MarkerReducer::ensureTables() const {
  if (tables_ != nullptr) {
    return ArchiveError::NONE;
  }
  void* block = allocBlock(sizeof(Tables));
  if (block == nullptr) {
    return ArchiveError::STORAGE_UNAVAILABLE;
  }
  tables_ = new (block) Tables();
  return ArchiveError::NONE;
}

void MarkerReducer::reset(const uint8_t* validatedJournal, size_t bytes) {
  journal_ = validatedJournal;
  available_ = bytes;
  consumed_ = 0;
  runId_ = 0;
  revision_ = 0;
  annotationCount_ = 0;
  error_ = ArchiveError::NONE;
  headerSeen_ = false;
  started_ = false;
  ended_ = false;
  if (validatedJournal == nullptr && bytes != 0) {
    error_ = ArchiveError::INVALID_REQUEST;
    available_ = 0;
  } else if (bytes > kMaxJournalBytes) {
    error_ = ArchiveError::INVALID_REQUEST;
    available_ = 0;
  }
  // Index/JSON scratch allocations are reused; the stale entries themselves
  // must not survive into the new binding.
  if (tables_ != nullptr) {
    memset(tables_->markerIndex, 0, sizeof(tables_->markerIndex));
    memset(tables_->opIndex, 0, sizeof(tables_->opIndex));
  }
}

ArchiveError MarkerReducer::extendValidatedPrefix(size_t bytes) {
  if (error_ != ArchiveError::NONE) {
    return error_;
  }
  if (journal_ == nullptr || bytes > kMaxJournalBytes || bytes < available_) {
    return ArchiveError::INVALID_REQUEST;
  }
  available_ = bytes;
  return ArchiveError::NONE;
}

ArchiveError MarkerReducer::ingestNext(size_t maxFrames) {
  if (error_ != ArchiveError::NONE) {
    return error_;  // sticky until reset()
  }
  const size_t bound = maxFrames < kMaxIngestFrames ? maxFrames : kMaxIngestFrames;
  if (bound == 0 || journal_ == nullptr) {
    return ArchiveError::NONE;  // no progress, no state change
  }
  const ArchiveError allocError = ensureTables();
  if (allocError != ArchiveError::NONE) {
    error_ = allocError;
    return error_;
  }

  size_t steps = 0;
  while (steps < bound && consumed_ < available_) {
    if (!headerSeen_) {
      eventjournalcodec::Header header;
      if (!eventjournalcodec::decodeHeader(journal_ + consumed_,
                                           available_ - consumed_, header)) {
        error_ = ArchiveError::CORRUPT_RUN;
        return error_;
      }
      runId_ = header.runId;
      headerSeen_ = true;
      consumed_ += eventjournalcodec::kHeaderSize;
      continue;  // the header is not a frame step
    }

    eventjournalcodec::FrameView view;
    if (!eventjournalcodec::decodeFrame(journal_ + consumed_,
                                        available_ - consumed_, view)) {
      error_ = ArchiveError::CORRUPT_RUN;
      return error_;
    }
    if (!JsonEncodingValidator(view.payload, view.payloadBytes)
             .validateDocument()) {
      error_ = ArchiveError::CORRUPT_RUN;
      return error_;
    }

    // Full extraction validates every field exactly (client parseEvent
    // rules). The scalar copies in `event` survive later scratch reuse;
    // event.note borrows the scratch and must not be read after the probe
    // decodes below (ingest never needs the note text).
    const size_t frameOffset = consumed_;
    ParsedEvent event;
    if (!extractEvent(view.payload, view.payloadBytes, *tables_, event)) {
      error_ = ArchiveError::CORRUPT_RUN;
      return error_;
    }
    const ParsedEvent::Kind kind = event.kind;
    const uint32_t revision = event.revision;
    const uint32_t baseRevision = event.baseRevision;
    char operationId[81];
    char markerId[81];
    memcpy(operationId, event.operationId, sizeof(operationId));
    memcpy(markerId, event.id, sizeof(markerId));

    SlotLookup markerSlot;
    SlotLookup opSlot;
    uint32_t priorRevision = 0;
    bool priorDeleted = false;

    const bool isMarker = kind == ParsedEvent::MarkerCreated ||
                          kind == ParsedEvent::MarkerUpdated ||
                          kind == ParsedEvent::MarkerDeleted;
    if (isMarker) {
      opSlot = findSlot(tables_->opIndex, operationId, journal_, available_,
                        *tables_, true, true);
      if (!opSlot.ok) {
        error_ = ArchiveError::CORRUPT_RUN;
        return error_;
      }
      if (opSlot.found || opSlot.slot < 0) {
        // Duplicate operationId, or index exhausted (unreachable in budget).
        error_ = opSlot.found ? ArchiveError::CORRUPT_RUN
                              : ArchiveError::STORAGE_UNAVAILABLE;
        return error_;
      }
      markerSlot = findSlot(tables_->markerIndex, markerId, journal_,
                            available_, *tables_, false, true);
      if (!markerSlot.ok) {
        error_ = ArchiveError::CORRUPT_RUN;
        return error_;
      }
      if (!markerSlot.found && markerSlot.slot < 0) {
        error_ = ArchiveError::STORAGE_UNAVAILABLE;
        return error_;
      }
      if (markerSlot.found) {
        // Decode the marker's latest mutation for the baseRevision check.
        const uint32_t priorEntry =
            tables_->markerIndex[static_cast<size_t>(markerSlot.slot)];
        eventjournalcodec::FrameView priorView;
        if (!eventjournalcodec::decodeFrame(
                journal_ + (priorEntry & 0x3FFFF),
                available_ - (priorEntry & 0x3FFFF), priorView)) {
          error_ = ArchiveError::CORRUPT_RUN;
          return error_;
        }
        ParsedEvent prior;
        if (!extractEvent(priorView.payload, priorView.payloadBytes,
                          *tables_, prior)) {
          error_ = ArchiveError::CORRUPT_RUN;
          return error_;
        }
        priorRevision = prior.revision;
        priorDeleted = prior.kind == ParsedEvent::MarkerDeleted;
      }
    }

    // Lifecycle and transition rules. Everything is checked before commit.
    bool accept = false;
    uint32_t nextAnnotationCount = annotationCount_;
    if (revision != revision_ + 1) {
      accept = false;
    } else if (!started_) {
      accept = kind == ParsedEvent::RunStarted;
    } else if (kind == ParsedEvent::RunStarted) {
      accept = false;  // run.started only at revision 1
    } else if (kind == ParsedEvent::RunEnded) {
      accept = !ended_;  // end at most once; markers may follow
    } else if (kind == ParsedEvent::MarkerCreated) {
      accept = !markerSlot.found && baseRevision == 0;
      if (accept) {
        nextAnnotationCount = annotationCount_ + 1;
      }
    } else if (kind == ParsedEvent::MarkerUpdated) {
      accept = markerSlot.found && !priorDeleted && baseRevision == priorRevision;
    } else {  // MarkerDeleted
      accept = markerSlot.found && !priorDeleted && baseRevision == priorRevision;
      if (accept) {
        nextAnnotationCount = annotationCount_ - 1;
      }
    }
    if (!accept) {
      error_ = ArchiveError::CORRUPT_RUN;
      return error_;
    }

    // Commit (no partial event: all validation above succeeded).
    if (isMarker) {
      tables_->opIndex[static_cast<size_t>(opSlot.slot)] = makeEntry(
          frameOffset, operationId);
      tables_->markerIndex[static_cast<size_t>(markerSlot.slot)] =
          makeEntry(frameOffset, markerId);
    }
    annotationCount_ = nextAnnotationCount;
    revision_ = revision;
    if (kind == ParsedEvent::RunStarted) {
      started_ = true;
    } else if (kind == ParsedEvent::RunEnded) {
      ended_ = true;
    }
    consumed_ += view.frameBytes;
    ++steps;
  }
  return ArchiveError::NONE;
}

bool MarkerReducer::ready() const {
  return journal_ != nullptr && error_ == ArchiveError::NONE && headerSeen_ &&
         consumed_ == available_;
}

bool MarkerReducer::lookupMarker(const char* id, MarkerState& out) const {
  if (id == nullptr || tables_ == nullptr || !headerSeen_) {
    return false;
  }
  SlotLookup slot = findSlot(tables_->markerIndex, id, journal_, available_,
                             *tables_, false, false);
  if (!slot.ok || !slot.found) {
    return false;
  }
  const uint32_t entry = tables_->markerIndex[static_cast<size_t>(slot.slot)];
  eventjournalcodec::FrameView view;
  if (!eventjournalcodec::decodeFrame(journal_ + (entry & 0x3FFFF),
                                      available_ - (entry & 0x3FFFF), view)) {
    return false;
  }
  ParsedEvent event;
  if (!extractEvent(view.payload, view.payloadBytes, *tables_, event)) {
    return false;
  }
  MarkerState state;
  memcpy(state.id, event.id, strlen(event.id) + 1);
  state.revision = event.revision;
  state.deleted = event.kind == ParsedEvent::MarkerDeleted;
  state.hasMs = false;
  state.ms = 0;
  state.note[0] = '\0';
  state.timeBasis = TimeBasis::Untimed;
  if (!state.deleted) {
    state.hasMs = event.hasMs;
    state.ms = event.ms;
    state.timeBasis = event.timeBasis;
    if (!copyNote(event.note, state.note)) {
      return false;
    }
  }
  out = state;
  return true;
}

bool MarkerReducer::lookupOperation(const char* operationId,
                                    MarkerMutation& original,
                                    MutationResult& result) const {
  if (operationId == nullptr || tables_ == nullptr || !headerSeen_) {
    return false;
  }
  SlotLookup slot = findSlot(tables_->opIndex, operationId, journal_,
                             available_, *tables_, true, false);
  if (!slot.ok || !slot.found) {
    return false;
  }
  const uint32_t entry = tables_->opIndex[static_cast<size_t>(slot.slot)];
  eventjournalcodec::FrameView view;
  if (!eventjournalcodec::decodeFrame(journal_ + (entry & 0x3FFFF),
                                      available_ - (entry & 0x3FFFF), view)) {
    return false;
  }
  ParsedEvent event;
  if (!extractEvent(view.payload, view.payloadBytes, *tables_, event)) {
    return false;
  }
  MarkerMutation mutation;
  MutationResult outcome;
  switch (event.kind) {
    case ParsedEvent::MarkerCreated:
      mutation.kind = MarkerMutationKind::Create;
      break;
    case ParsedEvent::MarkerUpdated:
      mutation.kind = MarkerMutationKind::Update;
      break;
    default:
      mutation.kind = MarkerMutationKind::Delete;
      break;
  }
  memcpy(mutation.operationId, event.operationId,
         strlen(event.operationId) + 1);
  memcpy(mutation.id, event.id, strlen(event.id) + 1);
  mutation.baseRevision = event.baseRevision;
  mutation.hasMs = false;
  mutation.ms = 0;
  mutation.note[0] = '\0';
  mutation.timeBasis = TimeBasis::Untimed;
  if (event.kind != ParsedEvent::MarkerDeleted) {
    mutation.hasMs = event.hasMs;
    mutation.ms = event.ms;
    mutation.timeBasis = event.timeBasis;
    if (!copyNote(event.note, mutation.note)) {
      return false;
    }
  }
  outcome.runId = runId_;
  memcpy(outcome.operationId, event.operationId,
         strlen(event.operationId) + 1);
  memcpy(outcome.id, event.id, strlen(event.id) + 1);
  outcome.revision = event.revision;
  outcome.persisted = true;
  outcome.deleted = event.kind == ParsedEvent::MarkerDeleted;
  original = mutation;
  result = outcome;
  return true;
}

uint32_t MarkerReducer::annotationCount() const {
  return annotationCount_;
}

uint32_t MarkerReducer::revision() const {
  return revision_;
}
