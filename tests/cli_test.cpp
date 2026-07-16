// CLI argument parsing — regression tests for src/cli.cpp.
//
// Uses a table-driven style: each row is one invocation + expected Config.
// The trick with CLI11 is that some options (clip_len, min_gap) are stored
// as std::optional and are only set when the user passes the flag; we assert
// on that specifically to catch the "always set to 0" foot-gun.

#include <doctest/doctest.h>

#include "cli.h"

#include <string>
#include <vector>

namespace {

// argv fixture — CLI11 modifies argv in place, so we hand it mutable buffers.
struct Argv {
    std::vector<std::string> owned;
    std::vector<char*> ptrs;

    Argv(std::initializer_list<const char*> args) {
        owned.reserve(args.size());
        for (auto a : args) owned.emplace_back(a);
        for (auto& s : owned) ptrs.push_back(s.data());
    }
    int argc() const { return static_cast<int>(ptrs.size()); }
    char** argv() { return ptrs.data(); }
};

crux::CliResult run_cli(std::initializer_list<const char*> args) {
    Argv a(args);
    return crux::parse_cli(a.argc(), a.argv());
}

} // namespace

TEST_CASE("cli: URL required — bare call fails") {
    auto r = run_cli({"crux"});
    CHECK(r.should_run == false);
    CHECK(r.exit_code != 0);   // CLI11 non-zero for missing required arg
}

TEST_CASE("cli: URL positional → config.url_or_id") {
    auto r = run_cli({"crux", "iIY9fPgY5wM"});
    REQUIRE(r.should_run);
    CHECK(r.config.url_or_id == "iIY9fPgY5wM");
    // sensible defaults
    CHECK(r.config.max_clips == 6);
    CHECK(r.config.format == crux::Format::Blur916);
    CHECK(r.config.source == crux::SourceKind::Ytdlp);
    CHECK(r.config.dry_run == false);
    CHECK(r.config.strict == false);
    CHECK(!r.config.clip_len.has_value());
    CHECK(!r.config.min_gap.has_value());
    CHECK(!r.config.cookies_from_browser.has_value());
}

TEST_CASE("cli: -n / --max-clips accepts values in 1..10") {
    for (int n : {1, 3, 6, 10}) {
        auto r = run_cli({"crux", "abc", "-n", std::to_string(n).c_str()});
        REQUIRE(r.should_run);
        CHECK(r.config.max_clips == n);
    }
}

TEST_CASE("cli: --max-clips 0 or 11 fails validation") {
    for (const char* v : {"0", "11", "99"}) {
        auto r = run_cli({"crux", "abc", "--max-clips", v});
        CHECK(r.should_run == false);
        CHECK(r.exit_code != 0);
    }
}

TEST_CASE("cli: --format enumeration") {
    struct Row { const char* val; crux::Format expected; };
    Row rows[] = {
        {"916blur", crux::Format::Blur916},
        {"916crop", crux::Format::Crop916},
        {"orig",    crux::Format::Orig},
    };
    for (auto& row : rows) {
        auto r = run_cli({"crux", "abc", "--format", row.val});
        REQUIRE(r.should_run);
        CHECK(r.config.format == row.expected);
    }
}

TEST_CASE("cli: invalid --format is rejected") {
    auto r = run_cli({"crux", "abc", "--format", "vertical"});
    CHECK(r.should_run == false);
}

TEST_CASE("cli: --clip-len sets the optional; absent leaves nullopt") {
    auto r1 = run_cli({"crux", "abc"});
    CHECK(!r1.config.clip_len.has_value());

    auto r2 = run_cli({"crux", "abc", "-l", "60"});
    REQUIRE(r2.should_run);
    REQUIRE(r2.config.clip_len.has_value());
    CHECK(*r2.config.clip_len == doctest::Approx(60.0));
}

TEST_CASE("cli: --clip-len bounds (15..180)") {
    for (const char* v : {"14", "181", "1000"}) {
        auto r = run_cli({"crux", "abc", "--clip-len", v});
        CHECK(r.should_run == false);
    }
    for (const char* v : {"15", "90", "180"}) {
        auto r = run_cli({"crux", "abc", "--clip-len", v});
        REQUIRE(r.should_run);
        CHECK(r.config.clip_len.has_value());
    }
}

TEST_CASE("cli: --cookies-from-browser passthrough") {
    auto r = run_cli({"crux", "abc", "--cookies-from-browser", "chrome"});
    REQUIRE(r.should_run);
    REQUIRE(r.config.cookies_from_browser.has_value());
    CHECK(*r.config.cookies_from_browser == "chrome");
}

TEST_CASE("cli: binary override flags populate optionals") {
    auto r = run_cli({"crux", "abc",
        "--ytdlp",  "C:\\bin\\yt-dlp.exe",
        "--ffmpeg", "C:\\bin\\ffmpeg.exe"});
    REQUIRE(r.should_run);
    REQUIRE(r.config.ytdlp_path.has_value());
    REQUIRE(r.config.ffmpeg_path.has_value());
    CHECK(*r.config.ytdlp_path  == "C:\\bin\\yt-dlp.exe");
    CHECK(*r.config.ffmpeg_path == "C:\\bin\\ffmpeg.exe");
}

TEST_CASE("cli: --source native maps correctly") {
    auto r = run_cli({"crux", "abc", "--source", "native"});
    REQUIRE(r.should_run);
    CHECK(r.config.source == crux::SourceKind::Native);
}

TEST_CASE("cli: boolean flags flip fields") {
    auto r = run_cli({"crux", "abc",
        "--dry-run", "--dump-heatmap", "--keep-intro",
        "--strict", "--full-download", "--json", "-v"});
    REQUIRE(r.should_run);
    CHECK(r.config.dry_run);
    CHECK(r.config.dump_heatmap);
    CHECK(r.config.keep_intro);
    CHECK(r.config.strict);
    CHECK(r.config.full_download);
    CHECK(r.config.json_stdout);
    CHECK(r.config.verbose);
}

TEST_CASE("cli: --version returns should_run=false, exit_code=0") {
    auto r = run_cli({"crux", "--version"});
    CHECK(r.should_run == false);
    CHECK(r.exit_code == 0);
}
