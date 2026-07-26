// Small formatting helpers shared across pipeline + media modules. Pure —
// no I/O, no globals. Split out to eliminate the utf8_truncate / hms
// duplicates that existed in pipeline.cpp and outro_card.cpp.
#pragma once

#include <cstddef>
#include <string>

namespace crux::text {

// Truncate a UTF-8 string to at most `max_bytes`, walking back to a valid
// codepoint boundary. Prevents partial multi-byte sequences from breaking
// JSON serialization (nlohmann::json throws on invalid UTF-8).
std::string utf8_truncate(std::string s, std::size_t max_bytes);

// Format seconds as "hh:mm:ss" (rounded).
std::string hms(double seconds);

// Format seconds as "hh:mm:ss.mmm" if you need sub-second precision.
std::string ftos(double v);

} // namespace crux::text
