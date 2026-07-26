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
// gracefully to heatmap-only planning. Retries transparently on YouTube's
// HTTP 429 with 0/15/30 s backoff.
std::optional<SubsResult> fetch_subtitles(const std::string& url_or_id,
                                          const Config& cfg,
                                          const std::string& dir);

// True when the most recent fetch_subtitles() call on this thread returned
// nullopt purely because YouTube rate-limited every attempt. Lets the
// pipeline print a "wait a minute and retry" message instead of the generic
// "video has no captions" fallback.
bool subtitle_fetch_was_rate_limited();

// Downloads the video's thumbnail as JPEG into `dir` via yt-dlp (for the
// clip intro card). Returns the image path, or std::nullopt on failure —
// callers cut without the intro card.
std::optional<std::string> fetch_thumbnail(const std::string& url_or_id,
                                           const Config& cfg,
                                           const std::string& dir);

// Channel branding assets used by the outro card. Any field may be empty on
// partial success (e.g. a channel with no banner uploaded).
struct ChannelAssets {
    std::string banner_path;   // widescreen banner JPG
    std::string avatar_path;   // square avatar JPG
};

// Downloads the channel's banner + avatar as JPEG via yt-dlp using the
// channel URL (from parse.cpp). Best-effort; returns std::nullopt if nothing
// usable arrived.
std::optional<ChannelAssets> fetch_channel_assets(const std::string& channel_url,
                                                  const Config& cfg,
                                                  const std::string& dir);

} // namespace crux::fetch
