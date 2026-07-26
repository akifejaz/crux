#include <doctest/doctest.h>

#include "core/caption_parse.h"
#include "media/subtitle_render.h"
#include "media/transliterate.h"

#include <cstdio>
#include <fstream>
#include <sstream>

using namespace crux::media;

TEST_CASE("script detection") {
    CHECK(detect_script("hello world") == 0);
    CHECK(detect_script("मैंने देखा") == 1);
    CHECK(detect_script("میں نے دیکھا") == 2);
    // Mixed cue with mostly Latin still surfaces the non-Latin script when
    // that's what needs converting.
    CHECK(detect_script("say नमस्ते now") == 1);
}

TEST_CASE("devanagari transliteration — schwa deletion at word end") {
    // पर → par, दिन → din, राम → ram, नमस्ते → namaste
    CHECK(devanagari_to_latin("पर") == "par");
    CHECK(devanagari_to_latin("दिन") == "din");
    CHECK(devanagari_to_latin("राम") == "ram");
    CHECK(devanagari_to_latin("नमस्ते") == "namaste");
}

TEST_CASE("devanagari transliteration — matras and halant") {
    // डार्क has a halant between र and क, so the 'a' between them drops.
    CHECK(devanagari_to_latin("डार्क") == "dark");
    // करेंगे — matra ে replaces schwa, ं nasalizes, final schwa deletes.
    CHECK(devanagari_to_latin("करेंगे") == "karenge");
    // Devanagari digits fold to Latin.
    CHECK(devanagari_to_latin("३०") == "30");
    // Nukta is dropped; anglicized o-matra ॉ renders as "o" ("log-in").
    CHECK(devanagari_to_latin("लॉगिन") == "login");
    CHECK(devanagari_to_latin("पड़ी") == "pari");
}

TEST_CASE("devanagari transliteration — mixed with English") {
    auto out = devanagari_to_latin("मैंने dark web पर 30 दिन बिताए");
    CHECK(out == "mainne dark web par 30 din bitae");
}

TEST_CASE("loanword dictionary — user example 1 (FBI / crackdown / roast)") {
    // Reported case: "ephabiai ke ek kraikadaun men ephabiai ne ros"
    // should now come out with acronyms uppercase and loanwords in real
    // English while native Hindi ("ने", "एक", "में") stays transliterated.
    auto out = devanagari_to_latin(
        "एफबीआई ने एक क्रैकडाउन में एफबीआई ने रोस्ट");
    CHECK(out == "FBI ne ek crackdown men FBI ne roast");
}

TEST_CASE("loanword dictionary — user example 2 (website / through)") {
    // Reported case: "naujavan isi vebasait ke thru kharidi hui" should
    // become readable Hinglish with the English loanwords restored.
    auto out = devanagari_to_latin(
        "नौजवान इसी वेबसाइट के थ्रू खरीदी हुई");
    CHECK(out == "naujavan isi website ke through kharidi hui");
}

TEST_CASE("loanword dictionary — brands and acronyms keep their case") {
    CHECK(devanagari_to_latin("यूट्यूब पर") == "YouTube par");
    CHECK(devanagari_to_latin("वीपीएन इस्तेमाल करो") == "VPN istemal karo");
    CHECK(devanagari_to_latin("वीडियो में") == "video men");
}

TEST_CASE("write_clip_ass — English lang stays English even with romanize=true") {
    // For an English video the fetched lang is "en" (or "en-orig", "en-US"),
    // and cue text is Latin. The ASS writer must emit the cues without any
    // romanization — but it DOES still line-wrap them.
    const char* vtt =
        "WEBVTT\n\n"
        "00:00:01.000 --> 00:00:03.000\n"
        "The FBI issued a crackdown last month.\n"
        "\n"
        "00:00:03.500 --> 00:00:06.000\n"
        "You can watch the full report on YouTube.\n";
    auto doc = crux::captions::parse_vtt(vtt);
    doc.lang = "en";
    SubtitleStyle style;
    REQUIRE(write_clip_ass(doc, 0.0, 10.0, 0.0, style,
                           "english_untouched.ass",
                           /*romanize=*/true, /*lang_hint=*/"en"));
    std::ifstream in("english_untouched.ass", std::ios::binary);
    std::ostringstream ss; ss << in.rdbuf();
    std::string content = ss.str();
    // Words survive verbatim (no phonetic mangling), and land on the
    // balanced 2-line layout: 7 words → 4 + 3, 8 words → 4 + 4.
    CHECK(content.find("The FBI issued a\\N") != std::string::npos);
    CHECK(content.find("crackdown last month.") != std::string::npos);
    CHECK(content.find("You can watch the\\N") != std::string::npos);
    CHECK(content.find("full report on YouTube.") != std::string::npos);
}

