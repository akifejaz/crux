// Standalone verification harness. Uses no external libraries — just
// standard C++20 — so it can be built with plain g++ in environments where
// vcpkg / FetchContent are unavailable. Duplicates the FINDINGS §4 arrays
// inline to avoid a JSON dependency.
//
// Build:
//   g++ -std=c++20 -Isrc tests/standalone_verify.cpp \
//       src/core/detector.cpp src/core/planner.cpp -o verify
// Run:
//   ./verify

#include "core/detector.h"
#include "core/planner.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using ytshorts::Heatmap;
using ytshorts::Config;

namespace {

struct Fx {
    std::string id;
    double len_sec;
    double bin_ms;
    double scale;
    std::vector<int> scores;
};

Heatmap to_h(const Fx& f) {
    Heatmap h;
    h.bin_seconds = f.bin_ms / 1000.0;
    for (std::size_t i = 0; i < ytshorts::kBinCount; ++i) {
        h.bins[i].start_sec = i * h.bin_seconds;
        h.bins[i].end_sec   = (i + 1) * h.bin_seconds;
        h.bins[i].value     = f.scores[i] / f.scale;
    }
    return h;
}

Config cfg() { Config c; c.max_clips = 6; return c; }

int failures = 0;
void check(bool cond, const std::string& name) {
    if (!cond) { std::printf("  FAIL: %s\n", name.c_str()); ++failures; }
    else       { std::printf("  ok:   %s\n", name.c_str()); }
}

} // namespace

