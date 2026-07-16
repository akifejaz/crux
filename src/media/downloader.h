#pragma once

#include "config.h"
#include "core/heatmap.h"

#include <string>
#include <vector>

namespace crux::media {

// Strategy the downloader ended up using — surfaced so the pipeline / tests
// / logs can distinguish. Not user-selectable directly; picked by
// pick_strategy() based on cfg + video duration + clip totals.
enum class Strategy {
    Full,         // one yt-dlp call, whole video
    PerClipParallel,   // N parallel yt-dlps, one per clip
    LegacySections     // one yt-dlp call with N --download-sections (deprecated)
};

struct DownloadResult {
    // Present when the whole video was fetched into a single mp4.
    // Empty when per-clip mode was used.
    std::string source_file;

    // When per-clip mode was used, this vector is indexed by clip.index-1
    // and holds the mp4 path for that clip's downloaded section.
    // When full mode was used, this stays empty.
    std::vector<std::string> per_clip_files;

    // Metadata for time-translation when the source file's timeline doesn't
    // match the original video's. In per-clip mode, each entry corresponds
    // to per_clip_files[i] and file_start_sec is 0 (each file is its own
    // 0-based timeline). In legacy single-file-multi-section mode, entries
    // describe where each range lives in the concatenated output.
    struct Section {
        double src_start_sec;
        double src_end_sec;
        double file_start_sec;
    };
    std::vector<Section> sections;

    Strategy strategy = Strategy::Full;
};

// Downloads what the planner needs. Strategy is picked automatically:
//   * `--full-download`  or  single-clip cases          → Full
//   * long video, multi-clip, small total footprint     → PerClipParallel
//   * otherwise                                         → Full
// On any parallel-mode failure, falls back to Full so the pipeline is
// robust in the face of temporary bot-check / rate-limit issues.
DownloadResult download(const std::string& url,
                        const VideoMeta& meta,
                        const std::vector<Clip>& clips,
                        const Config& cfg,
                        const std::string& work_dir);

// Pure strategy picker — testable without spawning yt-dlp.
// Exposed so unit tests can lock down the heuristic without network I/O.
Strategy pick_strategy(const VideoMeta& meta,
                       const std::vector<Clip>& clips,
                       const Config& cfg);

} // namespace crux::media
