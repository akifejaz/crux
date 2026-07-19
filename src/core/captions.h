// Pure data types for the caption → crux-score pipeline. No I/O.
// Research basis: docs/research/README.md (381 segments, 4 channels).
#pragma once

#include <array>
#include <string>
#include <vector>

#include "core/heatmap.h"   // kBinCount

namespace crux {

// One subtitle cue after parsing + rolling-line dedupe.
struct CaptionCue {
    double start_sec = 0.0;
    double end_sec = 0.0;
    std::string text;        // original script (UTF-8), tags stripped
};

struct CaptionDoc {
    std::vector<CaptionCue> cues;
    std::string lang;        // e.g. "hi", "ur", "en", "en-orig"
};

// A caption-detected crux candidate (30-60 s window).
struct CruxCandidate {
    double start_sec = 0.0;
    double end_sec = 0.0;
    double score = 0.0;              // raw tier-1 score (pre-normalization)
    std::string hook_text;           // cue text at the strongest marker/payload
    std::vector<std::string> signals; // matched signal names, deduped
    bool four_beat = false;          // pre + payload + trailing all present in order
    bool cold_open_match = false;    // window replays a cold-open teaser line
};

struct CaptionScore {
    // Per-bin score resampled onto the 100-bin heatmap grid, normalized 0..1.
    std::array<double, kBinCount> bins{};
    std::vector<CruxCandidate> candidates;  // NMS'ed, sorted by score desc
    bool usable = false;             // false when too few cues to score
};

} // namespace crux
