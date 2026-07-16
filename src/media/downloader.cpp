#include "media/downloader.h"

#include "binres.h"
#include "media/proc.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace crux::media {

namespace {

// Minimum plausible size for a working video download. Section downloads that
// fail due to bot checks or 403/400 from googlevideo often leave a container
// stub of a few hundred bytes ("Output file is empty, nothing was encoded").
constexpr std::uintmax_t kMinPlausibleBytes = 64 * 1024;   // 64 KiB

std::string format_time_range(double s, double e) {
    std::ostringstream oss;
    oss.setf(std::ios::fixed); oss.precision(2);
    oss << "*" << s << "-" << e;
    return oss.str();
}

double total_clip_seconds(const std::vector<Clip>& clips) {
    double s = 0.0;
    for (const auto& c : clips) s += c.end_sec - c.start_sec;
    return s;
}

// Common yt-dlp args used by both section and full-download attempts.
std::vector<std::string> base_args(const std::string& out_template,
                                   const Config& cfg) {
    std::vector<std::string> a = {
        // Prefer H.264 (avc1) — much faster to decode than AV1 and yt-dlp can
        // use HTTP range requests instead of streaming through ffmpeg. Fall
        // back to any mp4 if avc1 unavailable.
        "-f",
        "bv*[height<=1080][vcodec^=avc1]+ba[ext=m4a]"
        "/bv*[height<=720][vcodec^=avc1]+ba"
        "/bv*[height<=1080][ext=mp4]+ba[ext=m4a]"
        "/b[ext=mp4]/b",
        "--concurrent-fragments", "4",
        "--merge-output-format", "mp4",
        "--retries", "10",
        "--fragment-retries", "10",
        "--no-abort-on-error",
        "--force-overwrites",
        "--no-warnings",
        "--no-playlist",
        "-o", out_template,
    };
    if (cfg.cookies_from_browser) {
        a.push_back("--cookies-from-browser");
        a.push_back(*cfg.cookies_from_browser);
    }
    return a;
}

// Find the .mp4 that yt-dlp actually wrote (may be `<id>.mp4` or the temp
// merger output). Returns empty string if none found.
std::string find_output_mp4(const std::string& work_dir, const std::string& id) {
    fs::path expected = fs::path(work_dir) / (id + ".mp4");
    if (fs::exists(expected)) return expected.string();
    for (auto& e : fs::directory_iterator(work_dir)) {
        if (e.is_regular_file() && e.path().extension() == ".mp4" &&
            e.path().filename().string().rfind(id, 0) == 0) {
            return e.path().string();
        }
    }
    return {};
}

// Validate that the downloaded file is plausibly a real video.
// Returns std::nullopt on OK, else a human-readable reason.
std::optional<std::string> validate_output(const std::string& path,
                                           double min_seconds_expected) {
    if (path.empty()) return "no output mp4 found";
    std::error_code ec;
    auto sz = fs::file_size(path, ec);
    if (ec) return "cannot stat output file";
    if (sz < kMinPlausibleBytes) {
        return "output file is " + std::to_string(sz) +
               " bytes — download almost certainly failed";
    }
    // Rough size check: ~50 KB/s is well below any reasonable video encoding.
    if (min_seconds_expected > 0.0) {
        const double bytes_per_sec = static_cast<double>(sz) / min_seconds_expected;
        if (bytes_per_sec < 50000.0) {
            return "output is only " + std::to_string(sz) +
                   " bytes for ~" + std::to_string(static_cast<int>(min_seconds_expected)) +
                   "s of video — likely truncated";
        }
    }
    return std::nullopt;
}

void remove_stale(const std::string& work_dir, const std::string& id) {
    for (auto& e : fs::directory_iterator(work_dir)) {
        auto name = e.path().filename().string();
        if (name.rfind(id, 0) == 0) {
            std::error_code ec;
            fs::remove(e.path(), ec);
        }
    }
}

int run_ytdlp(const std::string& exe, const std::vector<std::string>& args) {
    proc::RunOptions opts;
    opts.timeout_ms = 60 * 60 * 1000;   // 60 min budget for full downloads
    opts.inherit_stdout = true;
    opts.inherit_stderr = true;
    opts.capture_stdout = false;
    opts.capture_stderr = false;
    proc::RunResult r = proc::run(exe, args, opts);
    return r.exit_code;
}

std::string cookies_hint(const Config& cfg) {
    if (cfg.cookies_from_browser) return {};
    return "\n"
           "hint: YouTube's bot check often rejects section downloads.\n"
           "      Retry with:  --cookies-from-browser chrome\n"
           "      (or firefox / edge — whichever browser you're signed in to).";
}

} // namespace

DownloadResult download(const std::string& url,
                        const VideoMeta& meta,
                        const std::vector<Clip>& clips,
                        const Config& cfg,
                        const std::string& work_dir) {
    const std::string exe = binres::resolve_ytdlp(cfg.ytdlp_path);
    fs::create_directories(work_dir);

    const double sum = total_clip_seconds(clips);
    // Only try sections when they'd meaningfully save bandwidth AND the clip
    // total covers more than a trivial amount (single-clip 30s section fetches
    // are often broken by bot checks; full download of a 20-min video is a few
    // dozen MB and takes seconds).
    const bool sections_ok = !cfg.full_download &&
        meta.duration_sec > 0.0 &&
        sum < 0.20 * meta.duration_sec &&
        meta.duration_sec > 30.0 * 60.0;    // only for videos > 30 min

    DownloadResult out;
    const fs::path outpl = fs::path(work_dir) / (meta.id + ".%(ext)s");

    // -------- Attempt 1: sections --------
    if (sections_ok) {
        spdlog::debug("attempting section download ({} sections)", clips.size());
        std::vector<std::string> args = base_args(outpl.string(), cfg);
        for (const auto& c : clips) {
            args.push_back("--download-sections");
            double s = std::max(0.0, c.start_sec - 10.0);
            double e = std::min(meta.duration_sec, c.end_sec + 10.0);
            args.push_back(format_time_range(s, e));
            out.sections.push_back({s, e, /*file_start_sec=*/0.0});
        }
        args.push_back(url);

        const int rc = run_ytdlp(exe, args);
        std::string path = find_output_mp4(work_dir, meta.id);
        auto err = validate_output(path, sum);
        if (rc == 0 && !err) {
            out.source_file = path;
            return out;
        }
        std::string reason = (rc != 0) ? "yt-dlp returned non-zero exit code"
                                       : "yt-dlp produced an invalid file";
        if (err) reason += " (" + *err + ")";
        std::fprintf(stderr,
            "\n[warn] section download failed: %s — falling back to full download.\n",
            reason.c_str());
        remove_stale(work_dir, meta.id);
        out.sections.clear();
    }

    // -------- Attempt 2: full download --------
    spdlog::debug("full video download");
    std::vector<std::string> args = base_args(outpl.string(), cfg);
    args.push_back(url);

    const int rc = run_ytdlp(exe, args);
    std::string path = find_output_mp4(work_dir, meta.id);
    auto err = validate_output(path, meta.duration_sec);
    if (rc != 0 || err) {
        std::string msg = "yt-dlp download failed";
        if (rc != 0) msg += " (exit " + std::to_string(rc) + ")";
        if (err)     msg += ": " + *err;
        msg += cookies_hint(cfg);
        throw std::runtime_error(msg);
    }
    out.source_file = path;
    return out;
}

} // namespace crux::media
