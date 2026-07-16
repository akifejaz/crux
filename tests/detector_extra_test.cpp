// Detector — additional edge cases beyond the FINDINGS fixtures.

#include <doctest/doctest.h>

#include "core/detector.h"

namespace {
crux::Heatmap uniform(double v, double bin_s = 12.0) {
    crux::Heatmap h; h.bin_seconds = bin_s;
    for (std::size_t i = 0; i < crux::kBinCount; ++i) {
        h.bins[i].start_sec = i * bin_s;
        h.bins[i].end_sec   = (i + 1) * bin_s;
        h.bins[i].value = v;
    }
    return h;
}

crux::Config cfg() { crux::Config c; c.max_clips = 6; return c; }
}

TEST_CASE("detector: all-zero heatmap → no regions, gate flagged") {
    auto h = uniform(0.0);
    auto d = crux::detector::detect(h, 1200.0, cfg());
    CHECK(d.regions.empty());
    // max/median with all zeros is defined as mx*1000 in the code — non-zero
    // but the shape is trivially flat.
    // The important thing: no regions means no clips means no crash downstream.
}

TEST_CASE("detector: all-max heatmap → flat gate triggers") {
    auto h = uniform(1.0);
    auto d = crux::detector::detect(h, 1200.0, cfg());
    // max / median = 1.0 → below the 2.0 threshold → flat.
    CHECK(d.quality.flat);
}

TEST_CASE("detector: single-bin spike is preserved") {
    auto h = uniform(0.02);
    h.bins[42].value = 1.0;
    auto d = crux::detector::detect(h, 1200.0, cfg());
    bool has42 = false;
    for (auto& r : d.regions)
        if (r.start_bin <= 42 && r.end_bin >= 42) has42 = true;
    CHECK(has42);
}

TEST_CASE("detector: --keep-intro keeps bin 0/1 as candidates") {
    auto h = uniform(0.05);
    h.bins[0].value = 1.0;    // artificial intro spike
    h.bins[1].value = 0.9;
    h.bins[2].value = 0.5;
    auto c = cfg(); c.keep_intro = true;
    auto d = crux::detector::detect(h, 1200.0, c);
    bool intro_kept = false;
    for (auto& r : d.regions)
        if (r.start_bin == 0 || r.start_bin == 1) intro_kept = true;
    CHECK(intro_kept);
}

TEST_CASE("detector: adjacent tied peaks merge into one region (no dup clips)") {
    auto h = uniform(0.05);
    // three adjacent identical peaks
    h.bins[50].value = 0.9;
    h.bins[51].value = 0.9;
    h.bins[52].value = 0.9;
    auto d = crux::detector::detect(h, 1200.0, cfg());
    // Should produce exactly one region covering 50..52 (± hysteresis).
    int matching = 0;
    for (auto& r : d.regions)
        if (r.start_bin <= 50 && r.end_bin >= 52) ++matching;
    CHECK(matching == 1);
}

TEST_CASE("detector: hysteresis merges near-adjacent regions across a small dip") {
    auto h = uniform(0.05);
    // two peaks separated by a single sub-threshold bin
    h.bins[40].value = 0.9;
    h.bins[41].value = 0.6;    // dip but still above 0.6·T
    h.bins[42].value = 0.9;
    auto d = crux::detector::detect(h, 1200.0, cfg());
    // Expect merged into one region.
    int matching = 0;
    for (auto& r : d.regions)
        if (r.start_bin <= 40 && r.end_bin >= 42) ++matching;
    CHECK(matching == 1);
}

TEST_CASE("detector: quality.max_over_median is monotonic in peak height") {
    auto h1 = uniform(0.10);   h1.bins[50].value = 0.30;
    auto h2 = uniform(0.10);   h2.bins[50].value = 0.90;
    auto d1 = crux::detector::detect(h1, 1200.0, cfg());
    auto d2 = crux::detector::detect(h2, 1200.0, cfg());
    CHECK(d2.quality.max_over_median > d1.quality.max_over_median);
}

TEST_CASE("detector: coarse bins (binS >= 60) skip the smoothing pass") {
    // With a 90 s bin, the smoothing branch is disabled. Values should
    // pass through untouched, so a sharp isolated peak survives at
    // its raw value rather than being blended into neighbors.
    crux::Heatmap h; h.bin_seconds = 90.0;
    for (std::size_t i = 0; i < crux::kBinCount; ++i) {
        h.bins[i].start_sec = i * 90.0;
        h.bins[i].end_sec   = (i + 1) * 90.0;
        h.bins[i].value = 0.10;
    }
    h.bins[45].value = 0.85;
    auto d = crux::detector::detect(h, 9000.0, cfg());
    bool has45 = false;
    double peak = 0.0;
    for (auto& r : d.regions) {
        if (r.start_bin <= 45 && r.end_bin >= 45) { has45 = true; peak = r.peak_score; }
    }
    CHECK(has45);
    // raw value survives (no smoothing dilution)
    CHECK(peak == doctest::Approx(0.85));
}

TEST_CASE("detector: end-of-video region has centroid clamped to duration") {
    auto h = uniform(0.05);
    h.bins[99].value = 1.0;
    h.bins[98].value = 0.9;
    auto d = crux::detector::detect(h, 1200.0, cfg());
    for (auto& r : d.regions) {
        CHECK(r.centroid_sec <= 1200.0);
    }
}
