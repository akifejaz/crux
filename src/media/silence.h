#pragma once

#include "config.h"
#include "core/heatmap.h"

#include <string>
#include <vector>

namespace crux::media {

struct SilenceWindow {
    double start_sec = 0.0;
    double end_sec = 0.0;
};

// Runs ffmpeg silencedetect over [center-window, center+window] and returns
// silence intervals in the source-file timeline. M4-only.
std::vector<SilenceWindow> detect_silences(const std::string& source_file,
                                           double center_sec,
                                           double window_sec,
                                           const Config& cfg);

// Snap `t` to the nearest silence boundary within ±max_shift seconds.
double snap_to_silence(double t,
                       const std::vector<SilenceWindow>& silences,
                       double max_shift);

} // namespace crux::media
