// Renders the "Watch full video here" outro card as a static JPG using
// ffmpeg's drawtext filter. One card per run — reused across every clip.
#pragma once

#include "config.h"
#include "core/heatmap.h"
#include "fetch/subs.h"

#include <optional>
#include <string>

namespace crux::media {

// Composes a 1080x1920 JPG at `out_jpg`: banner strip on top, circular avatar,
// channel name + handle + follower count, "Watch full video" CTA, video
// title. Uses whichever of Segoe UI / DejaVu Sans / Liberation Sans is found
// on the host — returns std::nullopt if no usable font exists or the ffmpeg
// call fails; callers cut without the outro in that case.
std::optional<std::string> render_outro_card(
    const VideoMeta& video,
    const fetch::ChannelAssets& assets,
    const Config& cfg,
    const std::string& out_jpg);

} // namespace crux::media
