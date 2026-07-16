#pragma once

#include "config.h"

namespace crux {

// Runs the whole pipeline. Returns process exit code per PLAN §3.3.
int run_pipeline(const Config& cfg);

} // namespace crux
