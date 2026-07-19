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
//
// When `thumb_path` is non-empty (and cfg.intro_card), a cfg.intro_sec
// animated pre-roll is prepended: full-width thumbnail on a blurred
// backdrop, centered play button, a cursor that flies in and "clicks" it
// (whoosh + tick), then a crossfade into the clip — so viewers instantly
// see which video the short came from. 916blur/916crop only; if the
// pre-roll graph fails, the cut is retried without it.
void cut_clip(const std::string& source_file,
              const Clip& clip,
              const DownloadResult& dl,
              Format fmt,
              const Config& cfg,
              const std::string& out_file,
              const std::string& thumb_path = {},
              const std::string& outro_path = {});

} // namespace crux::media
