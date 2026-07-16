#include "fetch/source.h"
#include "fetch/parse.h"
#include "binres.h"
#include "media/proc.h"

#include <spdlog/spdlog.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace crux::fetch {

namespace {

class YtdlpSource : public IHeatmapSource {
public:
    FetchResult fetch(const std::string& url_or_id, const Config& cfg) override {
        const std::string exe = binres::resolve_ytdlp(cfg.ytdlp_path);
        std::vector<std::string> args = {
            "--skip-download",
            "--dump-single-json",
            "--no-warnings",
            "--no-playlist",
        };
        if (cfg.cookies_from_browser) {
            args.push_back("--cookies-from-browser");
            args.push_back(*cfg.cookies_from_browser);
        }
        args.push_back(url_or_id);

        proc::RunOptions opts;
        opts.capture_stdout = true;
        opts.capture_stderr = true;
        opts.timeout_ms = 60000;

        spdlog::debug("running yt-dlp fetch: {}", exe);
        proc::RunResult r = proc::run(exe, args, opts);
        if (r.exit_code != 0) {
            // Surface actionable hints (bot check, geo-lock, private).
            std::string hint;
            const auto& err = r.stderr_text;
            if (err.find("confirm you're not a bot") != std::string::npos) {
                hint = " Hint: pass --cookies-from-browser chrome (or firefox).";
            } else if (err.find("age") != std::string::npos ||
                       err.find("Sign in to confirm") != std::string::npos) {
                hint = " Hint: this video may require sign-in; try --cookies-from-browser.";
            }
            throw std::runtime_error("yt-dlp failed (" + std::to_string(r.exit_code) +
                                     "): " + err + hint);
        }
        return parse_ytdlp_json(r.stdout_text);
    }
};

} // namespace

std::unique_ptr<IHeatmapSource> make_ytdlp_source() {
    return std::make_unique<YtdlpSource>();
}

// Stub — real implementation lands in M5.
std::unique_ptr<IHeatmapSource> make_native_source() {
    throw std::runtime_error("native heatmap source not implemented until M5");
}

} // namespace crux::fetch
