#pragma once

#include "config.h"

#include <optional>
#include <string>

namespace crux::fetch {

struct SubsResult {
    std::string vtt_path;   // downloaded .vtt file
    std::string lang;       // language code actually fetched (e.g. "hi")
};

// Downloads the best-available subtitle track (manual or auto) as VTT into
// `dir` via yt-dlp. Preference order follows the research: original-language
// auto captions first (hi, ur), then native/translated English.
// Returns std::nullopt when the video has no captions — callers degrade
// gracefully to heatmap-only planning.
std::optional<SubsResult> fetch_subtitles(const std::string& url_or_id,
                                          const Config& cfg,
                                          const std::string& dir);

} // namespace crux::fetch
