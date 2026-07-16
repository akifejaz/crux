#pragma once

#include "core/heatmap.h"
#include "config.h"

#include <vector>

namespace ytshorts::detector {

struct Region {
    int start_bin = 0;
    int end_bin = 0;              // inclusive
    double peak_score = 0.0;
    int peak_bin = 0;
    double centroid_sec = 0.0;
};

struct DetectResult {
    std::vector<Region> regions;
    Quality quality;
};

// Runs steps 1..7 of PLAN §4 (clean, smooth, stats, gate, thresh, regions, score).
// Pure function — no I/O. `bin_seconds` must come from the raw durationMillis.
DetectResult detect(const Heatmap& h,
                    double duration_sec,
                    const Config& cfg);

} // namespace ytshorts::detector
