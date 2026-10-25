#ifndef MARKER_REDUCER_H
#define MARKER_REDUCER_H
#include "RunLogTypes.h"
// Bounded journal reduction. Caller retains immutable CRC-validated bytes.

namespace markerreducer_detail {
struct Tables;  // index + JSON scratch block, defined in MarkerReducer.cpp
}
//
// Input model (whole-journal, caller-owned):
//   reset() binds ONE caller-owned buffer containing the COMPLETE PVE1 event
//   journal: the 16-byte EventJournalCodec header followed by framed UTF-8
//   JSON events (events.bin). The bytes are immutable for the lifetime of the
//   binding (they are the store's single journal cache, <= 262144 bytes) and
//   are never copied or retained beyond the binding; all lookups re-decode
//   from them. `bytes` is the current validated prefix length and must end at
//   a complete frame boundary. reset() drops all reduced state (revisions,
//   counts, both indexes, lifecycle flags and any latched error) and reuses
//   the previously allocated index/JSON scratch.
//
//   extendValidatedPrefix() grows the claimed prefix over the SAME buffer;
//   it only grows (append-only journal) up to 262144 bytes
//   (RUN_EVENT_BUDGET_BYTES), still at complete frame boundaries. It is a
//   pure watermark: validation happens during ingestion.
//
//   ingestNext() reduces at most min(maxFrames, 8) frames per call. Each
//   consumed unit is fully validated before any state is touched (no
//   partially applied event): header (magic/version/runId/CRC), frame
//   (bounds/CRC/UTF-8), strict JSON encoding (RFC 8259 strings, escapes,
//   numbers, unique keys; invalid or escaped Unicode, escaped NUL and raw
//   NUL are rejected), then exact journal semantics mirroring the client
//   run-protocol.js parseEvent plus transition rules: consecutive global
//   revisions from 1, run.started only at revision 1 with active status/null
//   error/zero dropped, run.ended at most once (markers may follow it),
//   marker.created with baseRevision 0 for an unknown id, marker.updated /
//   marker.deleted with baseRevision equal to that marker's latest mutation
//   revision on a live marker, unique operationIds, ids 1..80 ASCII
//   [A-Za-z0-9_-], notes <= 500 Unicode scalars / 2000 UTF-8 bytes with the
//   stored text preserved exactly (never trimmed).
//
// Errors: invalid header or invalid frame tail (CRC, UTF-8, truncation) and
// any semantic violation latch ArchiveError::CORRUPT_RUN; failure to
// allocate the index/JSON block latches ArchiveError::STORAGE_UNAVAILABLE;
// inconsistent reset/extend arguments report ArchiveError::INVALID_REQUEST.
// A latched error is sticky until reset(); it keeps the previous valid
// prefix's counts/revision/indexes intact and leaves ready() false.
//
// ready() is true when a bound journal has been reduced through its whole
// claimed prefix with no error (a header-only journal reduces to revision 0).
// Lookups reflect the reduced prefix regardless of readiness; they return
// false and leave their outputs COMPLETELY untouched when the id/operationId
// is unknown. lookupMarker() reports tombstones with deleted=true (no
// invented timestamp/note); lookupOperation() re-decodes the exact original
// domain payload into the MarkerMutation plus its persisted MutationResult,
// whose runId comes from the journal header.
//
// Memory: the reducer owns one lazily allocated block (explicit PSRAM on
// ESP32, portable malloc elsewhere) holding two fixed 4096-entry compact
// offset/hash indexes (marker and operation; hash collisions are resolved by
// decoding and comparing the stored ids) plus one bounded reusable ArduinoJson
// arena (custom bump allocator, rewound per event; no per-event heap
// allocation). No note text or journal copy is stored per index entry; notes
// decode into caller-owned output only. The block is freed by the destructor,
// so MarkerReducer is intentionally noncopyable.
class MarkerReducer {
public:
    MarkerReducer() = default;
    ~MarkerReducer();
    MarkerReducer(const MarkerReducer&) = delete;
    MarkerReducer& operator=(const MarkerReducer&) = delete;

    void reset(const uint8_t* validatedJournal, size_t bytes);
    ArchiveError extendValidatedPrefix(size_t bytes);
    ArchiveError ingestNext(size_t maxFrames);
    bool ready() const;
    bool lookupMarker(const char* id, MarkerState& out) const;
    bool lookupOperation(const char* operationId, MarkerMutation& original, MutationResult& result) const;
    uint32_t annotationCount() const;
    uint32_t revision() const;

private:
    ArchiveError ensureTables() const;

    const uint8_t* journal_ = nullptr;
    size_t available_ = 0;   // claimed validated prefix length
    size_t consumed_ = 0;    // next unconsumed byte (header + whole frames)
    uint32_t runId_ = 0;     // from the journal header
    uint32_t revision_ = 0;  // latest reduced global revision
    uint32_t annotationCount_ = 0;
    ArchiveError error_ = ArchiveError::NONE;
    bool headerSeen_ = false;
    bool started_ = false;
    bool ended_ = false;
    mutable markerreducer_detail::Tables* tables_ = nullptr;
};
#endif
