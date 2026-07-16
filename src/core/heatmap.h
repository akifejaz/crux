// Pure data types for the heatmap → plan pipeline. No I/O.
#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace crux {

// YouTube heatmap is always 100 bins (FINDINGS §3.1). We enforce that here.
inline constexpr std::size_t kBinCount = 100;

struct Chapter {
    double start_sec = 0.0;
    double end_sec = 0.0;
    std::string title;
};

struct VideoMeta {
    std::string id;
    std::string title;
    std::string channel;
    std::string url;
    double duration_sec = 0.0;
    bool is_live = false;
};

// Raw heatmap as returned by yt-dlp: 100 spans with normalized value 0..1.
struct HeatmapBin {
    double start_sec = 0.0;
    double end_sec = 0.0;
    double value = 0.0;    // intensityScoreNormalized 0..1
};

struct Heatmap {
    std::array<HeatmapBin, kBinCount> bins{};
    double bin_seconds = 0.0;   // duration_sec / 100 (derived from durationMillis)
};

struct FetchResult {
    VideoMeta video{};
    std::optional<Heatmap> heatmap;   // std::nullopt when unavailable
    std::vector<Chapter> chapters{};
};

// A planned clip.
struct Clip {
    int index = 1;
    double start_sec = 0.0;
    double end_sec = 0.0;
    double peak_score = 0.0;
    int    peak_bin = -1;
    double centroid_sec = 0.0;
    std::string label;              // may be empty
    bool snapped_start = false;
    bool snapped_end = false;
    std::string file;               // filled by media stage
};

// Quality gate on the profile (PLAN §4 step 4).
struct Quality {
    double max_over_median = 0.0;
    bool flat = false;              // true when max/median < flat_threshold
};

struct Plan {
    std::vector<Clip> clips;
    Quality quality;
};

} // namespace crux