int main() {
    Fx mrbeast_squid{"0e3GPea1Tyg", 1541, 15420, 100.0, {
        100,25,16,13,14,14,18,15,6,7,5,26,12,12,14,18,17,14,9,14,14,7,21,41,10,
        9,16,15,10,11,8,6,4,8,29,34,15,14,13,13,15,10,15,12,10,10,6,12,10,47,
        10,14,12,16,16,18,18,14,16,18,17,11,11,10,9,5,18,14,1,28,14,4,3,4,4,1,
        0,1,1,16,2,11,11,11,10,9,9,11,9,10,11,12,9,6,4,3,8,22,18,29
    }};
    Fx dream{"tylNqtyj0gs", 2507, 25080, 100.0, {
        55,10,9,15,4,12,11,4,3,2,2,8,1,12,6,6,1,5,2,5,13,2,5,5,2,3,0,4,6,11,
        6,6,3,5,4,5,5,4,9,6,4,3,14,54,16,3,2,26,9,6,6,2,3,2,5,36,18,22,13,11,
        2,3,8,6,5,16,2,3,2,5,11,13,1,2,6,7,25,18,10,25,23,3,2,8,2,7,5,4,5,5,
        8,11,9,8,13,28,100,79,50,69
    }};
    Fx ted{"Ks-_Mh1QhMc", 1263, 12630, 100.0, {
        59,56,55,47,37,34,54,100,62,27,26,38,32,32,25,25,23,30,23,24,19,15,14,
        40,23,20,16,15,14,22,13,11,11,18,28,29,25,16,21,25,32,33,37,35,27,23,
        30,34,31,37,45,51,51,32,25,31,28,21,19,22,21,45,16,26,20,19,20,17,23,
        25,18,26,29,30,41,42,30,28,28,52,21,24,26,24,23,28,41,47,45,33,33,26,
        47,46,30,22,21,18,9,0
    }};
    Fx sidemen{"iIY9fPgY5wM", 1201, 12010, 100.0, {
        29,25,11,13,28,14,16,27,23,6,7,5,4,5,2,11,18,10,11,13,4,7,12,4,12,3,3,
        4,3,5,10,14,32,15,5,5,20,36,17,30,22,9,8,2,6,5,1,9,8,8,8,10,8,52,55,8,
        7,14,89,100,15,9,9,6,2,17,17,12,20,28,9,3,0,3,5,12,14,21,13,8,5,6,2,6,
        7,7,14,21,12,14,3,2,4,1,5,9,10,2,7,7
    }};
    Fx boblazar{"BEWz4SXfyCQ", 8084, 80850, 1000.0, {
        1000,76,16,0,86,239,181,125,54,129,67,80,61,90,298,299,165,158,150,
        284,114,221,175,249,108,160,148,94,74,237,170,165,229,167,111,148,137,
        182,147,192,305,117,250,134,444,220,106,166,156,219,143,224,119,138,
        194,323,153,162,187,180,185,221,226,202,156,151,152,277,153,235,283,
        150,124,244,296,115,133,60,65,112,187,363,179,148,339,304,137,115,60,
        107,168,72,75,157,112,50,38,78,19,385
    }};

    std::printf("T1: MrBeast Squid Game (%.0fs, binS=%.2fs)\n",
                mrbeast_squid.len_sec, mrbeast_squid.bin_ms / 1000.0);
    {
        auto h = to_h(mrbeast_squid);
        auto det = ytshorts::detector::detect(h, mrbeast_squid.len_sec, cfg());
        bool h23=false, h49=false;
        for (auto& r : det.regions) {
            if (r.start_bin <= 23 && r.end_bin >= 23) h23 = true;
            if (r.start_bin <= 49 && r.end_bin >= 49) h49 = true;
        }
        check(h23, "region covers bin 23 (0.41 peak)");
        check(h49, "region covers bin 49 (0.47 peak)");
        check(!det.quality.flat, "not flagged flat");
        auto p = ytshorts::planner::plan(det, h, mrbeast_squid.len_sec, {}, cfg());
        for (auto& c : p.clips) {
            check(c.start_sec >= 0.0, "clip start >= 0");
            check(c.end_sec <= mrbeast_squid.len_sec + 0.01, "clip end <= duration");
            check(c.peak_bin != 0, "no clip peaks at bin 0");
        }
        std::printf("   -> %zu regions, %zu clips\n", det.regions.size(), p.clips.size());
    }

    std::printf("T2: Dream speedrunner (%.0fs, binS=%.2fs) — end spike\n",
                dream.len_sec, dream.bin_ms / 1000.0);
    {
        auto h = to_h(dream);
        auto det = ytshorts::detector::detect(h, dream.len_sec, cfg());
        bool end_kept = false;
        for (auto& r : det.regions) if (r.end_bin >= 96) end_kept = true;
        check(end_kept, "end spike (bins 96..99) kept");
        auto p = ytshorts::planner::plan(det, h, dream.len_sec, {}, cfg());
        for (auto& c : p.clips)
            check(c.end_sec <= dream.len_sec + 0.01, "end clip clamped inside duration");
    }

    std::printf("T3: TED talk (%.0fs, binS=%.2fs) — bin0 exclusion\n",
                ted.len_sec, ted.bin_ms / 1000.0);
    {
        auto h = to_h(ted);
        auto det = ytshorts::detector::detect(h, ted.len_sec, cfg());
        bool has7 = false;
        for (auto& r : det.regions)
            if (r.start_bin <= 7 && r.end_bin >= 7) has7 = true;
        check(has7, "region includes bin 7 (1.00 peak)");
        for (auto& r : det.regions) check(r.peak_bin != 0, "peak_bin != 0");
    }

    std::printf("T4: Sidemen WC vlog (%.0fs, binS=%.2fs)\n",
                sidemen.len_sec, sidemen.bin_ms / 1000.0);
    {
        auto h = to_h(sidemen);
        auto det = ytshorts::detector::detect(h, sidemen.len_sec, cfg());
        bool has59 = false;
        for (auto& r : det.regions)
            if (r.start_bin <= 59 && r.end_bin >= 59) has59 = true;
        check(has59, "region covers bin 59 (peak 1.00)");
        auto p = ytshorts::planner::plan(det, h, sidemen.len_sec, {}, cfg());
        bool top_at_59 = false;
        for (auto& c : p.clips) if (c.peak_bin == 59) top_at_59 = true;
        check(top_at_59, "planner selects bin-59 peak");
    }

    std::printf("T5: Bob Lazar podcast (%.0fs, binS=%.2fs) — coarse bins\n",
                boblazar.len_sec, boblazar.bin_ms / 1000.0);
    {
        auto h = to_h(boblazar);
        auto det = ytshorts::detector::detect(h, boblazar.len_sec, cfg());
        bool has44 = false, has99 = false;
        for (auto& r : det.regions) {
            if (r.start_bin <= 44 && r.end_bin >= 44) has44 = true;
            if (r.end_bin >= 99) has99 = true;
        }
        check(has44, "region covers bin 44 (0.444 peak)");
        check(has99, "end spike kept (bin 99)");
        auto p = ytshorts::planner::plan(det, h, boblazar.len_sec, {}, cfg());
        for (auto& c : p.clips)
            check(c.end_sec - c.start_sec <= 90.01, "clip length capped at 90s");
    }

    std::printf("T6: Synthetic flat-high (music-like)\n");
    {
        Heatmap h; h.bin_seconds = 2.8;
        for (std::size_t i = 0; i < ytshorts::kBinCount; ++i) {
            h.bins[i].start_sec = i * 2.8;
            h.bins[i].end_sec   = (i + 1) * 2.8;
            h.bins[i].value     = 0.65;
        }
        auto det = ytshorts::detector::detect(h, 280.0, cfg());
        check(det.quality.flat, "flat profile flagged (max/median < 2)");
    }

    std::printf("T7a: Very short video (<=180s) covers full duration\n");
    {
        Heatmap h; h.bin_seconds = 1.8;
        for (std::size_t i = 0; i < ytshorts::kBinCount; ++i) {
            h.bins[i].start_sec = i * 1.8;
            h.bins[i].end_sec   = (i + 1) * 1.8;
            h.bins[i].value     = 0.10;
        }
        double peak[] = {0.55, 0.70, 0.85, 0.95, 1.00, 0.90, 0.80, 0.70};
        for (int k = 0; k < 8; ++k) h.bins[71 + k].value = peak[k];
        double dur = 180.0;
        auto det = ytshorts::detector::detect(h, dur, cfg());
        auto p = ytshorts::planner::plan(det, h, dur, {}, cfg());
        check(p.clips.size() == 1, "one clip");
        if (!p.clips.empty())
            check(std::fabs(p.clips[0].end_sec - dur) < 0.001, "clip covers full duration");
    }

    std::printf("T7b: Karate-Kid-like (4:02, single peak)\n");
    {
        Heatmap h; h.bin_seconds = 2.4;
        for (std::size_t i = 0; i < ytshorts::kBinCount; ++i) {
            h.bins[i].start_sec = i * 2.4;
            h.bins[i].end_sec   = (i + 1) * 2.4;
            h.bins[i].value     = 0.10;
        }
        double peak[] = {0.55, 0.70, 0.85, 0.95, 1.00, 0.90, 0.80, 0.70};
        for (int k = 0; k < 8; ++k) h.bins[71 + k].value = peak[k];
        double dur = 240.0;
        auto det = ytshorts::detector::detect(h, dur, cfg());
        auto p = ytshorts::planner::plan(det, h, dur, {}, cfg());
        check(p.clips.size() == 1, "single clip");
        if (!p.clips.empty()) {
            const auto& c = p.clips[0];
            check(c.start_sec <= 180.0 && c.end_sec >= 175.0,
                  "clip window brackets peak region");
        }
    }

    std::printf("T8: min-gap enforcement (3h video, two peaks)\n");
    {
        Heatmap h; h.bin_seconds = 108.0;
        for (std::size_t i = 0; i < ytshorts::kBinCount; ++i) {
            h.bins[i].start_sec = i * 108.0;
            h.bins[i].end_sec   = (i + 1) * 108.0;
            h.bins[i].value     = 0.05;
        }
        h.bins[10].value = 0.9;
        h.bins[80].value = 0.85;
        auto det = ytshorts::detector::detect(h, 10800.0, cfg());
        auto p = ytshorts::planner::plan(det, h, 10800.0, {}, cfg());
        check(p.clips.size() == 2, "two distinct clips");
    }

    std::printf("\n=== %s (%d failure%s) ===\n",
                failures ? "FAILED" : "ALL PASSED",
                failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
