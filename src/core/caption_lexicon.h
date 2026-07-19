// Caption marker lexicon distilled from docs/research (59 unified markers,
// 4 channels, 381 scored segments). Generated table lives in
// caption_lexicon.cpp — regenerate with tools/gen_lexicon.py if the research
// lexicon (docs/research/crux-lexicon.tsv) changes.
#pragma once

#include <string>
#include <vector>

namespace crux::captions {

enum class MarkerRole {
    Pre,       // announces a reveal/story — crux payload follows within 15-60 s
    Payload,   // flanks the payload itself (oaths, stun-caps)
    Trailing,  // reaction/terminator — crux precedes; end-boundary anchor
    Negative   // promo/CTA — suppress any overlapping window
};

struct Marker {
    const char* pattern;   // lowercase for Latin; UTF-8 Devanagari as-is
    MarkerRole role;
    int weight;            // 1..3 (3 = reliably precedes a high-scoring crux)
    const char* signal;    // research signal class, for reporting
};

const std::vector<Marker>& default_lexicon();

// Magnitude tokens that turn an adjacent digit run into a numeric payload
// (research signal #2, strength 9/10).
const std::vector<const char*>& magnitude_tokens();

} // namespace crux::captions
