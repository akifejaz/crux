#include "pipeline.h"

#include "core/detector.h"
#include "core/planner.h"
#include "core/caption_parse.h"
#include "core/caption_scorer.h"
#include "fetch/source.h"
#include "fetch/subs.h"
#include "media/cutter.h"
#include "media/downloader.h"
#include "media/silence.h"
#include "out/manifest.h"

#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
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

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Writes captions.json (candidate list + fusion mode) next to manifest.json.
void write_captions_json(const std::string& dir, const std::string& lang,
                         const std::string& mode, double weight,
                         const CaptionScore& cs) {
    json j;
    j["lang"] = lang;
    j["mode"] = mode;               // "fused" | "caption-only" | "off"
    j["weight"] = weight;
    j["candidates"] = json::array();
    for (const auto& c : cs.candidates) {
        j["candidates"].push_back({
            {"start_sec", c.start_sec},
            {"end_sec", c.end_sec},
            {"score", c.score},
            {"hook", c.hook_text},
            {"four_beat", c.four_beat},
            {"cold_open_match", c.cold_open_match},
            {"signals", c.signals},
        });
    }
    std::ofstream out(fs::path(dir) / "captions.json", std::ios::binary);
    out << j.dump(2) << "\n";
}

void print_caption_candidates(const CaptionScore& cs) {
    std::printf("\ncaption crux candidates (tier-1 scorer):\n");
    std::printf("%-3s  %-10s  %-10s  %-7s  %-4s  %-4s  %s\n",
                "#", "start", "end", "score", "4bt", "cold", "hook");
    int i = 0;
    for (const auto& c : cs.candidates) {
        auto hms = [](double t) {
            int T = static_cast<int>(t + 0.5);
            char b[16];
            std::snprintf(b, sizeof(b), "%02d:%02d:%02d",
                          T / 3600, (T % 3600) / 60, T % 60);
            return std::string(b);
        };
        std::string hook = c.hook_text.substr(0, 72);
        std::printf("%-3d  %-10s  %-10s  %-7.1f  %-4s  %-4s  %s\n",
                    ++i, hms(c.start_sec).c_str(), hms(c.end_sec).c_str(),
                    c.score, c.four_beat ? "yes" : "-",
                    c.cold_open_match ? "yes" : "-", hook.c_str());
    }
    std::printf("\n");
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
    const bool captions_enabled = cfg.detect != DetectMode::Heatmap;
    const bool heatmap_wanted   = cfg.detect != DetectMode::Captions;
    int total_steps = (cfg.dry_run ? 2 : 3 + cfg.max_clips) +
                      (captions_enabled ? 1 : 0);
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
    if (cfg.out_dir.empty()) cfg.out_dir = default_out_dir(fr.video.id);
    fs::create_directories(cfg.out_dir);

    // Caption fetch + tier-1 crux scoring (docs/research/README.md).
    CaptionDoc caption_doc;
    CaptionScore caption_score;
    std::string caption_lang, caption_mode = "off";
    if (captions_enabled) {
        say("Fetching captions (%s)", cfg.captions_langs.c_str());
        std::string subs_dir = (fs::path(cfg.out_dir) / "work" / "subs").string();
        try {
            if (auto subs = fetch::fetch_subtitles(
                    fr.video.url.empty() ? cfg.url_or_id : fr.video.url, cfg, subs_dir)) {
                caption_doc = captions::parse_vtt(read_file(subs->vtt_path));
                caption_doc.lang = subs->lang;
                caption_lang = subs->lang;
                caption_score = captions::score_captions(caption_doc,
                                                         fr.video.duration_sec);
            }
        } catch (const std::exception& e) {
            spdlog::debug("caption fetch/score failed: {}", e.what());
        }
        if (caption_score.usable) {
            std::printf("    captions: %s, %zu cues, %zu crux candidate(s)\n",
                        caption_lang.c_str(), caption_doc.cues.size(),
                        caption_score.candidates.size());
        } else {
            std::printf("    captions: none usable — heatmap-only planning\n");
        }
    }

    // Pick the profile detection runs on (--detect):
    //   fused    — heatmap blended with caption score when both exist
    //   heatmap  — replay heatmap only (pre-caption behavior)
    //   captions — caption score only, even when a heatmap exists
    // Captions alone also rescue no-heatmap videos in fused mode.
    Heatmap detect_map;
    if (!heatmap_wanted && !caption_score.usable) {
        std::fprintf(stderr,
            "error: --detect captions requested but no usable captions were "
            "found for this video. Try --captions-langs or --detect fused.\n");
        return 2;
    }
    if (heatmap_wanted && fr.heatmap && caption_score.usable && cfg.caption_weight > 0.0) {
        detect_map = *fr.heatmap;
        const double w = cfg.caption_weight;
        for (std::size_t i = 0; i < kBinCount; ++i)
            detect_map.bins[i].value =
                (1.0 - w) * detect_map.bins[i].value + w * caption_score.bins[i];
        caption_mode = "fused";
    } else if (heatmap_wanted && fr.heatmap) {
        detect_map = *fr.heatmap;
        if (caption_score.usable) caption_mode = "fused";   // weight 0 → plain
    } else if (caption_score.usable) {
        // No heatmap: synthesize a 100-bin profile from the caption score.
        const double bin_s = fr.video.duration_sec / static_cast<double>(kBinCount);
        detect_map.bin_seconds = bin_s;
        for (std::size_t i = 0; i < kBinCount; ++i) {
            detect_map.bins[i].start_sec = static_cast<double>(i) * bin_s;
            detect_map.bins[i].end_sec = static_cast<double>(i + 1) * bin_s;
            detect_map.bins[i].value = caption_score.bins[i];
        }
        caption_mode = "caption-only";
        std::printf("    %s — planning from captions alone\n",
                    heatmap_wanted ? "no replay heatmap" : "--detect captions");
    } else {
        std::fprintf(stderr,
            "error: no heatmap available for this video, and no usable "
            "captions to plan from. YouTube only exposes 'Most replayed' on "
            "eligible videos (about 11%% of high-view videos lack it).\n");
        return 2;
    }

    // Dump-heatmap side output (the profile detection actually runs on).
    if (cfg.dump_heatmap) {
        std::string spark = out::write_heatmap_json_and_sparkline(cfg.out_dir, detect_map);
        std::printf("%s\n", spark.c_str());
    }

    say("Planning up to %d clip(s) — video: %s (%.0fs)",
        cfg.max_clips,
        fr.video.title.empty() ? fr.video.id.c_str() : fr.video.title.c_str(),
        fr.video.duration_sec);

    // Detect + plan on the (possibly fused / caption-synthesized) profile.
    detector::DetectResult det =
        detector::detect(detect_map, fr.video.duration_sec, cfg);
    Plan plan =
        planner::plan(det, detect_map, fr.video.duration_sec, fr.chapters, cfg);

    // Caption post-pass: when a planned clip overlaps a caption candidate,
    // adopt the candidate's boundaries — the candidate starts on the hook
    // line and ends after the payoff, which beats a centroid-centered window
    // for reel material. Then snap everything to cue boundaries and label
    // clips from the candidate's hook line.
    if (caption_score.usable) {
        for (auto& c : plan.clips) {
            const CruxCandidate* best = nullptr;
            for (const auto& cand : caption_score.candidates) {
                if (cand.start_sec < c.end_sec && cand.end_sec > c.start_sec &&
                    (!best || cand.score > best->score))
                    best = &cand;
            }
            if (best && !cfg.clip_len) {   // an explicit -l overrides captions
                c.start_sec = best->start_sec;
                c.end_sec = std::max(best->end_sec, best->start_sec + 30.0);
                c.centroid_sec = (c.start_sec + c.end_sec) / 2.0;
            }
            captions::snap_to_cues(caption_doc, 3.0, c.start_sec, c.end_sec);
            if (c.label.empty() && best) c.label = best->hook_text.substr(0, 80);
        }
        write_captions_json(cfg.out_dir, caption_lang, caption_mode,
                            cfg.caption_weight, caption_score);
    }

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
            print_dry_run_table(plan, fr.video, detect_map);
            if (caption_score.usable) print_caption_candidates(caption_score);
            std::printf("dry-run complete — manifest: %s/manifest.json\n", cfg.out_dir.c_str());
        }
        return 0;
    }

    // Download + cut.
    std::string work_dir = (fs::path(cfg.out_dir) / "work").string();
    media::DownloadResult dl;
    // Ask the downloader what strategy it will use so we can tell the user.
    auto planned = media::pick_strategy(fr.video, plan.clips, cfg);
    switch (planned) {
    case media::Strategy::PerClipParallel: {
        double sum = 0.0;
        for (const auto& c : plan.clips) sum += c.end_sec - c.start_sec;
        say("Downloading %zu clip(s) in parallel via yt-dlp (~%.0fs of %.0fs) "
            "[--try-sections opt-in; may fall back to full download]",
            plan.clips.size(), sum, fr.video.duration_sec);
        break;
    }
    case media::Strategy::LegacySections:
    case media::Strategy::Full:
    default:
        say("Downloading full source video via yt-dlp (%.0fs) "
            "[reliable path; pass --try-sections to opt into per-clip mode]",
            fr.video.duration_sec);
        break;
    }
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
