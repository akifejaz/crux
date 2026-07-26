#include "media/outro_card.h"

#include "binres.h"
#include "media/proc.h"
#include "media/text_util.h"

#include <spdlog/spdlog.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

namespace crux::media {

namespace {

// Format a follower count the way YouTube does — 6520 → "6.52K", 1_234_567 → "1.23M".
std::string fmt_followers(long long n) {
    auto fmt = [](double v, const char* suf) {
        char b[32]; std::snprintf(b, sizeof(b), "%.2f%s", v, suf);
        return std::string(b);
    };
    if (n >= 1'000'000) return fmt(n / 1'000'000.0, "M");
    if (n >= 1'000)     return fmt(n / 1'000.0,     "K");
    return std::to_string(n);
}

// Escapes a string for drawtext's `text=` argument: colons and backslashes
// carry meaning inside the filter graph, and single-quote isn't allowed in
// the value we quote with. Drop anything below 0x20 too.
std::string drawtext_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char ch : s) {
        unsigned char u = static_cast<unsigned char>(ch);
        if (u < 0x20) continue;
        switch (ch) {
        case '\\': out += "\\\\"; break;
        case ':':  out += "\\:";  break;
        case '\'': out += "\\'";  break;
        case '%':  out += "\\%";  break;
        default:   out += ch;
        }
    }
    return out;
}

// Escapes a filesystem path for embedding in a filter-graph string. The
// drawtext parser treats ':' as a key separator and needs backslash escapes,
// and Windows paths carry drive letters like "C:/…".
std::string fontfile_escape(const std::string& p) {
    std::string out;
    for (char ch : p) {
        if (ch == '\\') out += "/";           // normalize separators
        else if (ch == ':') out += "\\:";      // escape drive-letter colon
        else if (ch == '\'') out += "\\'";
        else out += ch;
    }
    return out;
}

// Picks the first font that exists from a short priority list. Returns "" if
// none — callers degrade the outro card in that case rather than fail the cut.
std::string find_font() {
    std::vector<fs::path> candidates;
#ifdef _WIN32
    const char* windir = std::getenv("WINDIR");
    fs::path fonts = windir ? fs::path(windir) / "Fonts" : fs::path("C:/Windows/Fonts");
    candidates = {
        fonts / "segoeuib.ttf",
        fonts / "segoeui.ttf",
        fonts / "arial.ttf",
    };
#else
    candidates = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
        "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
    };
#endif
    for (const auto& c : candidates) {
        std::error_code ec;
        if (fs::exists(c, ec) && !ec) return c.string();
    }
    return {};
}

std::string find_font_regular() {
    std::vector<fs::path> candidates;
#ifdef _WIN32
    const char* windir = std::getenv("WINDIR");
    fs::path fonts = windir ? fs::path(windir) / "Fonts" : fs::path("C:/Windows/Fonts");
    candidates = { fonts / "segoeui.ttf", fonts / "arial.ttf" };
#else
    candidates = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/System/Library/Fonts/Supplemental/Arial.ttf",
    };
#endif
    for (const auto& c : candidates) {
        std::error_code ec;
        if (fs::exists(c, ec) && !ec) return c.string();
    }
    return {};
}

} // namespace

