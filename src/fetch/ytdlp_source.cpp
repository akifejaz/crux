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
            // Surface actionable hints (DPAPI, bot check, geo-lock, private).
            std::string hint;
            const auto& err = r.stderr_text;
            if (err.find("Failed to decrypt with DPAPI") != std::string::npos ||
                err.find("app-bound") != std::string::npos) {
                // Chrome 127+ on Windows moved cookies to app-bound encryption
                // and yt-dlp can't decrypt them from another process
                // (yt-dlp/yt-dlp#10927). Try a different browser or none.
                hint = " Hint: Chrome cookies can't be read on recent Chrome for Windows "
                       "(yt-dlp #10927). Switch the cookie source to Edge, Firefox, or "
                       "Brave, or set it to None if this video is public.";
            } else if (err.find("could not find") != std::string::npos &&
                       err.find("cookies database") != std::string::npos) {
                // yt-dlp couldn't find that browser's profile — it isn't
                // installed, or the profile lives in a non-default path.
                hint = " Hint: that browser isn't installed. Try a different one "
                       "(Edge, Firefox, Brave), or set the cookie source to None — "
                       "public videos don't need cookies.";
            } else if (err.find("confirm you're not a bot") != std::string::npos) {
                hint = " Hint: YouTube's bot check triggered. Try --cookies-from-browser edge "
                       "(or firefox/brave — whichever you're signed into YouTube with). "
                       "Chrome on Windows may fail with DPAPI errors.";
            } else if (err.find("age") != std::string::npos ||
                       err.find("Sign in to confirm") != std::string::npos) {
                hint = " Hint: this video may require sign-in. Try --cookies-from-browser edge "
                       "(or firefox/brave).";
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
