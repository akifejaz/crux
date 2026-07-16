#include <doctest/doctest.h>

#include "core/detector.h"
#include "core/planner.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

crux::Config default_cfg() { crux::Config c; c.max_clips = 6; return c; }

crux::Heatmap make_flat_high(double val, double bin_s) {
    crux::Heatmap h; h.bin_seconds = bin_s;
    for (std::size_t i = 0; i < crux::kBinCount; ++i) {
        h.bins[i].start_sec = i * bin_s;
        h.bins[i].end_sec   = (i + 1) * bin_s;
        h.bins[i].value = val;
    }
    return h;
}

crux::Heatmap single_peak_short() {
    // Karate-Kid-like: cluster 71..78 peaks near bin 75.
    crux::Heatmap h; h.bin_seconds = 2.4;
    for (std::size_t i = 0; i < crux::kBinCount; ++i) {
        h.bins[i].start_sec = i * 2.4;
        h.bins[i].end_sec   = (i + 1) * 2.4;
        h.bins[i].value = 0.10;
    }
    double peak[] = {0.55, 0.70, 0.85, 0.95, 1.00, 0.90, 0.80, 0.70};
    for (int k = 0; k < 8; ++k) h.bins[71 + k].value = peak[k];
    return h;
}

}

TEST_CASE("planner: short video with single region emits one clamped clip") {
    auto h = single_peak_short();
    double dur = crux::kBinCount * h.bin_seconds; // 240s
    auto cfg = default_cfg();
    auto det = crux::detector::detect(h, dur, cfg);
    auto p = crux::planner::plan(det, h, dur, {}, cfg);
    REQUIRE(p.clips.size() >= 1);
    // Duration-constrained: clip should be roughly 40..90s and inside video.
    for (const auto& c : p.clips) {
        CHECK(c.start_sec >= 0.0);
        CHECK(c.end_sec   <= dur + 0.001);
        CHECK(c.end_sec - c.start_sec <= 90.001);
    }
}

TEST_CASE("planner: flat-high (music) profile flagged") {
    auto h = make_flat_high(0.6, 2.8);
    auto cfg = default_cfg();
    auto det = crux::detector::detect(h, 280.0, cfg);
    auto p = crux::planner::plan(det, h, 280.0, {}, cfg);
    CHECK(p.quality.flat);
}

TEST_CASE("planner: greedy min-gap keeps regions well-separated") {
    // Build synthetic: three sharp peaks at bins 20, 22, 60. 20 and 22 are
    // adjacent enough to be merged into one region already, but even if they
    // weren't, min-gap should keep at most one of them.
    crux::Heatmap h; h.bin_seconds = 30.0;    // 3000s video, 30s bins
    for (std::size_t i = 0; i < crux::kBinCount; ++i) {
        h.bins[i].start_sec = i * 30.0;
        h.bins[i].end_sec   = (i + 1) * 30.0;
        h.bins[i].value = 0.05;
    }
    h.bins[20].value = 1.0;
    h.bins[22].value = 0.9;
    h.bins[60].value = 0.85;
    auto cfg = default_cfg();
    auto det = crux::detector::detect(h, 3000.0, cfg);
    auto p = crux::planner::plan(det, h, 3000.0, {}, cfg);
    // Adjacent close peaks should collapse to fewer selected clips than raw peaks.
    // We expect two distinct clips (20-region + 60-region).
    CHECK(p.clips.size() <= 2);
}

TEST_CASE("planner: clip windows clamp by shift near end of video") {
    crux::Heatmap h; h.bin_seconds = 30.0;
    for (std::size_t i = 0; i < crux::kBinCount; ++i) {
        h.bins[i].start_sec = i * 30.0;
        h.bins[i].end_sec   = (i + 1) * 30.0;
        h.bins[i].value = 0.05;
    }
    // bin 99 is the strong peak
    h.bins[99].value = 1.0;
    h.bins[98].value = 0.9;
    auto cfg = default_cfg();
    auto det = crux::detector::detect(h, 3000.0, cfg);
    auto p = crux::planner::plan(det, h, 3000.0, {}, cfg);
    REQUIRE(!p.clips.empty());
    for (const auto& c : p.clips) CHECK(c.end_sec <= 3000.001);
}
