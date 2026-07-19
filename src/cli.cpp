#include "cli.h"

#include <CLI/CLI.hpp>
#include <cstdio>
#include <string>

namespace crux {

const char* version_string() {
    return "crux 0.1.0";
}

CliResult parse_cli(int argc, char** argv) {
    CliResult r{};
    Config& c = r.config;

    CLI::App app{"crux — cut the most-replayed moments out of a YouTube video", "crux"};
    app.set_version_flag("--version", version_string());
    app.set_help_flag("-h,--help", "Show help");

    app.add_option("url", c.url_or_id, "YouTube URL or 11-char video id")
        ->required();

    app.add_option("-o,--out", c.out_dir, "Output directory (default ./out/<id>)");
    app.add_option("-n,--max-clips", c.max_clips, "Max clips (default 6, cap 10)")
        ->check(CLI::Range(1, 10));

    double clip_len_val = 0.0;
    auto* clip_len_opt = app.add_option("-l,--clip-len", clip_len_val,
        "Clip length seconds (default auto; cap 180)")
        ->check(CLI::Range(15.0, 180.0));

    double min_gap_val = 0.0;
    auto* min_gap_opt = app.add_option("--min-gap", min_gap_val,
        "Minimum gap between clip centroids in seconds (default auto)");

    std::string fmt_str = "916blur";
    app.add_option("--format", fmt_str, "Output format: 916blur | 916crop | orig")
        ->check(CLI::IsMember({"916blur", "916crop", "orig"}));

    std::string source_str = "ytdlp";
    app.add_option("--source", source_str,
        "Heatmap source backend: ytdlp | native (M5)")
        ->check(CLI::IsMember({"ytdlp", "native"}));

    std::string cookies;
    auto* cookies_opt = app.add_option("--cookies-from-browser", cookies,
        "Passthrough to yt-dlp (e.g. chrome, firefox)");

    std::string ytdlp_path, ffmpeg_path;
    auto* ytdlp_opt  = app.add_option("--ytdlp",  ytdlp_path,  "Path to yt-dlp binary");
    auto* ffmpeg_opt = app.add_option("--ffmpeg", ffmpeg_path, "Path to ffmpeg binary");

    std::string detect_str = "fused";
    app.add_option("--detect", detect_str,
        "Detection signal: fused (heatmap + captions, default) | heatmap | captions")
        ->check(CLI::IsMember({"fused", "heatmap", "captions"}));
    app.add_flag("--no-captions",   c.no_captions,   "Alias for --detect heatmap");
    app.add_option("--captions-langs", c.captions_langs,
        "Subtitle languages to try, yt-dlp syntax (default hi,ur,en,en-orig)");
    app.add_option("--caption-weight", c.caption_weight,
        "Caption share in heatmap fusion 0..1 (default 0.4)")
        ->check(CLI::Range(0.0, 1.0));

    app.add_flag("--dry-run",       c.dry_run,       "Plan only (no download)");
    app.add_flag("--dump-heatmap",  c.dump_heatmap,  "Write heatmap.json + ASCII sparkline");
    app.add_flag("--json",          c.json_stdout,   "Machine-readable stdout");
    app.add_flag("--keep-intro",    c.keep_intro,    "Keep intro bins in candidacy");
    app.add_flag("--strict",        c.strict,        "Exit 6 on flat heatmap");
    app.add_flag("--full-download", c.full_download, "Force full video download (default when not opting into --try-sections)");
    app.add_flag("--try-sections",  c.try_sections,  "Opt-in: parallel per-clip section downloads (unreliable for YouTube DASH; hard 5-min timeout per worker)");
    app.add_flag("-v,--verbose",    c.verbose,       "Verbose logging");

    try {
        app.parse(argc, argv);
    } catch (const CLI::CallForHelp&) {
        std::fputs(app.help().c_str(), stdout);
        return r;                       // should_run stays false
    } catch (const CLI::CallForVersion&) {
        std::printf("%s\n", version_string());
        return r;
    } catch (const CLI::ParseError& e) {
        r.exit_code = app.exit(e);
        return r;
    }

    if (*clip_len_opt)  c.clip_len = clip_len_val;
    if (*min_gap_opt)   c.min_gap  = min_gap_val;
    if (*cookies_opt)   c.cookies_from_browser = cookies;
    if (*ytdlp_opt)     c.ytdlp_path  = ytdlp_path;
    if (*ffmpeg_opt)    c.ffmpeg_path = ffmpeg_path;

    if      (fmt_str == "916blur") c.format = Format::Blur916;
    else if (fmt_str == "916crop") c.format = Format::Crop916;
    else if (fmt_str == "orig")    c.format = Format::Orig;

    c.source = (source_str == "native") ? SourceKind::Native : SourceKind::Ytdlp;

    if      (detect_str == "heatmap")  c.detect = DetectMode::Heatmap;
    else if (detect_str == "captions") c.detect = DetectMode::Captions;
    else                               c.detect = DetectMode::Fused;
    if (c.no_captions) c.detect = DetectMode::Heatmap;

    r.should_run = true;
    return r;
}

} // namespace crux
