#include "fetch/subs.h"
#include "binres.h"
#include "media/proc.h"

#include <spdlog/spdlog.h>

#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

namespace crux::fetch {

std::optional<SubsResult> fetch_subtitles(const std::string& url_or_id,
                                          const Config& cfg,
                                          const std::string& dir) {
    const std::string exe = binres::resolve_ytdlp(cfg.ytdlp_path);
    fs::create_directories(dir);
    const std::string out_tpl = (fs::path(dir) / "subs").string();

    std::vector<std::string> args = {
        "--skip-download",
        "--write-subs",
        "--write-auto-subs",
        "--sub-langs", cfg.captions_langs,
        "--sub-format", "vtt",
        "--no-warnings",
        "--no-playlist",
        "-o", out_tpl,
    };
    if (cfg.cookies_from_browser) {
        args.push_back("--cookies-from-browser");
        args.push_back(*cfg.cookies_from_browser);
    }
    args.push_back(url_or_id);

    proc::RunOptions opts;
    opts.capture_stdout = true;
    opts.capture_stderr = true;
    opts.timeout_ms = 120000;

    spdlog::debug("running yt-dlp subtitle fetch: {}", exe);
    proc::RunResult r = proc::run(exe, args, opts);
    if (r.exit_code != 0) {
        // Partial failures are common (one language 429s while another
        // downloaded fine) — log and fall through to the directory scan.
        spdlog::debug("subtitle fetch exit {} — checking for partial results: {}",
                      r.exit_code, r.stderr_text);
    }

    // yt-dlp writes subs.<lang>.vtt for every matched language. Pick by the
    // research preference order: original speech first, then English.
    static const char* kPreferred[] = {"hi", "ur", "en-orig", "en"};
    for (const char* lang : kPreferred) {
        fs::path p = fs::path(dir) / (std::string("subs.") + lang + ".vtt");
        if (fs::exists(p)) return SubsResult{p.string(), lang};
    }
    // Fallback: any .vtt that appeared (e.g. en-US).
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (e.path().extension() == ".vtt") {
            std::string stem = e.path().stem().string();   // "subs.<lang>"
            auto dot = stem.find('.');
            std::string lang = dot == std::string::npos ? "" : stem.substr(dot + 1);
            return SubsResult{e.path().string(), lang};
        }
    }
    return std::nullopt;
}

} // namespace crux::fetch
