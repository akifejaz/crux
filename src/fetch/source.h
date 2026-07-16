// Common interface for a heatmap source (M1: yt-dlp; M5: native).
#pragma once

#include "config.h"
#include "core/heatmap.h"

#include <memory>

namespace ytshorts::fetch {

class IHeatmapSource {
public:
    virtual ~IHeatmapSource() = default;
    // Fetch full metadata + heatmap. May return with heatmap == nullopt.
    // Throws std::runtime_error on hard failures (network, auth, ...).
    virtual FetchResult fetch(const std::string& url_or_id, const Config& cfg) = 0;
};

std::unique_ptr<IHeatmapSource> make_ytdlp_source();
std::unique_ptr<IHeatmapSource> make_native_source();   // M5

} // namespace ytshorts::fetch
