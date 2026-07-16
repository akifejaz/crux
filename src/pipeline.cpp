#include "pipeline.h"

#include "core/detector.h"
#include "core/planner.h"
#include "fetch/source.h"
#include "media/cutter.h"
#include "media/downloader.h"
#include "media/silence.h"
#include "out/manifest.h"

#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

#include <cstdio>
#include <filesystem>
#include <sstream>
#include <stdexcept>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace crux {

namespace {

std::string default_out_dir(const std::string& id) {
    return (fs::path("out") / id).string();
}

std::string clip_filename(const Clip& c, Format /*fmt*/) {
    // clip_NN_HHMMSS.mp4
    int total = static_cast<int>(c.start_sec + 0.5);
    int h = total / 3600, m = (total % 3600) / 60, s = total % 60;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "clip_%02d_%02d%02d%02d.mp4", c.index, h, m, s);
    return buf;
}

void print_dry_run_table(const Plan& p, const VideoMeta& v, const Heatmap& h) {
    std::printf("\n[%s] %s  duration=%.0fs  binS=%.1fs  maxOverMedian=%.2f%s\n",
                v.id.c_str(), v.title.c_str(), v.duration_sec, h.bin_seconds,
                p.quality.max_over_median, p.quality.flat ? " (FLAT)" : "");
    std::printf("%-3s  %-10s  %-10s  %-6s  %-6s  %-5s  %s\n",
                "#", "start", "end", "dur", "peak", "bin", "label");
    for (const auto& c : p.clips) {
        auto hms = [](double t) {
            int total = static_cast<int>(t + 0.5);
            char b[16];
            std::snprintf(b, sizeof(b), "%02d:%02d:%02d",
                          total / 3600, (total % 3600) / 60, total % 60);
            return std::string(b);
        };
        std::printf("%-3d  %-10s  %-10s  %-6.1f  %-6.3f  %-5d  %s\n",
                    c.index, hms(c.start_sec).c_str(), hms(c.end_sec).c_str(),
                    c.end_sec - c.start_sec, c.peak_score, c.peak_bin,
                    c.label.c_str());
    }
    std::printf("\n");
}

} // namespace

