#pragma once

#include "core/captions.h"

#include <string>

namespace crux::captions {

// Parses WebVTT text (YouTube manual or auto captions) into cues.
// - strips inline tags (<c>, <00:00:00.000>, …)
// - collapses the rolling-repeat lines auto captions emit (each line appears
//   in two consecutive cues; we keep its first appearance)
// - skips WEBVTT header, NOTE/STYLE blocks and metadata lines
// Pure function — no I/O.
CaptionDoc parse_vtt(const std::string& vtt_text);

} // namespace crux::captions
