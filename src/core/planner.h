#pragma once

#include "config.h"
#include "core/detector.h"
#include "core/heatmap.h"

#include <vector>

namespace crux::planner {

// Runs steps 8..11 of PLAN §4 (select, window, emit).
// Silence-snap (step 10) is done later in the media stage; this returns
// snapped=false for both endpoints.
Plan plan(const detector::DetectResult& d,
          const Heatmap& h,
          double duration_sec,
          const std::vector<Chapter>& chapters,
          const Config& cfg);

} // namespace crux::planner
