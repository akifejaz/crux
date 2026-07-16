#pragma once

#include "config.h"
#include "core/heatmap.h"
#include "media/downloader.h"

#include <string>

namespace crux::media {

// Cuts a single clip out of the downloaded source and writes it to `out_file`.
// - 916blur: 1080x1920 blurred-background + fitted foreground (PLAN §5).
// - 916crop: 1080x1920 center crop.
// - orig:    stream-window re-encode with no reframing.
// Uses -ss/-to BEFORE -i for fast seek, forces re-encode for frame accuracy.
void cut_clip(const std::string& source_file,
              const Clip& clip,
              const DownloadResult& dl,
              Format fmt,
              const Config& cfg,
              const std::string& out_file);

} // namespace crux::media
