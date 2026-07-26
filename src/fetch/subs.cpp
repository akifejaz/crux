#include "fetch/subs.h"
#include "binres.h"
#include "media/proc.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <filesystem>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace crux::fetch {

namespace {

// Any .vtt in `dir` counts as a partial success — even if yt-dlp exited
// non-zero we may already have one of the requested languages on disk.
bool any_vtt_present(const std::string& dir) {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (e.path().extension() == ".vtt") return true;
    }
    return false;
}

// yt-dlp surfaces the origin's HTTP response verbatim. YouTube's rate limit
// is a distinct failure mode we want to retry instead of falling straight
// through to the "no captions" branch.
bool is_rate_limited(const std::string& stderr_text) {
    return stderr_text.find("HTTP Error 429") != std::string::npos ||
           stderr_text.find("Too Many Requests") != std::string::npos;
}

// Signalled to callers so the pipeline can print a clearer error when it
// exhausts both fallbacks (heatmap and captions) purely because of throttling.
thread_local bool g_last_fetch_rate_limited = false;

} // namespace

bool subtitle_fetch_was_rate_limited() { return g_last_fetch_rate_limited; }

namespace {

// Splits "hi,ur,en" into ["hi", "ur", "en"]. Trims whitespace; ignores
// empty tokens.
std::vector<std::string> split_langs(const std::string& csv) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : csv) {
        if (c == ',') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else if (c == ' ' || c == '\t') {
            // skip
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// Try to download ONE language. Returns the .vtt path on success, empty on
// terminal failure. Sets `rate_limited` if all internal retries hit HTTP 429.
std::string fetch_one_language(const std::string& exe,
                               const std::string& url_or_id,
                               const std::string& dir,
                               const std::string& out_tpl,
                               const std::string& lang,
                               const Config& cfg,
                               bool& rate_limited) {
    rate_limited = false;
    std::vector<std::string> args = {
        "--skip-download",
        "--write-subs",
        "--write-auto-subs",
        "--sub-langs", lang,
        "--sub-format", "vtt",
        "--no-warnings",
        "--no-playlist",
        "--sleep-subtitles", "1",
        "--retries",         "10",
        "--retry-sleep",     "linear=1:8:2",
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

    // Per-language outer retry: yt-dlp's internal --retries handles brief
    // 429s, we handle the multi-minute cool-off with these longer sleeps.
    static const int kWaitSecs[] = {0, 30, 60};
    proc::RunResult r;
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (kWaitSecs[attempt] > 0) {
            spdlog::info("caption fetch for '{}' rate-limited — waiting {} s "
                         "and retrying (attempt {}/3)",
                         lang, kWaitSecs[attempt], attempt + 1);
            std::this_thread::sleep_for(
                std::chrono::seconds(kWaitSecs[attempt]));
        }
        spdlog::debug("running yt-dlp subtitle fetch ({}): {}", lang, exe);
        r = proc::run(exe, args, opts);
        if (r.exit_code == 0) break;
        spdlog::debug("subtitle fetch ({}) exit {}: {}",
                      lang, r.exit_code, r.stderr_text);
        if (!is_rate_limited(r.stderr_text)) break;
        rate_limited = true;
    }

    // yt-dlp names the file subs.<lang>.vtt on success, but for some
    // language codes YouTube substitutes a locale variant (en → en-US).
    // First look for the exact match, then scan for any newly-arrived VTT
    // (the dir may already have prior-language files, but if this call
    // succeeded, `exists` on the exact-name path is the truth).
    fs::path p = fs::path(dir) / (std::string("subs.") + lang + ".vtt");
    if (fs::exists(p)) return p.string();
    return {};
}

} // namespace

std::optional<SubsResult> fetch_subtitles(const std::string& url_or_id,
                                          const Config& cfg,
                                          const std::string& dir) {
    g_last_fetch_rate_limited = false;
    const std::string exe = binres::resolve_ytdlp(cfg.ytdlp_path);
    fs::create_directories(dir);
    const std::string out_tpl = (fs::path(dir) / "subs").string();

    // Walk the requested languages in the user's preferred order, but stop
    // on the FIRST one that lands a .vtt on disk. This matters for two
    // reasons: (a) each fetch is one YouTube request, so trying one at a
    // time keeps us well under the per-IP rate window; (b) we don't need
    // multiple translations of the same content, only the best-available.
    std::vector<std::string> langs = split_langs(cfg.captions_langs);
    if (langs.empty()) langs = {"en"};

    bool any_rate_limited = false;
    for (const auto& lang : langs) {
        bool rl = false;
        std::string path = fetch_one_language(exe, url_or_id, dir, out_tpl,
                                              lang, cfg, rl);
        if (!path.empty()) return SubsResult{path, lang};
        if (rl) any_rate_limited = true;
    }

    // Nothing landed via the requested langs — do a final scan in case
    // yt-dlp renamed a track (e.g. "en" → "en-US" locale substitution).
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (e.path().extension() == ".vtt") {
            std::string stem = e.path().stem().string();   // "subs.<lang>"
            auto dot = stem.find('.');
            std::string lang = dot == std::string::npos ? "" : stem.substr(dot + 1);
            return SubsResult{e.path().string(), lang};
        }
    }
    g_last_fetch_rate_limited = any_rate_limited;
    return std::nullopt;
}

