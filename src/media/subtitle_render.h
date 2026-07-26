// Writes a per-clip Advanced SubStation Alpha (.ass) subtitle file trimmed
// to a clip window and time-shifted so cue 0 aligns with the clip's t=0.
// libass (via ffmpeg's `subtitles=` filter) renders it into the video.
#pragma once

#include "core/captions.h"

#include <string>

namespace crux::media {

struct SubtitleStyle {
    // ASS font size in the reference coord system (PlayResX/Y below). At
    // PlayResY=1920 this maps roughly 1:1 to output pixels; ~84-96 reads
    // as "shorts-size" subtitles at typical phone-viewing distance.
    int font_size = 88;
    // Layout target — matches the 9:16 output canvas. Position math (below)
    // assumes PlayResY=1920.
    int play_res_x = 1080;
    int play_res_y = 1920;
    // Vertical margin from the bottom of the canvas in ASS units. With a
    // 16:9 source scaled to 1080 wide, the inner video's bottom edge sits
    // at y=(1920+608)/2 ≈ 1264. For an 88-px 2-line block (~210 tall),
    // MarginV=420 puts the block's top just below the video edge — the
    // text sits right where the faded blur bar begins.
    int margin_v = 420;
    // Horizontal padding — kept generous so 4-word lines never touch the
    // canvas edges.
    int margin_lr = 40;
    // Cue-level tuning: cues longer than max_words_per_cue are split
    // temporally into equal-word sub-cues, and any sub-cue with more than
    // words_per_line words gets one balanced line break.
    int max_words_per_cue = 8;
    int words_per_line = 4;
};

// Writes an ASS file to `out_path` covering cues that intersect [clip_start,
// clip_end], time-shifted by `-clip_start + time_offset_sec`. When `romanize`
// is true and `lang_hint` names a non-Latin language (hi, ur, mr, …), cue
// text is transliterated to Latin so shorts stay readable on phones.
// Returns false when no cues survive the trim (caller should skip the
// subtitles filter).
bool write_clip_ass(const CaptionDoc& doc,
                    double clip_start_sec,
                    double clip_end_sec,
                    double time_offset_sec,
                    const SubtitleStyle& style,
                    const std::string& out_path,
                    bool romanize = false,
                    const std::string& lang_hint = "");

// A time-boxed subtitle segment ready to emit as one ASS Dialogue line.
// `text` may contain `\N` where a hard line break is required.
struct SubtitleSegment {
    std::string text;
    double start_sec = 0.0;
    double end_sec = 0.0;
};

// Splits a string on ASCII whitespace (spaces, tabs, newlines). UTF-8 safe
// because ASCII bytes cannot appear inside a multibyte codepoint.
std::vector<std::string> tokenize_words(const std::string& text);

// Divides `text` into sub-cues so no sub-cue exceeds `max_words_per_cue`
// words (temporally split proportionally across [start_sec, end_sec]) and
// inserts a balanced `\N` in any sub-cue with more than `words_per_line`
// words. Short cues pass through as a single segment.
std::vector<SubtitleSegment> split_and_wrap(const std::string& text,
                                            double start_sec,
                                            double end_sec,
                                            int max_words_per_cue,
                                            int words_per_line);

// Escapes a filesystem path so it can be used as the `filename=` value of
// ffmpeg's `subtitles=` filter. Handles Windows drive-letter colons and
// backslashes (libavfilter's k=v parser treats bare `:` as a separator).
std::string escape_ass_path_for_filter(const std::string& path);

} // namespace crux::media
