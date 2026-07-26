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

// Which signal(s) drive clip detection.
enum class DetectMode {
    Fused,    // replay heatmap + caption crux scorer, blended (default)
    Heatmap,  // heatmap only — pre-caption behavior
    Captions  // caption crux scorer only, even when a heatmap exists
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

    // Captions (crux caption scorer — docs/research/README.md)
    // Fused by default; degrades gracefully when a video has no captions.
    DetectMode detect = DetectMode::Fused;
    bool no_captions = false;          // legacy alias for --detect heatmap
    std::string captions_langs = "hi,ur,en,en-orig";
    // Heatmap/caption fusion: crux = (1-w)*heatmap + w*captions. Research
    // default 0.4 — heatmap stays dominant when present; captions alone
    // (w=1) drive planning for the ~11% of videos with no heatmap.
    double caption_weight = 0.4;

    // Intro card: overlay the source video's thumbnail on the first
    // intro_sec seconds of every clip (with a synthesized whoosh) so viewers
    // see which video the short came from. The clip audio starts at t=0 —
    // the hook is never delayed.
    bool intro_card = true;
    double intro_sec = 1.5;

    // Outro card: append a "Watch full video here" card built from the
    // channel's banner + avatar for outro_sec seconds. Crossfades in from
    // the clip so the reel doesn't hard-cut. 916blur/916crop only.
    bool outro_card = true;
    double outro_sec = 2.0;

    // Burned-in subtitles: overlay the fetched caption cues on the inner
    // 16:9 video (bottom edge, white with outline). 916blur/916crop only —
    // Orig keeps the untouched stream. Requires captions to be fetched
    // (i.e. --detect fused|captions).
    bool subtitles = true;
    int subtitle_size = 44;
    // Auto-transliterate Devanagari/Nastaliq caption text to Latin
    // (Hinglish/Roman-Urdu) so shorts stay readable on phones. English
    // captions pass through unchanged. --original-script disables it.
    bool subtitle_romanize = true;

    // Space: after a fully successful run the downloaded source video (and
    // per-clip intermediates) are deleted from work/ — only the generated
    // clips are kept. --keep-source opts out (e.g. to iterate on clip params
    // without re-downloading).
    bool keep_source = false;

    // Flow controls
    bool dry_run = false;
    bool dump_heatmap = false;

    // External binaries — resolution order: flag → env → bundled → PATH
    std::optional<std::string> ytdlp_path;
    std::optional<std::string> ffmpeg_path;
};

} // namespace crux
