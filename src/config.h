// Runtime configuration for crux. Populated by CLI parser (see cli.h).
// Defaults in this file mirror PLAN.md §4 / §6.
#pragma once

#include <optional>
#include <string>

namespace crux {

enum class Format {
    Blur916,  // 1080x1920 blurred-bars (default)
    Crop916,  // 1080x1920 center crop
    Orig      // no re-frame, just cut
};

enum class SourceKind {
    Ytdlp,   // yt-dlp subprocess (M1)
    Native   // libcurl direct fetch (M5)
};

struct Config {
    // Input
    std::string url_or_id;

    // Output
    std::string out_dir;              // default ./out/<id>
    bool json_stdout = false;
    bool verbose = false;

    // Planning
    int max_clips = 6;                 // hard-capped at 10
    std::optional<double> clip_len;    // seconds; std::nullopt = auto (§4.9)
    std::optional<double> min_gap;     // seconds; std::nullopt = auto max(90, 2·binS)
    bool keep_intro = false;
    bool strict = false;               // exit 6 on flat heatmap

    // Output format
    Format format = Format::Blur916;

    // Fetch mode
    SourceKind source = SourceKind::Ytdlp;
    std::optional<std::string> cookies_from_browser;  // e.g. "chrome"

    // Media pipeline
    bool full_download = false;
    // Opt-in to parallel per-clip section downloads. Default OFF because
    // yt-dlp's --download-sections is unreliable for the DASH streams
    // YouTube serves for HD H.264: workers stall after emitting "Destination"
    // with no error, no progress. Full download is the reliable path.
    // Users on slow connections downloading tiny clips from very long videos
    // may still want to try this; hard per-worker timeout guards against
    // indefinite hangs.
    bool try_sections = false;

    // Flow controls
    bool dry_run = false;
    bool dump_heatmap = false;

    // External binaries — resolution order: flag → env → bundled → PATH
    std::optional<std::string> ytdlp_path;
    std::optional<std::string> ffmpeg_path;
};

} // namespace crux
