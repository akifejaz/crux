#include <doctest/doctest.h>

#include "fetch/parse.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace {
std::string read_file(const std::string& path) {
    std::ifstream f(path);
    std::ostringstream oss; oss << f.rdbuf();
    return oss.str();
}
}

TEST_CASE("parse_ytdlp_json: full sample with heatmap + chapters") {
    auto path = fs::path(CRUX_FIXTURES_DIR) / "sample_ytdlp.json";
    auto text = read_file(path.string());
    REQUIRE(!text.empty());

    auto r = crux::fetch::parse_ytdlp_json(text);
    CHECK(r.video.id == "iIY9fPgY5wM");
    CHECK(r.video.title.find("WORLD CUP") != std::string::npos);
    CHECK(r.video.duration_sec == doctest::Approx(1201.0));
    REQUIRE(r.heatmap.has_value());
    CHECK(r.heatmap->bin_seconds == doctest::Approx(12.01).epsilon(0.01));
    // bin 59 should be the max (1.00)
    double peak = 0; std::size_t peak_idx = 0;
    for (std::size_t i = 0; i < r.heatmap->bins.size(); ++i) {
        if (r.heatmap->bins[i].value > peak) { peak = r.heatmap->bins[i].value; peak_idx = i; }
    }
    CHECK(peak_idx == 59);
    CHECK(peak == doctest::Approx(1.00));
    CHECK(r.chapters.size() == 3);
    CHECK(r.chapters[0].title == "Intro");
}

TEST_CASE("parse_ytdlp_json: absent heatmap") {
    auto path = fs::path(CRUX_FIXTURES_DIR) / "no_heatmap.json";
    auto text = read_file(path.string());
    auto r = crux::fetch::parse_ytdlp_json(text);
    CHECK(r.video.id == "ycPr5-27vSI");
    CHECK(!r.heatmap.has_value());
    CHECK(r.chapters.empty());
}

TEST_CASE("parse_ytdlp_json: bad JSON throws") {
    CHECK_THROWS_AS(crux::fetch::parse_ytdlp_json("{ not json"),
                    std::exception);
}
