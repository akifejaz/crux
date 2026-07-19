// Embedded intro pre-roll graphics (play button + mouse cursor), generated
// by tools/gen_intro_assets.py into intro_assets.cpp. No external asset
// files to ship — the PNGs are written into the work dir at cut time.
#pragma once

#include <string>

namespace crux::media {

struct IntroAssets {
    std::string play_png;
    std::string cursor_png;
};

// Writes play.png / cursor.png into `dir` (skips existing) and returns paths.
IntroAssets write_intro_assets(const std::string& dir);

} // namespace crux::media
