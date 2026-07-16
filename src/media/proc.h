// Cross-platform subprocess wrapper. Small enough to hand-roll (no reproc dep).
// - Captures stdout+stderr as strings.
// - Returns exit code (0..N) or negative on spawn failure.
// - Supports optional timeout (ms).
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace ytshorts::proc {

struct RunResult {
    int exit_code = -1;
    std::string stdout_text;
    std::string stderr_text;
    bool timed_out = false;
};

struct RunOptions {
    std::optional<int> timeout_ms;    // std::nullopt = no timeout
    bool capture_stdout = true;
    bool capture_stderr = true;
    bool merge_streams = false;       // if true, stderr routed into stdout_text
    // When true, the child's stdout/stderr are inherited from the parent (so
    // yt-dlp / ffmpeg progress prints straight to the terminal in real time).
    // If inherit_* is true, capture of that stream is disabled regardless.
    bool inherit_stdout = false;
    bool inherit_stderr = false;
    // If set, both stdout and stderr of the child are written directly to
    // this file (opened O_TRUNC on POSIX / CREATE_ALWAYS on Windows, with a
    // shared handle so a concurrent reader can tail it). Overrides all
    // capture_*, inherit_*, and merge_streams settings.
    std::optional<std::string> redirect_output_to;
};

// Runs `exe` with `args`, blocks until exit (or timeout). No shell involved.
// `args` should NOT include argv[0]; it's constructed from `exe`.
RunResult run(const std::string& exe,
              const std::vector<std::string>& args,
              const RunOptions& opts = {});

// Convenience: same as run() but raises std::runtime_error if exit_code != 0.
RunResult check_run(const std::string& exe,
                    const std::vector<std::string>& args,
                    const RunOptions& opts = {});

} // namespace ytshorts::proc