TEST_CASE("tokenize_words — whitespace splitting is UTF-8 safe") {
    CHECK(tokenize_words("").empty());
    CHECK(tokenize_words("   ").empty());
    auto ws = tokenize_words("hello world");
    REQUIRE(ws.size() == 2);
    CHECK(ws[0] == "hello");
    CHECK(ws[1] == "world");
    // Multiple whitespace and mixed types collapse cleanly.
    ws = tokenize_words("a  b\tc\n d");
    REQUIRE(ws.size() == 4);
    CHECK(ws[3] == "d");
    // UTF-8 tokens survive intact.
    ws = tokenize_words("mainne dark web");
    CHECK(ws.size() == 3);
}

TEST_CASE("split_and_wrap — short cue fits on one line") {
    auto segs = split_and_wrap("hello world", 0.0, 2.0, 8, 4);
    REQUIRE(segs.size() == 1);
    CHECK(segs[0].text == "hello world");
    CHECK(segs[0].start_sec == doctest::Approx(0.0));
    CHECK(segs[0].end_sec == doctest::Approx(2.0));
}

TEST_CASE("split_and_wrap — 5–8 words get balanced two lines") {
    auto s5 = split_and_wrap("a b c d e", 0.0, 1.0, 8, 4);
    REQUIRE(s5.size() == 1);
    CHECK(s5[0].text == "a b c\\Nd e");         // 3 + 2

    auto s7 = split_and_wrap("a b c d e f g", 0.0, 1.0, 8, 4);
    REQUIRE(s7.size() == 1);
    CHECK(s7[0].text == "a b c d\\Ne f g");     // 4 + 3

    auto s8 = split_and_wrap("a b c d e f g h", 0.0, 1.0, 8, 4);
    REQUIRE(s8.size() == 1);
    CHECK(s8[0].text == "a b c d\\Ne f g h");   // 4 + 4
}

TEST_CASE("split_and_wrap — long cue splits temporally") {
    // 12 words → 2 sub-cues of 6 each, split at the midpoint of [0, 4].
    auto segs = split_and_wrap(
        "a b c d e f g h i j k l", 0.0, 4.0, 8, 4);
    REQUIRE(segs.size() == 2);
    CHECK(segs[0].text == "a b c\\Nd e f");
    CHECK(segs[0].start_sec == doctest::Approx(0.0));
    CHECK(segs[0].end_sec   == doctest::Approx(2.0));
    CHECK(segs[1].text == "g h i\\Nj k l");
    CHECK(segs[1].start_sec == doctest::Approx(2.0));
    CHECK(segs[1].end_sec   == doctest::Approx(4.0));
}

TEST_CASE("split_and_wrap — odd remainders get distributed to the front") {
    // 9 words, max 8 per cue → 2 sub-cues. Balanced split: 5 + 4.
    auto segs = split_and_wrap(
        "a b c d e f g h i", 0.0, 3.0, 8, 4);
    REQUIRE(segs.size() == 2);
    // 5-word sub-cue wraps 3 + 2.
    CHECK(segs[0].text == "a b c\\Nd e");
    // 4-word sub-cue stays on one line (wpl = 4).
    CHECK(segs[1].text == "f g h i");
}

TEST_CASE("dump — user-example ASS for visual burn-in") {
    // Writes an ASS file to the scratchpad so the outer visual-check script
    // can burn it onto the clean base clip via ffmpeg. Skipped in CI unless
    // the env var is set — it just dumps a file, no assertions of value.
    const char* vtt =
        "WEBVTT\n\n"
        "00:00:01.500 --> 00:00:04.500\n"
        "एफबीआई ने एक क्रैकडाउन में एफबीआई ने रोस्ट\n\n"
        "00:00:05.000 --> 00:00:08.000\n"
        "नौजवान इसी वेबसाइट के थ्रू खरीदी हुई\n\n"
        "00:00:08.500 --> 00:00:11.500\n"
        "यूट्यूब पर वीडियो देखो\n";
    auto doc = crux::captions::parse_vtt(vtt);
    doc.lang = "hi";
    SubtitleStyle style;
    (void)write_clip_ass(doc, 0.0, 12.0, 0.0, style,
                         "hindi_user_example.ass",
                         /*romanize=*/true, /*lang_hint=*/"hi");
    // Emit final Dialogue lines to stdout for eyeballing.
    std::ifstream in("hindi_user_example.ass", std::ios::binary);
    std::ostringstream ss; ss << in.rdbuf();
    std::string content = ss.str();
    std::size_t pos = 0;
    while ((pos = content.find("Dialogue:", pos)) != std::string::npos) {
        std::size_t end = content.find('\n', pos);
        std::printf("  %.*s\n", static_cast<int>(end - pos),
                    content.c_str() + pos);
        pos = end;
    }
}

