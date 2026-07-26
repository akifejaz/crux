#include "media/cutter.h"

#include "binres.h"
#include "media/intro_assets.h"
#include "media/proc.h"
#include "media/subtitle_render.h"

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

namespace {

// Builds the ffmpeg args for one cut. Optional pre-roll (thumbnail +
// play-button + cursor + whoosh) and outro card ("Watch full video here"
// with channel branding) are appended as xfades on the main clip. Inputs
// (variable): source, [thumb, whoosh, tick, play, cursor] if with_intro,
// [outro] if with_outro. 916blur/916crop only — orig skips both.
std::vector<std::string> build_cut_args(const std::string& input,
                                        double s, double e,
                                        Format fmt,
                                        const Config& cfg,
                                        const std::string& out_file,
                                        const std::string& thumb_path,
                                        const std::string& play_png,
                                        const std::string& cursor_png,
                                        const std::string& outro_path,
                                        const std::string& subtitle_file,
                                        bool with_intro,
                                        bool with_outro) {
    // Subtitles are burned onto the inner-video branch (fg only, not the
    // blurred backdrop). Timing is clip-relative — the intro xfade already
    // hides them until the reframed video becomes visible.
    const bool with_subs = !subtitle_file.empty() && fmt != Format::Orig;
    const std::string subs_filter = with_subs
        ? ",subtitles=filename='" +
              crux::media::escape_ass_path_for_filter(subtitle_file) + "'"
        : std::string();
    std::vector<std::string> args = {
        "-y",
        "-loglevel", "warning",
        "-stats",
        "-ss", ftos(s),
        "-to", ftos(e),
        "-i", input,
    };

    const char* filt = filter_for(fmt);
    if ((with_intro || with_outro) && filt) {
        // Layout: [intro?] → clip → [outro?]. Each transition is a 0.4s xfade
        // so nothing hard-cuts.
        const double clip_dur = e - s;
        const double xfade = 0.4;

        // Intro storyboard, proportional to intro_sec T (defaults for T=1.5):
        //   cursor flight 0 → 0.47T (0.7s, ease-out cubic)
        //   click at 0.60T (0.9s): press nudge + tick, play button vanishes
        //   crossfade 0.73T → T (1.1s → 1.5s)
        const double T = cfg.intro_sec;
        const double arrive = 0.47 * T;
        const double press = 0.60 * T;
        const double intro_fade_off = with_intro ? T - xfade : 0.0;

        // Assemble input args in a fixed order so the [N:v] indices line up.
        int input_i = 1;
        int thumb_i = -1, whoosh_i = -1, tick_i = -1, play_i = -1, cursor_i = -1;
        if (with_intro) {
            args.insert(args.end(), {
                "-loop", "1", "-framerate", "30", "-t", ftos(T), "-i", thumb_path,
                "-f", "lavfi", "-t", ftos(T),
                "-i", "anoisesrc=color=pink:r=44100:a=0.6:d=" + ftos(T),
                "-f", "lavfi", "-t", "0.05",
                "-i", "sine=frequency=1500:sample_rate=44100:duration=0.05",
                "-i", play_png,
                "-i", cursor_png,
            });
            thumb_i = input_i++;
            whoosh_i = input_i++;
            tick_i = input_i++;
            play_i = input_i++;
            cursor_i = input_i++;
        }
        int outro_i = -1;
        if (with_outro) {
            args.insert(args.end(), {
                "-loop", "1", "-framerate", "30",
                "-t", ftos(cfg.outro_sec),
                "-i", outro_path,
            });
            outro_i = input_i++;
        }

        std::ostringstream g;
        // Main video branch always runs through the reframe filter.
        g << filt << ";";
        g << "[v]fps=30,setsar=1,format=yuv420p,settb=AVTB" << subs_filter << "[main_v];";

        // The "current" merged video/audio labels — we chain xfades onto them.
        std::string cur_v = "[main_v]";
        std::string cur_a = "[0:a]";

        if (with_intro) {
            const std::string ease =
                "(1-pow(1-min(t/" + ftos(arrive) + ",1),3))";
            const std::string nudge =
                "if(between(t," + ftos(press - 0.03) + "," + ftos(press + 0.06) + "),7,0)";

            // Intro backdrop + play button + cursor overlay.
            g << "[" << thumb_i << ":v]split=2[t1][t2];"
              << "[t1]scale=1080:1920:force_original_aspect_ratio=increase,"
              << "crop=1080:1920,gblur=sigma=24[ibg];"
              << "[t2]scale=1080:-2[ifg];"
              << "[ibg][ifg]overlay=(W-w)/2:(H-h)/2[ib];"
              << "[ib][" << play_i << ":v]overlay=(W-w)/2:(H-h)/2:enable='lte(t," << ftos(press) << ")'[ipb];"
              << "[ipb][" << cursor_i << ":v]overlay="
              << "x='(W+20)+((W/2-6)-(W+20))*" << ease << "':"
              << "y='(H+20)+((H/2-6)-(H+20))*" << ease << "+" << nudge << "'[iov];"
              << "[iov]fps=30,setsar=1,format=yuv420p,settb=AVTB[intro_v];"
              << "[intro_v]" << cur_v << "xfade=transition=fade:duration=" << ftos(xfade)
              << ":offset=" << ftos(intro_fade_off) << "[after_intro_v];";
            cur_v = "[after_intro_v]";

            // Audio: whoosh + click tick + clip audio delayed until the crossfade.
            g << "[" << whoosh_i << ":a]lowpass=f=2600,"
              << "afade=t=in:st=0:d=" << ftos(0.2 * T)
              << ",afade=t=out:st=" << ftos(0.45 * T) << ":d=" << ftos(0.55 * T)
              << ",volume=0.55[wh];"
              << "[" << tick_i << ":a]adelay=" << static_cast<int>((press - 0.05) * 1000.0)
              << "|" << static_cast<int>((press - 0.05) * 1000.0) << ",volume=0.45[tk];"
              << "[0:a]adelay=" << static_cast<int>(intro_fade_off * 1000.0)
              << "|" << static_cast<int>(intro_fade_off * 1000.0)
              << ",afade=t=in:st=" << ftos(intro_fade_off) << ":d=" << ftos(xfade) << "[ca];"
              << "[wh][tk][ca]amix=inputs=3:duration=longest:normalize=0[after_intro_a];";
            cur_a = "[after_intro_a]";
        }

        if (with_outro) {
            // Video merged so far spans [0 … intro_len + clip_dur - xfade].
            const double pre_outro_len = with_intro
                ? (T + clip_dur - xfade)
                : clip_dur;
            const double outro_xfade_off = pre_outro_len - xfade;
            const double total = pre_outro_len + cfg.outro_sec - xfade;

            g << "[" << outro_i << ":v]scale=1080:1920,fps=30,setsar=1,"
              << "format=yuv420p,settb=AVTB[outro_v];"
              << cur_v << "[outro_v]xfade=transition=fade:duration=" << ftos(xfade)
              << ":offset=" << ftos(outro_xfade_off) << "[vo];";
            cur_v = "[vo]";

            // Audio: pad the running audio to the new total length and fade
            // out into silence over the outro xfade window.
            g << cur_a << "apad=whole_dur=" << ftos(total)
              << ",afade=t=out:st=" << ftos(outro_xfade_off)
              << ":d=" << ftos(xfade) << "[ao];";
            cur_a = "[ao]";
        }

        // Emit final labels [vo]/[ao] if intro-only path didn't already.
        if (cur_v != "[vo]") g << cur_v << "null[vo];";
        if (cur_a != "[ao]") g << cur_a << "anull[ao]";
        else {
            // Trim trailing ';' from the last statement so filter_complex parses.
            std::string s = g.str();
            if (!s.empty() && s.back() == ';') s.pop_back();
            g.str(s);
            g.seekp(0, std::ios::end);
        }

        args.insert(args.end(), {
            "-filter_complex", g.str(),
            "-map", "[vo]",
            "-map", "[ao]",
        });
    } else if (filt) {
        std::string graph = filt;
        std::string video_label = "[v]";
        if (with_subs) {
            graph = graph + ";[v]" + subs_filter.substr(1) + "[vsub]";
            video_label = "[vsub]";
        }
        args.insert(args.end(), {
            "-filter_complex", graph,
            // Explicit stream mapping: filtered video + optional audio track.
            "-map", video_label,
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
    return args;
}

} // namespace

void cut_clip(const std::string& source_file,
              const Clip& clip,
              const DownloadResult& dl,
              Format fmt,
              const Config& cfg,
              const std::string& out_file,
              const std::string& thumb_path,
              const std::string& outro_path,
              const std::string& subtitle_file) {
    const std::string ffmpeg = binres::resolve_ffmpeg(cfg.ffmpeg_path);

    // Pick the actual input file. Three cases:
    //   (a) Per-clip parallel mode → one mp4 per clip, indexed by clip.index-1
    //   (b) Legacy multi-section single-file → source_file with dl.sections[]
    //   (c) Full download → source_file, no translation
    std::string input = source_file;
    double s = clip.start_sec;
    double e = clip.end_sec;

    const std::size_t idx = static_cast<std::size_t>(std::max(0, clip.index - 1));
    if (!dl.per_clip_files.empty() &&
        idx < dl.per_clip_files.size() &&
        !dl.per_clip_files[idx].empty()) {
        // Case (a): per-clip file. Its timeline starts at src_start_sec.
        input = dl.per_clip_files[idx];
        if (idx < dl.sections.size()) {
            const auto& sec = dl.sections[idx];
            s = clip.start_sec - sec.src_start_sec + sec.file_start_sec;
            e = clip.end_sec   - sec.src_start_sec + sec.file_start_sec;
        }
    } else if (!dl.sections.empty()) {
        // Case (b): legacy single-file-multi-section.
        for (const auto& sec : dl.sections) {
            if (clip.start_sec >= sec.src_start_sec && clip.end_sec <= sec.src_end_sec) {
                s = clip.start_sec - sec.src_start_sec + sec.file_start_sec;
                e = clip.end_sec   - sec.src_start_sec + sec.file_start_sec;
                break;
            }
        }
    }
    // Case (c): defaults are already correct.

    preflight_input(input);

    bool with_intro = cfg.intro_card && !thumb_path.empty() &&
                      fmt != Format::Orig;
    bool with_outro = cfg.outro_card && !outro_path.empty() &&
                      fmt != Format::Orig;
    {
        std::error_code tec;
        if (with_intro && !fs::exists(thumb_path, tec)) with_intro = false;
        if (with_outro && !fs::exists(outro_path, tec)) with_outro = false;
    }
    std::string play_png, cursor_png;
    if (with_intro) {
        IntroAssets assets = write_intro_assets(
            fs::path(thumb_path).parent_path().string());
        play_png = assets.play_png;
        cursor_png = assets.cursor_png;
    }

    proc::RunOptions opts;
    opts.timeout_ms = 15 * 60 * 1000;
    opts.inherit_stdout = true;
    opts.inherit_stderr = true;
    opts.capture_stdout = false;
    opts.capture_stderr = false;

    std::vector<std::string> args =
        build_cut_args(input, s, e, fmt, cfg, out_file, thumb_path,
                       play_png, cursor_png, outro_path, subtitle_file,
                       with_intro, with_outro);
    proc::RunResult r = proc::run(ffmpeg, args, opts);
    if (r.exit_code != 0 && (with_intro || with_outro)) {
        // The intro/outro graph assumes an audio track, decodable thumbnail,
        // and a valid outro image; degrade to a plain cut rather than failing
        // the clip.
        spdlog::warn("intro/outro graph failed (exit {}) — retrying without them",
                     r.exit_code);
        args = build_cut_args(input, s, e, fmt, cfg, out_file, thumb_path,
                              play_png, cursor_png, outro_path, subtitle_file,
                              false, false);
        r = proc::run(ffmpeg, args, opts);
    }
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