namespace {

// Picks the widest and squarest images out of yt-dlp's --write-all-thumbnails
// dump, based on ffprobe-reported dimensions. Files are laid out as
//   <stem>.<id>.jpg  or  <stem>.<label>.jpg  where <label> can be
//   "avatar_uncropped" / "banner_uncropped" — we handle both.
struct Cand {
    std::string path;
    int w = 0, h = 0;
};

int probe_dim(const std::string& ffprobe, const std::string& path, bool want_w) {
    proc::RunOptions opts;
    opts.capture_stdout = true;
    opts.timeout_ms = 10000;
    std::vector<std::string> args = {
        "-v", "error",
        "-select_streams", "v:0",
        "-show_entries", "stream=" + std::string(want_w ? "width" : "height"),
        "-of", "csv=p=0",
        path,
    };
    proc::RunResult r = proc::run(ffprobe, args, opts);
    if (r.exit_code != 0) return 0;
    try { return std::stoi(r.stdout_text); } catch (...) { return 0; }
}

} // namespace

std::optional<ChannelAssets> fetch_channel_assets(const std::string& channel_url,
                                                  const Config& cfg,
                                                  const std::string& dir) {
    if (channel_url.empty()) return std::nullopt;
    const std::string ytdlp = binres::resolve_ytdlp(cfg.ytdlp_path);
    const std::string ffmpeg = binres::resolve_ffmpeg(cfg.ffmpeg_path);
    const std::string ffprobe = binres::resolve_ffprobe(cfg.ffmpeg_path);
    fs::create_directories(dir);
    const std::string out_tpl = (fs::path(dir) / "chan").string();
    const std::string ffmpeg_dir = fs::path(ffmpeg).parent_path().string();

    std::vector<std::string> args = {
        "--skip-download",
        "--write-all-thumbnails",
        "--convert-thumbnails", "jpg",
        "--playlist-items", "0",
        "--no-warnings",
        "-o", out_tpl,
    };
    if (!ffmpeg_dir.empty()) {
        args.push_back("--ffmpeg-location");
        args.push_back(ffmpeg_dir);
    }
    if (cfg.cookies_from_browser) {
        args.push_back("--cookies-from-browser");
        args.push_back(*cfg.cookies_from_browser);
    }
    args.push_back(channel_url);

    proc::RunOptions opts;
    opts.capture_stdout = true;
    opts.capture_stderr = true;
    opts.timeout_ms = 60000;

    spdlog::debug("running yt-dlp channel-assets fetch: {}", channel_url);
    proc::RunResult r = proc::run(ytdlp, args, opts);
    if (r.exit_code != 0)
        spdlog::debug("channel-assets exit {}: {}", r.exit_code, r.stderr_text);

    // Score every chan*.jpg by dimensions; widest = banner, most-square = avatar.
    std::vector<Cand> jpgs;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file()) continue;
        const auto& p = e.path();
        if (p.extension() != ".jpg") continue;
        if (p.filename().string().rfind("chan", 0) != 0) continue;
        Cand c;
        c.path = p.string();
        c.w = probe_dim(ffprobe, c.path, true);
        c.h = probe_dim(ffprobe, c.path, false);
        if (c.w <= 0 || c.h <= 0) continue;
        jpgs.push_back(std::move(c));
    }
    if (jpgs.empty()) return std::nullopt;

    ChannelAssets out;
    // Prefer the uncropped variants when present — they carry the source
    // upload aspect, whereas the numbered variants are pre-cropped 6:1
    // display strips that are too short for a card layout.
    const Cand* best_banner = nullptr;
    const Cand* best_avatar = nullptr;
    for (const auto& c : jpgs) {
        const std::string name = fs::path(c.path).filename().string();
        const bool is_banner_hint = name.find("banner") != std::string::npos;
        const bool is_avatar_hint = name.find("avatar") != std::string::npos;
        const double ratio = static_cast<double>(c.w) / c.h;

        // Banner: uncropped file wins outright; otherwise widest widescreen
        // that's tall enough to crop a card strip from.
        if (is_banner_hint || (ratio >= 1.5 && c.h >= 240)) {
            const bool cur_hint = best_banner &&
                fs::path(best_banner->path).filename().string()
                    .find("banner") != std::string::npos;
            if (!best_banner || (is_banner_hint && !cur_hint) ||
                (is_banner_hint == cur_hint &&
                 c.w * c.h > best_banner->w * best_banner->h))
                best_banner = &c;
        }
        // Avatar: uncropped file wins outright; otherwise largest square-ish.
        if (is_avatar_hint || (ratio > 0.7 && ratio < 1.3)) {
            const bool cur_hint = best_avatar &&
                fs::path(best_avatar->path).filename().string()
                    .find("avatar") != std::string::npos;
            if (!best_avatar || (is_avatar_hint && !cur_hint) ||
                (is_avatar_hint == cur_hint &&
                 c.w * c.h > best_avatar->w * best_avatar->h))
                best_avatar = &c;
        }
    }
    if (best_banner) out.banner_path = best_banner->path;
    if (best_avatar) out.avatar_path = best_avatar->path;
    if (out.banner_path.empty() && out.avatar_path.empty()) return std::nullopt;
    return out;
}

