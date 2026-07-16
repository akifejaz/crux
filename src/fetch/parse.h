// Pure JSON → data-model parser for yt-dlp --dump-single-json output.
// No I/O, no network — unit-testable against fixtures.
#pragma once

#include "core/heatmap.h"

#include <string>

namespace ytshorts::fetch {

// Parses a yt-dlp info-JSON string. Throws std::runtime_error on bad JSON or
// missing critical fields (id, duration).
// heatmap remains std::nullopt when the JSON's `heatmap` field is null/absent.
FetchResult parse_ytdlp_json(const std::string& json_text);

} // namespace ytshorts::fetch