std::optional<std::string> render_outro_card(
    const VideoMeta& video,
    const fetch::ChannelAssets& assets,
    const Config& cfg,
    const std::string& out_jpg) {
    const std::string ffmpeg = binres::resolve_ffmpeg(cfg.ffmpeg_path);
    const std::string font_bold = find_font();
    const std::string font_reg  = find_font_regular();
    if (font_bold.empty() || font_reg.empty()) {
        spdlog::warn("outro card: no usable font found — skipping");
        return std::nullopt;
    }

    // Text lines. Falls back gracefully when a channel doesn't publish a
    // handle or follower count.
    const std::string channel_name = video.channel.empty() ? "Watch full video"
                                                            : video.channel;
    std::string subline = video.channel_handle;
    if (video.channel_follower_count > 0) {
        if (!subline.empty()) subline += "  \xc2\xb7  ";   // " · "
        subline += fmt_followers(video.channel_follower_count) + " subscribers";
    }
    const std::string cta   = "Watch full video";
    const std::string title = text::utf8_truncate(video.title, 60);

    const bool have_banner = !assets.banner_path.empty();
    const bool have_avatar = !assets.avatar_path.empty();

    // Build the filter graph. Banner and avatar are optional; without them
    // we fall back to a solid backdrop / no avatar overlay.
    std::vector<std::string> args = {"-y", "-loglevel", "error"};
    int idx = 0;
    if (have_banner) args.insert(args.end(),
        {"-loop", "1", "-i", assets.banner_path}), idx++;
    if (have_avatar) args.insert(args.end(),
        {"-loop", "1", "-i", assets.avatar_path}), idx++;
    args.insert(args.end(), {"-f", "lavfi", "-i", "color=c=0x0b1220:s=1080x1920"});

    // Input indices resolved after the fact: if we have banner and avatar,
    // banner=0, avatar=1, backdrop=2. If only banner, banner=0, backdrop=1.
    const int banner_i  = have_banner ? 0 : -1;
    const int avatar_i  = have_avatar ? (have_banner ? 1 : 0) : -1;
    const int backdrop_i = idx;
    (void)backdrop_i;

    std::ostringstream g;
    // Background: blurred banner if we have one, otherwise the solid backdrop.
    if (have_banner) {
        g << "[" << banner_i << ":v]scale=1080:1920:force_original_aspect_ratio=increase,"
             "crop=1080:1920,gblur=sigma=30,eq=brightness=-0.3[bg];";
        // Adaptive banner strip: never crop taller than the actual image.
        g << "[" << banner_i << ":v]scale=1080:-2,crop=w=1080:h='min(600\\,ih)':x=0:y=0[bstrip];";
        g << "[bg][bstrip]overlay=0:80[cvs];";
    } else {
        g << "[" << backdrop_i << ":v]null[cvs];";
    }

    // Circular avatar mask via geq alpha.
    if (have_avatar) {
        g << "[" << avatar_i << ":v]scale=500:500,format=rgba,"
             "geq=r='r(X,Y)':g='g(X,Y)':b='b(X,Y)':"
             "a='if(lte(hypot(X-250\\,Y-250)\\,245)\\,255\\,0)'[avatar];";
        g << "[cvs][avatar]overlay=(W-w)/2:640[c1];";
    } else {
        g << "[cvs]null[c1];";
    }

    // Text stack. `text=` on the command line gets mangled through Windows'
    // ANSI argv layer, so multibyte chars like · and — turn to mojibake.
    // Writing each line to a UTF-8 file and using `textfile=` bypasses that.
    const std::string fb = fontfile_escape(font_bold);
    const std::string fr = fontfile_escape(font_reg);
    fs::path dir = fs::path(out_jpg).parent_path();
    auto write_text = [&](const std::string& name,
                          const std::string& body) -> std::string {
        fs::path p = dir / name;
        std::ofstream f(p, std::ios::binary);
        f.write(body.data(), static_cast<std::streamsize>(body.size()));
        return fontfile_escape(p.string());
    };
    const std::string ft_chan  = write_text("outro_txt_channel.txt", channel_name);
    const std::string ft_sub   = subline.empty() ? std::string{}
                                                 : write_text("outro_txt_sub.txt", subline);
    const std::string ft_cta   = write_text("outro_txt_cta.txt", cta);
    const std::string ft_title = title.empty() ? std::string{}
                                               : write_text("outro_txt_title.txt", title);

    g << "[c1]drawtext=fontfile='" << fb << "':textfile='" << ft_chan
      << "':fontcolor=white:fontsize=72:x=(w-text_w)/2:y=1200";
    if (!ft_sub.empty()) {
        g << ",drawtext=fontfile='" << fr << "':textfile='" << ft_sub
          << "':fontcolor=0xB0B6C0:fontsize=32:x=(w-text_w)/2:y=1295";
    }
    g << ",drawtext=fontfile='" << fb << "':textfile='" << ft_cta
      << "':fontcolor=white:fontsize=68:x=(w-text_w)/2:y=1490";
    if (!ft_title.empty()) {
        g << ",drawtext=fontfile='" << fr << "':textfile='" << ft_title
          << "':fontcolor=0x8A93A1:fontsize=28:x=(w-text_w)/2:y=1780";
    }

    args.insert(args.end(), {"-filter_complex", g.str(),
                             "-frames:v", "1", out_jpg});

    proc::RunOptions opts;
    opts.timeout_ms = 60000;
    opts.capture_stdout = true;
    opts.capture_stderr = true;
    proc::RunResult r = proc::run(ffmpeg, args, opts);
    if (r.exit_code != 0) {
        spdlog::warn("outro card ffmpeg exit {}: {}", r.exit_code, r.stderr_text);
        return std::nullopt;
    }
    std::error_code ec;
    if (!fs::exists(out_jpg, ec) || fs::file_size(out_jpg, ec) < 4096) {
        spdlog::warn("outro card render produced no usable output");
        return std::nullopt;
    }
    return out_jpg;
}

} // namespace crux::media