std::optional<std::string> fetch_thumbnail(const std::string& url_or_id,
                                           const Config& cfg,
                                           const std::string& dir) {
    const std::string exe = binres::resolve_ytdlp(cfg.ytdlp_path);
    fs::create_directories(dir);
    const std::string out_tpl = (fs::path(dir) / "thumb").string();

    // yt-dlp needs ffmpeg to convert webp thumbnails to jpg.
    const std::string ffmpeg_dir =
        fs::path(binres::resolve_ffmpeg(cfg.ffmpeg_path)).parent_path().string();

    std::vector<std::string> args = {
        "--skip-download",
        "--write-thumbnail",
        "--convert-thumbnails", "jpg",
        "--no-warnings",
        "--no-playlist",
        "-o", out_tpl,
    };
    if (!ffmpeg_dir.empty()) {
        args.push_back("--ffmpeg-location");
        args.push_back(ffmpeg_dir);
    }
    if (cfg.cookies_from_browser) {
        args.push_back("--cookies-from-browser");
        args.push_back(*cfg.cookies_from_browser);
    }
    args.push_back(url_or_id);

    proc::RunOptions opts;
    opts.capture_stdout = true;
    opts.capture_stderr = true;
    opts.timeout_ms = 60000;

    spdlog::debug("running yt-dlp thumbnail fetch: {}", exe);
    proc::RunResult r = proc::run(exe, args, opts);
    if (r.exit_code != 0)
        spdlog::debug("thumbnail fetch exit {}: {}", r.exit_code, r.stderr_text);

    for (const char* ext : {".jpg", ".png", ".webp"}) {
        fs::path p = fs::path(dir) / (std::string("thumb") + ext);
        if (fs::exists(p)) return p.string();
    }
    return std::nullopt;
}

} // namespace crux::fetch
