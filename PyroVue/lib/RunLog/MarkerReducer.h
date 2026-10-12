#ifndef MARKER_REDUCER_H
#define MARKER_REDUCER_H
#include "RunLogTypes.h"
// M2 implementation contract. Caller retains immutable CRC-validated journal bytes.
class MarkerReducer {
public:
    void reset(const uint8_t* validatedJournal, size_t bytes);
    ArchiveError extendValidatedPrefix(size_t bytes);
    ArchiveError ingestNext(size_t maxFrames);
    bool ready() const;
    bool lookupMarker(const char* id, MarkerState& out) const;
    bool lookupOperation(const char* operationId, MarkerMutation& original, MutationResult& result) const;
    uint32_t annotationCount() const;
    uint32_t revision() const;
};
#endif
