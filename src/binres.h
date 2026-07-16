// Resolves the yt-dlp / ffmpeg executables at runtime.
// Order (PLAN §2): --flag → env var → bundled ./third_party/bin → PATH.
#pragma once

#include <optional>
#include <string>

namespace crux::binres {

// Resolve yt-dlp. Returns absolute (or bare, if PATH-resolved) path.
// Throws std::runtime_error if not found.
std::string resolve_ytdlp(const std::optional<std::string>& override_path);

// Resolve ffmpeg. Same rules as above.
std::string resolve_ffmpeg(const std::optional<std::string>& override_path);

// Also used for ffprobe if we need it later.
std::string resolve_ffprobe(const std::optional<std::string>& override_path);

} // namespace crux::binres
