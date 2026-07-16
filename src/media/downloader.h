#pragma once

#include "config.h"
#include "core/heatmap.h"

#include <string>
#include <vector>

namespace ytshorts::media {

struct DownloadResult {
    // Absolute path to a single input file the cutter can seek into.
    // If sections were used, the file may contain gaps for regions we didn't
    // request; the cutter must map (clip.start, clip.end) into file time.
    std::string source_file;

    // When sections are used, we store (video_start_sec → file_start_sec) so
    // the cutter can translate. Empty means source_file mirrors the input 1:1.
    struct Section {
        double src_start_sec;
        double src_end_sec;
        double file_start_sec;
    };
    std::vector<Section> sections;
};

// Download only what the planner needs. When Σ(clip lengths) < 25% of duration
// and --full-download is not set, use yt-dlp --download-sections; else download
// the whole video.
DownloadResult download(const std::string& url,
                        const VideoMeta& meta,
                        const std::vector<Clip>& clips,
                        const Config& cfg,
                        const std::string& work_dir);

} // namespace ytshorts::media
