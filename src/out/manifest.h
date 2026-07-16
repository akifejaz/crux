#pragma once

#include "config.h"
#include "core/heatmap.h"

#include <string>

namespace crux::out {

// Writes manifest.json (see PLAN §3.3) into `dir`.
void write_manifest(const std::string& dir,
                    const VideoMeta& video,
                    const Config& cfg,
                    bool heatmap_present,
                    const Quality& quality,
                    const std::vector<Clip>& clips);

// Writes heatmap.json (raw values + bin_seconds) and returns an ASCII sparkline.
std::string write_heatmap_json_and_sparkline(const std::string& dir, const Heatmap& h);

} // namespace crux::out
