#include "pipeline.h"

#include "core/detector.h"
#include "core/planner.h"
#include "core/caption_parse.h"
#include "core/caption_scorer.h"
#include "fetch/source.h"
#include "fetch/subs.h"
#include "media/cutter.h"
#include "media/downloader.h"
#include "media/outro_card.h"
#include "media/silence.h"
#include "media/subtitle_render.h"
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

// Truncates a UTF-8 string to at most `max_bytes`, walking back to a valid
// codepoint boundary. Prevents partial multi-byte sequences from breaking
// JSON serialization (nlohmann::json throws on invalid UTF-8).
std::string utf8_truncate(std::string s, std::size_t max_bytes) {
    if (s.size() <= max_bytes) return s;
    s.resize(max_bytes);
    while (!s.empty()) {
        unsigned char c = static_cast<unsigned char>(s.back());
        if (c < 0x80) break;               // ASCII — safe to end here
        if ((c & 0xC0) == 0xC0) { s.pop_back(); break; }  // leading byte
        s.pop_back();                       // continuation byte, keep walking
    }
    return s;
}

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
        // Prefer captions in the video's declared native language when we
        // know it — otherwise a language-agnostic fetch that puts "hi" first
        // will pick Hindi auto-translations even on an English video. The
        // user-facing captions_langs list is honored as fallback. This is a
        // display-quality fix: native auto captions transcribe original
        // audio directly, while translated tracks compound ASR + MT errors.
        Config subs_cfg = cfg;
        if (!fr.video.language.empty() &&
            cfg.captions_langs.find(fr.video.language) != 0) {
            subs_cfg.captions_langs = fr.video.language + "," + cfg.captions_langs;
        }
        say("Fetching captions (%s)", subs_cfg.captions_langs.c_str());
        std::string subs_dir = (fs::path(cfg.out_dir) / "work" / "subs").string();
        try {
            if (auto subs = fetch::fetch_subtitles(
                    fr.video.url.empty() ? cfg.url_or_id : fr.video.url,
                    subs_cfg, subs_dir)) {
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
        // Compound failure: video has no heatmap AND we couldn't plan from
        // captions. Distinguish the transient case (YouTube rate-limited
        // the subtitle fetch) from the permanent one (video really has no
        // captions) so the user knows whether "just retry" is the right
        // next step.
        if (captions_enabled && fetch::subtitle_fetch_was_rate_limited()) {
            std::fprintf(stderr,
                "error: no heatmap for this video, and YouTube rate-limited "
                "the caption fetch (HTTP 429). This is transient — wait a "
                "minute and retry, or add --cookies-from-browser chrome to "
                "use your logged-in session (higher quota).\n");
        } else {
            std::fprintf(stderr,
                "error: no heatmap available for this video, and no usable "
                "captions to plan from. YouTube only exposes 'Most replayed' "
                "on eligible videos (about 11%% of high-view videos lack "
                "it).\n");
        }
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
            if (c.label.empty() && best) c.label = utf8_truncate(best->hook_text, 80);
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

    // Thumbnail for the clip intro card ("which video is this from").
    // Best-effort: a failed fetch just means plain clips.
    std::string thumb_path;
    if (cfg.intro_card) {
        if (auto t = fetch::fetch_thumbnail(
                fr.video.url.empty() ? cfg.url_or_id : fr.video.url, cfg,
                (fs::path(work_dir) / "assets").string()))
            thumb_path = *t;
        else
            spdlog::warn("thumbnail unavailable — cutting clips without intro card");
    }

    // Outro "Watch full video here" card, one JPG shared by every clip.
    std::string outro_path;
    if (cfg.outro_card && !fr.video.channel_url.empty()) {
        std::string assets_dir = (fs::path(work_dir) / "assets" / "channel").string();
        if (auto assets = fetch::fetch_channel_assets(fr.video.channel_url, cfg,
                                                     assets_dir)) {
            std::string card = (fs::path(work_dir) / "assets" / "outro.jpg").string();
            if (auto p = media::render_outro_card(fr.video, *assets, cfg, card))
                outro_path = *p;
        }
        if (outro_path.empty())
            spdlog::warn("outro card unavailable — cutting clips without it");
    }

    // Per-clip subtitle files are cached alongside the source in work/subs.
    // Reused on retries; ignored (empty path) when no captions were fetched.
    const bool subs_available =
        cfg.subtitles && cfg.format != Format::Orig && !caption_doc.cues.empty();
    const fs::path subs_out_dir = fs::path(cfg.out_dir) / "work" / "subs";
    if (subs_available) fs::create_directories(subs_out_dir);

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

        std::string subtitle_file;
        if (subs_available) {
            char subname[64];
            std::snprintf(subname, sizeof(subname), "clip_%02d.ass", c.index);
            fs::path ass = subs_out_dir / subname;
            media::SubtitleStyle style;
            style.font_size = cfg.subtitle_size;
            // Subtitle timing is clip-relative — the intro xfade already
            // hides them until the reframed video becomes visible, so no
            // extra offset is needed here.
            if (media::write_clip_ass(caption_doc,
                                      c.start_sec, c.end_sec, 0.0,
                                      style, ass.string(),
                                      cfg.subtitle_romanize, caption_lang))
                subtitle_file = ass.string();
        }

        try {
            media::cut_clip(dl.source_file, c, dl, cfg.format, cfg, out_file,
                            thumb_path, outro_path, subtitle_file);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "error: ffmpeg cut failed for clip %d: %s\n",
                         c.index, e.what());
            return 5;
        }
    }

    // Rewrite manifest so file paths are final (they were already correct, but
    // this keeps the door open for M4 silence-snap post-processing).
    out::write_manifest(cfg.out_dir, fr.video, cfg, true, plan.quality, plan.clips);

    // Every clip cut fine — drop the big downloaded intermediates so out/
    // only holds the shorts (README in out/ documents the layout). Subtitles
    // and the thumbnail (a few hundred KB) stay for reference.
    if (!cfg.keep_source) {
        std::uintmax_t freed = 0;
        auto rm = [&freed](const fs::path& f) {
            std::error_code ec;
            auto sz = fs::file_size(f, ec);
            if (ec) return;
            if (fs::remove(f, ec) && !ec) freed += sz;
        };
        rm(dl.source_file);
        for (const auto& f : dl.per_clip_files)
            if (!f.empty()) rm(f);
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(work_dir, ec)) {
            if (!e.is_regular_file()) continue;
            auto ext = e.path().extension().string();
            if (ext == ".mp4" || ext == ".webm" || ext == ".m4a" || ext == ".part")
                rm(e.path());
        }
        if (freed > 0)
            std::printf("    freed %.1f MB of source video (--keep-source to keep it)\n",
                        static_cast<double>(freed) / (1024.0 * 1024.0));
    }

    if (cfg.json_stdout) {
        std::printf("{\"clips\":%zu,\"out\":\"%s\"}\n",
                    plan.clips.size(), cfg.out_dir.c_str());
    } else {
        std::printf("wrote %zu clip(s) to %s\n", plan.clips.size(), cfg.out_dir.c_str());
    }
    return 0;
}

} // namespace crux
