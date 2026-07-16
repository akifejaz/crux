// Silence-snap (M4). Stubbed with a real implementation so it can be enabled
// via config in M4 without further plumbing.
#include "media/silence.h"

#include "binres.h"
#include "media/proc.h"

#include <cmath>
#include <cstdlib>
#include <sstream>
#include <string>

namespace ytshorts::media {

std::vector<SilenceWindow> detect_silences(const std::string& source_file,
                                           double center_sec,
                                           double window_sec,
                                           const Config& cfg) {
    std::vector<SilenceWindow> out;
    const std::string ffmpeg = binres::resolve_ffmpeg(cfg.ffmpeg_path);
    double s = std::max(0.0, center_sec - window_sec);
    double dur = 2.0 * window_sec;

    std::ostringstream ss_s; ss_s << s;
    std::ostringstream ss_d; ss_d << dur;
    std::vector<std::string> args = {
        "-hide_banner", "-nostats",
        "-ss", ss_s.str(),
        "-t",  ss_d.str(),
        "-i", source_file,
        "-af", "silencedetect=noise=-35dB:d=0.35",
        "-f", "null", "-",
    };
    proc::RunOptions opts;
    opts.merge_streams = true;         // silencedetect writes to stderr
    opts.timeout_ms = 60000;
    proc::RunResult r = proc::run(ffmpeg, args, opts);
    // Parse "silence_start: N" / "silence_end: N | silence_duration: N"
    const std::string& text = r.stdout_text;
    SilenceWindow cur{};
    bool have_start = false;
    std::string line;
    for (char ch : text) {
        if (ch == '\n' || ch == '\r') {
            auto pos = line.find("silence_start:");
            if (pos != std::string::npos) {
                cur.start_sec = s + std::atof(line.c_str() + pos + 14);
                have_start = true;
            }
            pos = line.find("silence_end:");
            if (pos != std::string::npos && have_start) {
                cur.end_sec = s + std::atof(line.c_str() + pos + 12);
                out.push_back(cur);
                have_start = false;
                cur = {};
            }
            line.clear();
        } else {
            line.push_back(ch);
        }
    }
    return out;
}

double snap_to_silence(double t, const std::vector<SilenceWindow>& silences, double max_shift) {
    double best_t = t;
    double best_d = max_shift + 1.0;
    for (const auto& w : silences) {
        // consider both endpoints as candidate cut boundaries
        for (double cand : {w.start_sec, w.end_sec}) {
            double d = std::fabs(cand - t);
            if (d < best_d) { best_d = d; best_t = cand; }
        }
    }
    if (best_d <= max_shift) return best_t;
    return t;
}

} // namespace ytshorts::media
