#include "cli.h"

#include <CLI/CLI.hpp>
#include <cstdio>
#include <string>

namespace ytshorts {

const char* version_string() {
    return "ytshorts 0.1.0";
}

CliResult parse_cli(int argc, char** argv) {
    CliResult r{};
    Config& c = r.config;

    CLI::App app{"ytshorts — YouTube Most-Replayed → Shorts extractor", "ytshorts"};
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

    app.add_flag("--dry-run",       c.dry_run,       "Plan only (no download)");
    app.add_flag("--dump-heatmap",  c.dump_heatmap,  "Write heatmap.json + ASCII sparkline");
    app.add_flag("--json",          c.json_stdout,   "Machine-readable stdout");
    app.add_flag("--keep-intro",    c.keep_intro,    "Keep intro bins in candidacy");
    app.add_flag("--strict",        c.strict,        "Exit 6 on flat heatmap");
    app.add_flag("--full-download", c.full_download, "Download the entire video (skip sections)");
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

    r.should_run = true;
    return r;
}

} // namespace ytshorts
