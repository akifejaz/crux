#include "media/cutter.h"

#include "binres.h"
#include "media/proc.h"

#include <spdlog/spdlog.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace fs = std::filesystem;

namespace crux::media {

namespace {

// Filter graph with an explicit named output [v] so we can -map it and keep
// the audio track (`-map 0:a?`).
const char* filter_for(Format f) {
    switch (f) {
    case Format::Blur916:
        return "[0:v]split=2[bg][fg];"
               "[bg]scale=1080:1920:force_original_aspect_ratio=increase,"
               "crop=1080:1920,gblur=sigma=24[b];"
               "[fg]scale=1080:-2[f];"
               "[b][f]overlay=(W-w)/2:(H-h)/2[v]";
    case Format::Crop916:
        return "[0:v]scale=-2:1920,crop=1080:1920[v]";
    case Format::Orig:
        return nullptr;   // no filter graph — direct re-encode
    }
    return nullptr;
}

std::string ftos(double v) {
    std::ostringstream oss; oss.setf(std::ios::fixed); oss.precision(3); oss << v;
    return oss.str();
}

void preflight_input(const std::string& path) {
    std::error_code ec;
    if (!fs::exists(path, ec) || ec) {
        throw std::runtime_error("input file missing: " + path);
    }
    auto sz = fs::file_size(path, ec);
    if (ec) throw std::runtime_error("cannot stat input: " + path);
    if (sz < 64 * 1024) {
        throw std::runtime_error(
            "input file is " + std::to_string(sz) + " bytes — the download "
            "produced an empty/corrupted file. Retry, or add "
            "--cookies-from-browser chrome to bypass YouTube's bot check.");
    }
}

} // namespace

void cut_clip(const std::string& source_file,
              const Clip& clip,
              const DownloadResult& dl,
              Format fmt,
              const Config& cfg,
              const std::string& out_file) {
    const std::string ffmpeg = binres::resolve_ffmpeg(cfg.ffmpeg_path);

    preflight_input(source_file);

    // Translate video-time (clip.start_sec) → file-time when sections were
    // used. Sections mode writes each range starting at file-time 0. Full
    // download mode leaves dl.sections empty and we cut directly.
    double s = clip.start_sec;
    double e = clip.end_sec;
    if (!dl.sections.empty()) {
        for (const auto& sec : dl.sections) {
            if (clip.start_sec >= sec.src_start_sec && clip.end_sec <= sec.src_end_sec) {
                s = clip.start_sec - sec.src_start_sec + sec.file_start_sec;
                e = clip.end_sec   - sec.src_start_sec + sec.file_start_sec;
                break;
            }
        }
    }

    std::vector<std::string> args = {
        "-y",
        "-loglevel", "warning",
        "-stats",
        "-ss", ftos(s),
        "-to", ftos(e),
        "-i", source_file,
    };

    const char* filt = filter_for(fmt);
    if (filt) {
        args.insert(args.end(), {
            "-filter_complex", filt,
            // Explicit stream mapping: filtered video + optional audio track.
            "-map", "[v]",
            "-map", "0:a?",
        });
    }
    args.insert(args.end(), {
        "-c:v", "libx264", "-preset", "veryfast", "-crf", "20",
        "-pix_fmt", "yuv420p",
        "-c:a", "aac", "-b:a", "160k",
        "-movflags", "+faststart",
        out_file,
    });

    proc::RunOptions opts;
    opts.timeout_ms = 15 * 60 * 1000;
    opts.inherit_stdout = true;
    opts.inherit_stderr = true;
    opts.capture_stdout = false;
    opts.capture_stderr = false;
    proc::RunResult r = proc::run(ffmpeg, args, opts);
    if (r.exit_code != 0) {
        throw std::runtime_error("ffmpeg cut failed (exit " +
                                 std::to_string(r.exit_code) +
                                 "). See ffmpeg output above.");
    }

    // Sanity check the output.
    std::error_code ec;
    if (!fs::exists(out_file, ec) || fs::file_size(out_file, ec) < 32 * 1024) {
        throw std::runtime_error("ffmpeg produced empty output: " + out_file);
    }
}

} // namespace crux::media
