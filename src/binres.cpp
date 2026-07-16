#include "binres.h"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace crux::binres {

namespace {

#ifdef _WIN32
constexpr const char* kExeSuffix = ".exe";
constexpr char kPathSep = ';';
#else
constexpr const char* kExeSuffix = "";
constexpr char kPathSep = ':';
#endif

std::string exe_name(std::string_view base) {
    return std::string(base) + kExeSuffix;
}

fs::path executable_dir() {
#ifdef _WIN32
    // On Windows we could use GetModuleFileName; for portability we fall back
    // to argv[0]-derived paths via /proc/self/exe when available. To keep this
    // header-only-portable, use the current working directory as a hint plus
    // the well-known third_party/bin folder next to it.
    return fs::current_path();
#else
    std::error_code ec;
    auto p = fs::read_symlink("/proc/self/exe", ec);
    if (!ec && !p.empty()) return p.parent_path();
    return fs::current_path();
#endif
}

std::vector<fs::path> bundled_candidates() {
    std::vector<fs::path> out;
    fs::path exe_dir = executable_dir();
    out.push_back(exe_dir / "third_party" / "bin");
    // Common dev-tree layout: build/ next to third_party/
    out.push_back(exe_dir.parent_path() / "third_party" / "bin");
    // Current working directory relative
    out.push_back(fs::current_path() / "third_party" / "bin");
    return out;
}

std::optional<std::string> find_on_path(const std::string& exe) {
    const char* raw = std::getenv("PATH");
    if (!raw) return std::nullopt;
    std::string path{raw};
    std::string acc;
    auto try_dir = [&](const std::string& d) -> std::optional<std::string> {
        if (d.empty()) return std::nullopt;
        fs::path candidate = fs::path(d) / exe;
        std::error_code ec;
        if (fs::exists(candidate, ec) && !fs::is_directory(candidate, ec))
            return candidate.string();
        return std::nullopt;
    };
    for (char ch : path) {
        if (ch == kPathSep) {
            if (auto hit = try_dir(acc)) return hit;
            acc.clear();
        } else {
            acc.push_back(ch);
        }
    }
    if (!acc.empty()) {
        if (auto hit = try_dir(acc)) return hit;
    }
    return std::nullopt;
}

std::string resolve(std::string_view base,
                    const char* env_var,
                    const std::optional<std::string>& override_path) {
    // 1) explicit flag
    if (override_path && !override_path->empty()) {
        std::error_code ec;
        if (fs::exists(*override_path, ec)) return *override_path;
        throw std::runtime_error(std::string("path not found: ") + *override_path);
    }
    // 2) env var
    if (env_var) {
        if (const char* env = std::getenv(env_var)) {
            std::string s{env};
            std::error_code ec;
            if (!s.empty() && fs::exists(s, ec)) return s;
        }
    }
    const std::string exe = exe_name(base);
    // 3) bundled
    for (const auto& d : bundled_candidates()) {
        fs::path candidate = d / exe;
        std::error_code ec;
        if (fs::exists(candidate, ec) && !fs::is_directory(candidate, ec))
            return candidate.string();
    }
    // 4) PATH
    if (auto hit = find_on_path(exe)) return *hit;
    throw std::runtime_error(std::string(base) +
        " not found. Provide --" + std::string(base) +
        " PATH, set the env var, drop the binary in third_party/bin/, or add it to PATH.");
}

} // namespace

std::string resolve_ytdlp(const std::optional<std::string>& override_path) {
    return resolve("yt-dlp", "CRUX_YTDLP", override_path);
}
std::string resolve_ffmpeg(const std::optional<std::string>& override_path) {
    return resolve("ffmpeg", "CRUX_FFMPEG", override_path);
}
std::string resolve_ffprobe(const std::optional<std::string>& override_path) {
    return resolve("ffprobe", "CRUX_FFPROBE", override_path);
}

} // namespace crux::binres
