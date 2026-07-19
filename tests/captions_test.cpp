// Unit tests for the caption crux scorer (core/caption_*).
#include <doctest/doctest.h>

#include "core/caption_parse.h"
#include "core/caption_lexicon.h"
#include "core/caption_scorer.h"

#include <string>

using namespace crux;
using namespace crux::captions;

namespace {

// Builds a synthetic doc: one cue every 4 s with filler text, overridable.
CaptionDoc make_doc(double duration_sec) {
    CaptionDoc d;
    d.lang = "hi";
    for (double t = 0.0; t + 4.0 <= duration_sec; t += 4.0)
        d.cues.push_back({t, t + 3.5, "filler baat chal rahi hai theek hai"});
    return d;
}

void set_cue(CaptionDoc& d, double at_sec, const std::string& text) {
    for (auto& c : d.cues)
        if (c.start_sec <= at_sec && at_sec < c.start_sec + 4.0) { c.text = text; return; }
    REQUIRE(false);
}

} // namespace

TEST_SUITE("caption_parse") {

TEST_CASE("parses cues, strips tags, dedupes rolling repeats") {
    std::string vtt =
        "WEBVTT\n"
        "Kind: captions\n"
        "Language: hi\n"
        "\n"
        "00:00:01.000 --> 00:00:03.000 align:start position:0%\n"
        "hello <c>world</c>\n"
        "\n"
        "00:00:03.000 --> 00:00:05.000\n"
        "hello world\n"          // rolling repeat — must be dropped
        "next line<00:00:04.000> here\n"
        "\n"
        "1:00:03.000 --> 1:00:05.500\n"
        "late cue\n";
    CaptionDoc d = parse_vtt(vtt);
    REQUIRE(d.cues.size() == 3);
    CHECK(d.cues[0].text == "hello world");
    CHECK(d.cues[0].start_sec == doctest::Approx(1.0));
    CHECK(d.cues[1].text == "next line here");
    CHECK(d.cues[2].start_sec == doctest::Approx(3603.0));
    CHECK(d.cues[2].end_sec == doctest::Approx(3605.5));
}

TEST_CASE("empty and header-only input") {
    CHECK(parse_vtt("").cues.empty());
    CHECK(parse_vtt("WEBVTT\n\n").cues.empty());
}

} // TEST_SUITE

TEST_SUITE("caption_lexicon") {

TEST_CASE("lexicon has all roles and sane weights") {
    const auto& lex = default_lexicon();
    CHECK(lex.size() > 100);
    bool pre = false, payload = false, trailing = false, neg = false;
    for (const auto& m : lex) {
        CHECK(m.weight >= 1);
        CHECK(m.weight <= 3);
        switch (m.role) {
        case MarkerRole::Pre: pre = true; break;
        case MarkerRole::Payload: payload = true; break;
        case MarkerRole::Trailing: trailing = true; break;
        case MarkerRole::Negative: neg = true; break;
        }
    }
    CHECK(pre); CHECK(payload); CHECK(trailing); CHECK(neg);
}

} // TEST_SUITE

TEST_SUITE("caption_scorer") {

TEST_CASE("four-beat arc scores highest and sets structure flag") {
    CaptionDoc d = make_doc(600.0);
    // Beat 1+2 (pre), beat 3 (numeric payload), beat 4 (trailing) at ~300 s.
    set_cue(d, 300.0, "main aapko batata hoon ek baat");
    set_cue(d, 312.0, "unhone 50 lakh rupees kamaye aur 2 crore gawaye");
    set_cue(d, 336.0, "wow this is wild");
    CaptionScore s = score_captions(d, 600.0);
    REQUIRE(s.usable);
    REQUIRE(!s.candidates.empty());
    const auto& top = s.candidates.front();
    CHECK(top.start_sec == doctest::Approx(300.0));
    CHECK(top.four_beat);
    CHECK(top.score > 5.0);
    // bins around 300-340 s must be hot (bin 50-56 of 100 over 600 s)
    CHECK(s.bins[51] > 0.5);
    CHECK(s.bins[5] == doctest::Approx(0.0));
}

TEST_CASE("Devanagari marker and Devanagari digits match") {
    CaptionDoc d = make_doc(600.0);
    set_cue(d, 300.0, "मैं आपको बताता हूं एक बात");
    set_cue(d, 312.0, "उन्होंने ५० लाख कमाए");
    CaptionScore s = score_captions(d, 600.0);
    REQUIRE(!s.candidates.empty());
    CHECK(s.candidates.front().start_sec == doctest::Approx(300.0));
}

TEST_CASE("promo marker suppresses the window") {
    CaptionDoc d = make_doc(600.0);
    set_cue(d, 300.0, "use discount code crux for 50 lakh percent off sponsor");
    set_cue(d, 308.0, "50 lakh 20 crore 90 percent");   // numeric-dense promo
    CaptionScore s = score_captions(d, 600.0);
    for (const auto& c : s.candidates) {
        // No candidate may cover the promo block.
        CHECK(!(c.start_sec < 310.0 && c.end_sec > 300.0));
    }
}

TEST_CASE("cold-open replay match flags the full-context body segment") {
    CaptionDoc d = make_doc(1200.0);
    const std::string teaser = "us din jo hua wo koi soch nahi sakta tha bilkul";
    set_cue(d, 20.0, teaser);            // teaser montage line
    set_cue(d, 800.0, teaser);           // replayed at full context in body
    CaptionScore s = score_captions(d, 1200.0);
    bool found = false;
    for (const auto& c : s.candidates)
        if (c.cold_open_match && c.start_sec <= 800.0 && c.end_sec >= 800.0)
            found = true;
    CHECK(found);
}

TEST_CASE("too few cues → unusable") {
    CaptionDoc d;
    d.cues.push_back({0.0, 2.0, "hi"});
    CHECK(!score_captions(d, 60.0).usable);
}

TEST_CASE("snap_to_cues aligns to sentence boundaries within slack") {
    CaptionDoc d = make_doc(600.0);
    double start = 301.5, end = 341.0;   // cue grid is 4 s
    snap_to_cues(d, 3.0, start, end);
    CHECK(start == doctest::Approx(300.0));
    CHECK(end >= 341.0);                 // extended to containing cue end
}

} // TEST_SUITE
