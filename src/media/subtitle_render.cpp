#include "media/subtitle_render.h"

#include "media/transliterate.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace crux::media {

namespace {

// h:mm:ss.cc — ASS uses centiseconds and a single-digit hour field.
std::string ass_timestamp(double t) {
    if (t < 0.0) t = 0.0;
    int total_cs = static_cast<int>(t * 100.0 + 0.5);
    int h = total_cs / 360000;
    int rem = total_cs % 360000;
    int m = rem / 6000;
    rem = rem % 6000;
    int s = rem / 100;
    int cs = rem % 100;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d:%02d:%02d.%02d", h, m, s, cs);
    return buf;
}

// Escapes cue text so ASS override syntax and line breaks don't leak through.
// `{...}` is override; `\N` is a hard newline. We collapse embedded newlines
// to spaces so a two-line VTT cue renders as one wrap-friendly string.
std::string escape_ass_text(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char ch : in) {
        switch (ch) {
        case '{': out += "\\{"; break;
        case '}': out += "\\}"; break;
        case '\\': out += "\\\\"; break;
        case '\r': break;
        case '\n': out += ' '; break;
        default: out += ch;
        }
    }
    // Trim leading/trailing spaces.
    auto not_space = [](unsigned char c) { return c != ' '; };
    auto b = std::find_if(out.begin(), out.end(), not_space);
    auto e = std::find_if(out.rbegin(), out.rend(), not_space).base();
    return (b < e) ? std::string(b, e) : std::string();
}

} // namespace

std::vector<std::string> tokenize_words(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&] {
        if (!cur.empty()) { out.push_back(cur); cur.clear(); }
    };
    for (char c : text) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') flush();
        else cur.push_back(c);
    }
    flush();
    return out;
}

std::vector<SubtitleSegment> split_and_wrap(const std::string& text,
                                            double start_sec,
                                            double end_sec,
                                            int max_words_per_cue,
                                            int words_per_line) {
    std::vector<SubtitleSegment> out;
    auto words = tokenize_words(text);
    if (words.empty() || end_sec <= start_sec) return out;

    const int max_wpc = std::max(1, max_words_per_cue);
    const int wpl     = std::max(1, words_per_line);
    const int total   = static_cast<int>(words.size());
    // Number of sub-cues — one if the cue already fits; otherwise the
    // smallest count that puts every sub-cue at ≤ max_wpc words.
    const int subs    = (total + max_wpc - 1) / max_wpc;
    const int base    = total / subs;
    const int extra   = total % subs;

    const double duration = end_sec - start_sec;
    int word_i = 0;
    for (int i = 0; i < subs; ++i) {
        // Distribute the extra words to the first `extra` sub-cues so the
        // per-cue word count is as balanced as possible.
        const int take = base + (i < extra ? 1 : 0);

        std::string line;
        if (take <= wpl) {
            for (int k = 0; k < take; ++k) {
                if (k > 0) line += ' ';
                line += words[word_i + k];
            }
        } else {
            // Split into two balanced lines: ceil(take/2) on top, the rest
            // below. Handles 5→3+2, 6→3+3, 7→4+3, 8→4+4.
            const int top = (take + 1) / 2;
            for (int k = 0; k < top; ++k) {
                if (k > 0) line += ' ';
                line += words[word_i + k];
            }
            line += "\\N";
            for (int k = top; k < take; ++k) {
                if (k > top) line += ' ';
                line += words[word_i + k];
            }
        }

        SubtitleSegment seg;
        seg.text = std::move(line);
        seg.start_sec = start_sec + duration * i / subs;
        seg.end_sec   = start_sec + duration * (i + 1) / subs;
        out.push_back(std::move(seg));
        word_i += take;
    }
    return out;
}

bool write_clip_ass(const CaptionDoc& doc,
                    double clip_start_sec,
                    double clip_end_sec,
                    double time_offset_sec,
                    const SubtitleStyle& style,
                    const std::string& out_path,
                    bool romanize,
                    const std::string& lang_hint) {
    if (clip_end_sec <= clip_start_sec) return false;

    std::ostringstream body;
    int emitted = 0;
    for (const auto& cue : doc.cues) {
        // Keep any cue that overlaps the clip window at all.
        if (cue.end_sec <= clip_start_sec) continue;
        if (cue.start_sec >= clip_end_sec) continue;
        std::string source_text = romanize
            ? to_latin(cue.text, lang_hint)
            : cue.text;
        std::string text = escape_ass_text(source_text);
        if (text.empty()) continue;

        // Clamp to clip window, then shift so clip start → 0, then delay by
        // the intro-card offset so subtitles appear when the clip content
        // becomes visible (see cutter.cpp intro storyboard).
        double s = std::max(cue.start_sec, clip_start_sec) - clip_start_sec + time_offset_sec;
        double e = std::min(cue.end_sec,   clip_end_sec)   - clip_start_sec + time_offset_sec;
        if (e <= s) continue;

        // Break long cues into sub-cues capped at max_words_per_cue; any
        // sub-cue over words_per_line gets a balanced `\N`. Timing is
        // divided proportionally across the sub-cues.
        auto segments = split_and_wrap(text, s, e,
                                       style.max_words_per_cue,
                                       style.words_per_line);
        for (const auto& seg : segments) {
            body << "Dialogue: 0,"
                 << ass_timestamp(seg.start_sec) << ","
                 << ass_timestamp(seg.end_sec)
                 << ",Default,,0,0,0,," << seg.text << "\n";
            ++emitted;
        }
    }
    if (emitted == 0) return false;

    std::ofstream out(out_path, std::ios::binary);
    if (!out) return false;

    // UTF-8 BOM helps some libass builds pick the encoding correctly.
    static const unsigned char kBom[] = {0xEF, 0xBB, 0xBF};
    out.write(reinterpret_cast<const char*>(kBom), sizeof(kBom));

    // ASS colour byte order: &HAABBGGRR. White with fully-opaque outline and
    // ~50%-transparent shadow reads on any background.
    out <<
        "[Script Info]\n"
        "ScriptType: v4.00+\n"
        "PlayResX: " << style.play_res_x << "\n"
        "PlayResY: " << style.play_res_y << "\n"
        "ScaledBorderAndShadow: yes\n"
        "WrapStyle: 0\n"
        "YCbCr Matrix: TV.709\n"
        "\n"
        "[V4+ Styles]\n"
        "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, "
        "OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, "
        "ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, "
        "Alignment, MarginL, MarginR, MarginV, Encoding\n"
        "Style: Default,Arial," << style.font_size <<
        ",&H00FFFFFF,&H000000FF,&H00000000,&H80000000,"
        "-1,0,0,0,100,100,0,0,1,2.5,1,2,"
        << style.margin_lr << "," << style.margin_lr << "," << style.margin_v << ",1\n"
        "\n"
        "[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, "
        "Effect, Text\n";
    out << body.str();
    return static_cast<bool>(out);
}

std::string escape_ass_path_for_filter(const std::string& path) {
    // libavfilter's option parser splits on unescaped `:` and treats `\` as
    // an escape. Convert Windows separators to `/` (libass accepts either)
    // and prefix each `:` with `\\` so the drive letter survives parsing.
    std::string out;
    out.reserve(path.size() + 4);
    for (char ch : path) {
        if (ch == '\\') {
            out += '/';
        } else if (ch == ':') {
            out += "\\:";
        } else if (ch == '\'') {
            // A stray single quote would close our quoted value.
            out += "\\'";
        } else {
            out += ch;
        }
    }
    return out;
}

} // namespace crux::media
