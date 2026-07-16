#include "core/planner.h"

#include <algorithm>
#include <cmath>

namespace ytshorts::planner {

namespace {

double clamp(double v, double lo, double hi) {
    return std::max(lo, std::min(hi, v));
}

// Auto clip length (PLAN §4 step 9): clamp(1.5 · binS, 40, 90).
double auto_clip_len(double bin_s) {
    return clamp(1.5 * bin_s, 40.0, 90.0);
}

// Auto min-gap (PLAN §4 step 8): max(90, 2·binS).
double auto_min_gap(double bin_s) {
    return std::max(90.0, 2.0 * bin_s);
}

const std::string* chapter_at(const std::vector<Chapter>& chapters, double t) {
    for (const auto& ch : chapters) {
        if (t >= ch.start_sec && t < ch.end_sec) return &ch.title;
    }
    return nullptr;
}

} // namespace

Plan plan(const detector::DetectResult& d,
          const Heatmap& h,
          double duration_sec,
          const std::vector<Chapter>& chapters,
          const Config& cfg) {
    Plan p;
    p.quality = d.quality;

    // Effective params
    const double bin_s = h.bin_seconds;
    const double clip_len = cfg.clip_len ? *cfg.clip_len : auto_clip_len(bin_s);
    const double min_gap  = cfg.min_gap  ? *cfg.min_gap  : auto_min_gap(bin_s);

    // Special-case: very short video where a "region" essentially covers the
    // whole thing. If duration ≤ 180 s and there's ≥1 region, emit one clip
    // spanning the video (PLAN §9 edge case row 3).
    if (duration_sec <= 180.0 && !d.regions.empty()) {
        Clip c;
        c.index = 1;
        c.start_sec = 0.0;
        c.end_sec = duration_sec;
        // best region
        const detector::Region* best = &d.regions.front();
        for (const auto& r : d.regions) if (r.peak_score > best->peak_score) best = &r;
        c.peak_bin = best->peak_bin;
        c.peak_score = best->peak_score;
        c.centroid_sec = best->centroid_sec;
        if (const auto* label = chapter_at(chapters, c.centroid_sec)) c.label = *label;
        p.clips.push_back(c);
        return p;
    }

    // Step 8 — SELECT: sort by peak desc, greedily keep by min_gap.
    std::vector<detector::Region> regs = d.regions;
    std::sort(regs.begin(), regs.end(), [](const auto& a, const auto& b) {
        return a.peak_score > b.peak_score;
    });

    std::vector<detector::Region> kept;
    const int max_clips = std::max(1, std::min(cfg.max_clips, 10));
    for (const auto& r : regs) {
        if (static_cast<int>(kept.size()) >= max_clips) break;
        bool ok = true;
        for (const auto& k : kept) {
            if (std::fabs(r.centroid_sec - k.centroid_sec) < min_gap) { ok = false; break; }
        }
        if (ok) kept.push_back(r);
    }

    // Step 9 — WINDOW: center on centroid, clamp by SHIFT.
    struct Windowed {
        double start, end;
        const detector::Region* r;
    };
    std::vector<Windowed> windows;
    windows.reserve(kept.size());
    for (const auto& r : kept) {
        double s = r.centroid_sec - clip_len / 2.0;
        double e = r.centroid_sec + clip_len / 2.0;
        if (s < 0.0)              { e += (0.0 - s); s = 0.0; }
        if (e > duration_sec)     { s -= (e - duration_sec); e = duration_sec; }
        if (s < 0.0) s = 0.0;
        windows.push_back({s, e, &r});
    }

    // Step 11 — EMIT ordered by start time.
    std::sort(windows.begin(), windows.end(),
              [](const auto& a, const auto& b) { return a.start < b.start; });

    int idx = 1;
    for (const auto& w : windows) {
        Clip c;
        c.index = idx++;
        c.start_sec = w.start;
        c.end_sec = w.end;
        c.peak_score = w.r->peak_score;
        c.peak_bin = w.r->peak_bin;
        c.centroid_sec = w.r->centroid_sec;
        if (const auto* label = chapter_at(chapters, w.r->centroid_sec)) c.label = *label;
        p.clips.push_back(c);
    }
    return p;
}

} // namespace ytshorts::planner