int run_pipeline(const Config& cfg_in) {
    Config cfg = cfg_in;

    if (cfg.verbose) spdlog::set_level(spdlog::level::debug);

    // Total steps in the pipeline; refined after planning when the real clip
    // count is known.
    //   1) fetch metadata + heatmap
    //   2) detect + plan
    //   3) download                 (skipped for --dry-run)
    //   4..3+N) cut each clip (N clips)
    int total_steps = cfg.dry_run ? 2 : 3 + cfg.max_clips;
    int step = 0;
    auto say = [&](const char* fmt, auto... args) {
        ++step;
        std::printf("[%d/%d] ", step, total_steps);
        std::printf(fmt, args...);
        std::printf("\n");
        std::fflush(stdout);
    };

    say("Fetching heatmap for %s%s", cfg.url_or_id.c_str(),
        cfg.source == SourceKind::Native ? " (native)" : " (yt-dlp)");

    // Fetch metadata + heatmap.
    std::unique_ptr<fetch::IHeatmapSource> src =
        (cfg.source == SourceKind::Native) ? fetch::make_native_source()
                                           : fetch::make_ytdlp_source();
    FetchResult fr;
    try {
        fr = src->fetch(cfg.url_or_id, cfg);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: metadata fetch failed: %s\n", e.what());
        return 3;
    }

    if (fr.video.is_live) {
        std::fprintf(stderr,
            "error: live/premiere/upcoming video — heatmap is not applicable.\n");
        return 3;
    }
    if (!fr.heatmap) {
        std::fprintf(stderr,
            "error: no heatmap available for this video. "
            "YouTube only exposes 'Most replayed' on eligible videos, and about "
            "11%% of high-view videos lack it.\n");
        return 2;
    }

    if (cfg.out_dir.empty()) cfg.out_dir = default_out_dir(fr.video.id);
    fs::create_directories(cfg.out_dir);

    // Dump-heatmap side output.
    if (cfg.dump_heatmap) {
        std::string spark = out::write_heatmap_json_and_sparkline(cfg.out_dir, *fr.heatmap);
        std::printf("%s\n", spark.c_str());
    }

    say("Planning up to %d clip(s) — video: %s (%.0fs)",
        cfg.max_clips,
        fr.video.title.empty() ? fr.video.id.c_str() : fr.video.title.c_str(),
        fr.video.duration_sec);

    // Detect + plan.
    detector::DetectResult det =
        detector::detect(*fr.heatmap, fr.video.duration_sec, cfg);
    Plan plan =
        planner::plan(det, *fr.heatmap, fr.video.duration_sec, fr.chapters, cfg);

    if (plan.quality.flat) {
        std::fprintf(stderr,
            "warning: flat heatmap profile (max/median=%.2f) — no distinct moments.\n",
            plan.quality.max_over_median);
        if (cfg.strict) return 6;
    }

    // Attach filenames and write manifest early (so --dry-run has it).
    for (auto& c : plan.clips) c.file = clip_filename(c, cfg.format);
    out::write_manifest(cfg.out_dir, fr.video, cfg, true, plan.quality, plan.clips);

    // Refine total_steps now that we know the real number of clips.
    if (!cfg.dry_run) total_steps = 3 + static_cast<int>(plan.clips.size());

    if (cfg.dry_run) {
        if (cfg.json_stdout) {
            // The manifest already has everything; echo the count.
            std::printf("{\"clips\":%zu,\"dryRun\":true}\n", plan.clips.size());
        } else {
            print_dry_run_table(plan, fr.video, *fr.heatmap);
            std::printf("dry-run complete — manifest: %s/manifest.json\n", cfg.out_dir.c_str());
        }
        return 0;
    }

    // Download + cut.
    std::string work_dir = (fs::path(cfg.out_dir) / "work").string();
    media::DownloadResult dl;
    say("Downloading source video via yt-dlp (%.0fs total)",
        fr.video.duration_sec);
    try {
        dl = media::download(fr.video.url.empty() ? cfg.url_or_id : fr.video.url,
                             fr.video, plan.clips, cfg, work_dir);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: download failed: %s\n", e.what());
        return 4;
    }

    int clip_i = 0;
    for (auto& c : plan.clips) {
        ++clip_i;
        auto hms = [](double t) {
            int T = static_cast<int>(t + 0.5);
            char b[16];
            std::snprintf(b, sizeof(b), "%02d:%02d:%02d",
                          T / 3600, (T % 3600) / 60, T % 60);
            return std::string(b);
        };
        say("Cutting clip %d/%zu  %s..%s  %.0fs  → %s",
            clip_i, plan.clips.size(),
            hms(c.start_sec).c_str(), hms(c.end_sec).c_str(),
            c.end_sec - c.start_sec, c.file.c_str());
        std::string out_file = (fs::path(cfg.out_dir) / c.file).string();
        try {
            media::cut_clip(dl.source_file, c, dl, cfg.format, cfg, out_file);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "error: ffmpeg cut failed for clip %d: %s\n",
                         c.index, e.what());
            return 5;
        }
    }

    // Rewrite manifest so file paths are final (they were already correct, but
    // this keeps the door open for M4 silence-snap post-processing).
    out::write_manifest(cfg.out_dir, fr.video, cfg, true, plan.quality, plan.clips);

    if (cfg.json_stdout) {
        std::printf("{\"clips\":%zu,\"out\":\"%s\"}\n",
                    plan.clips.size(), cfg.out_dir.c_str());
    } else {
        std::printf("wrote %zu clip(s) to %s\n", plan.clips.size(), cfg.out_dir.c_str());
    }
    return 0;
}

} // namespace crux
