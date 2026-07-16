// Downloader strategy picker — the heuristic that decides between
// Full and PerClipParallel. Pure function, no network / no yt-dlp.

#include <doctest/doctest.h>

#include "media/downloader.h"

namespace {

crux::VideoMeta make_meta(double duration_sec) {
    crux::VideoMeta m;
    m.id = "x";
    m.duration_sec = duration_sec;
    return m;
}

crux::Clip make_clip(int idx, double start, double end) {
    crux::Clip c;
    c.index = idx;
    c.start_sec = start;
    c.end_sec = end;
    return c;
}

std::vector<crux::Clip> even_clips(int n, double each_sec, double stride_sec) {
    std::vector<crux::Clip> out;
    for (int i = 0; i < n; ++i) {
        out.push_back(make_clip(i + 1, i * stride_sec, i * stride_sec + each_sec));
    }
    return out;
}

} // namespace

TEST_CASE("strategy: --full-download always wins") {
    crux::Config cfg; cfg.full_download = true;
    auto clips = even_clips(6, 60.0, 600.0);
    auto meta = make_meta(3600.0);
    CHECK(crux::media::pick_strategy(meta, clips, cfg) == crux::media::Strategy::Full);
}

TEST_CASE("strategy: single clip → Full (per-clip has no benefit)") {
    crux::Config cfg;
    auto clips = even_clips(1, 60.0, 0.0);
    auto meta = make_meta(3600.0);   // 1h video
    CHECK(crux::media::pick_strategy(meta, clips, cfg) == crux::media::Strategy::Full);
}

TEST_CASE("strategy: short video (<= 15 min) → Full regardless") {
    crux::Config cfg;
    auto clips = even_clips(6, 40.0, 100.0);
    auto meta = make_meta(15.0 * 60.0);   // exactly 15 min
    CHECK(crux::media::pick_strategy(meta, clips, cfg) == crux::media::Strategy::Full);
    meta = make_meta(5.0 * 60.0);          // 5 min
    CHECK(crux::media::pick_strategy(meta, clips, cfg) == crux::media::Strategy::Full);
}

TEST_CASE("strategy: without --try-sections, always Full (reliable default)") {
    crux::Config cfg;   // try_sections default false
    auto clips = even_clips(6, 60.0, 500.0);
    auto meta = make_meta(6000.0);
    CHECK(crux::media::pick_strategy(meta, clips, cfg) == crux::media::Strategy::Full);
}

TEST_CASE("strategy: with --try-sections, long+multi+small → PerClipParallel") {
    crux::Config cfg; cfg.try_sections = true;
    auto clips = even_clips(6, 60.0, 500.0);   // 360s total
    auto meta = make_meta(6000.0);              // 100 min video (6% clips)
    CHECK(crux::media::pick_strategy(meta, clips, cfg) ==
          crux::media::Strategy::PerClipParallel);
}

TEST_CASE("strategy: --try-sections + user's 1h45m/6-clip case → PerClipParallel") {
    crux::Config cfg; cfg.try_sections = true;
    std::vector<crux::Clip> clips = {
        make_clip(1, 196.1,  306.1),
        make_clip(2, 1932.7, 2042.7),
        make_clip(3, 2563.7, 2673.7),
        make_clip(4, 3068.4, 3178.4),
        make_clip(5, 4259.0, 4369.0),
        make_clip(6, 5230.9, 5340.9),
    };
    auto meta = make_meta(6309.0);
    CHECK(crux::media::pick_strategy(meta, clips, cfg) ==
          crux::media::Strategy::PerClipParallel);
}

TEST_CASE("strategy: --try-sections but big clip footprint (>30%) → Full") {
    crux::Config cfg; cfg.try_sections = true;
    // 30 min video, 4 clips × 200s = 800s (~44% of video) — no bandwidth
    // savings from sections, just download the whole thing.
    auto clips = even_clips(4, 200.0, 400.0);
    auto meta = make_meta(30.0 * 60.0);
    CHECK(crux::media::pick_strategy(meta, clips, cfg) == crux::media::Strategy::Full);
}

TEST_CASE("strategy: zero-duration meta (defensive) → Full") {
    crux::Config cfg; cfg.try_sections = true;
    auto clips = even_clips(4, 60.0, 300.0);
    auto meta = make_meta(0.0);
    CHECK(crux::media::pick_strategy(meta, clips, cfg) == crux::media::Strategy::Full);
}