TEST_CASE("arabic transliteration — consonant heavy roman urdu") {
    // میں نے دیکھا  (I saw)
    auto out = arabic_to_latin("میں نے دیکھا");
    // Short vowels aren't written so we get a consonant-first rendering.
    CHECK(out.find("m") != std::string::npos);
    CHECK(out.find("dikh") != std::string::npos);
    // Eastern Arabic digits fold to Latin.
    CHECK(arabic_to_latin("۳۰") == "30");
}

TEST_CASE("to_latin — English cues pass through untouched") {
    // Every "en*" language tag must be a zero-change passthrough — English
    // videos never get their captions rewritten by the romanizer.
    CHECK(to_latin("Hello there", "en") == "Hello there");
    CHECK(to_latin("Hello there", "en-orig") == "Hello there");
    CHECK(to_latin("Hello there", "en-US") == "Hello there");
    CHECK(to_latin("Hello there", "en-GB") == "Hello there");
    // Punctuation, digits, and multi-line content stay byte-identical.
    CHECK(to_latin("Hello, world! 123.", "en") == "Hello, world! 123.");
    // Even when the English cue mentions loanword-adjacent tokens that
    // look like our dictionary values.
    CHECK(to_latin("The FBI issued a crackdown.", "en") ==
                   "The FBI issued a crackdown.");
    CHECK(to_latin("", "en") == "");
}

TEST_CASE("to_latin — lang hint routes to the right script") {
    CHECK(to_latin("राम", "hi") == "ram");
    // Unknown lang falls back to script detection.
    CHECK(to_latin("राम", "") == "ram");
    // Latin passes even with a non-English hint.
    CHECK(to_latin("plain", "hi") == "plain");
}

TEST_CASE("end-to-end — Hindi VTT → romanized ASS") {
    // Realistic auto-caption VTT chunk from a Hindi vlog.
    const char* vtt =
        "WEBVTT\n"
        "Kind: captions\n"
        "Language: hi\n"
        "\n"
        "00:00:01.500 --> 00:00:04.500\n"
        "मैंने डार्क वेब पर 30 दिन बिताए\n"
        "\n"
        "00:00:05.000 --> 00:00:08.000\n"
        "इस वीडियो में हम बात करेंगे\n"
        "\n"
        "00:00:08.500 --> 00:00:11.500\n"
        "कि साइट लॉगिन हुई पड़ी थी\n";

    auto doc = crux::captions::parse_vtt(vtt);
    doc.lang = "hi";
    REQUIRE(doc.cues.size() == 3);

    // Cache the output next to the test binary so it's easy to eyeball.
    const std::string out = "hindi_roman.ass";
    SubtitleStyle style;
    bool ok = write_clip_ass(doc, 0.0, 12.0, 0.0, style, out,
                             /*romanize=*/true, /*lang_hint=*/"hi");
    REQUIRE(ok);

    std::ifstream in(out, std::ios::binary);
    std::ostringstream ss; ss << in.rdbuf();
    std::string content = ss.str();

    // Original Devanagari must be gone; Roman-Hindi forms must be present.
    // Loanwords come out as real English via the dict pre-pass; native Hindi
    // words fall through to phonetic transliteration.
    CHECK(content.find("मैंने") == std::string::npos);
    CHECK(content.find("mainne") != std::string::npos);
    CHECK(content.find("dark") != std::string::npos);
    CHECK(content.find("web") != std::string::npos);      // वेब → web (dict)
    CHECK(content.find("video") != std::string::npos);    // वीडियो → video
    CHECK(content.find("din") != std::string::npos);
    CHECK(content.find("30") != std::string::npos);
    CHECK(content.find("karenge") != std::string::npos);
    CHECK(content.find("login") != std::string::npos);
    CHECK(content.find("site") != std::string::npos);     // साइट → site (dict)

    // For reviewer convenience — surface the produced Dialogue lines so we
    // can spot-check readability when this test is run verbosely.
    std::size_t pos = 0;
    while ((pos = content.find("Dialogue:", pos)) != std::string::npos) {
        std::size_t end = content.find('\n', pos);
        std::printf("  %.*s\n", static_cast<int>(end - pos),
                    content.c_str() + pos);
        pos = end;
    }
}
