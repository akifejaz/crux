// Manifest JSON contract — the dashboard reads this shape, so the fields
// (names + types) matter more than most internal APIs. These tests protect
// against silent field renames or type flips.

#include <doctest/doctest.h>

#include "out/manifest.h"
#include "core/heatmap.h"
#include "config.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

fs::path tempdir_for(const std::string& name) {
    auto p = fs::temp_directory_path() / ("crux_manifest_" + name);
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

json read_json(const fs::path& p) {
    std::ifstream f(p);
    std::ostringstream oss; oss << f.rdbuf();
    return json::parse(oss.str());
}

crux::VideoMeta sample_meta() {
    crux::VideoMeta v;
    v.id = "iIY9fPgY5wM";
    v.title = "WE WENT TO THE WORLD CUP!";
    v.channel = "MoreSidemen";
    v.url = "https://www.youtube.com/watch?v=iIY9fPgY5wM";
    v.duration_sec = 1201.0;
    return v;
}

crux::Clip sample_clip(int index, double start, double end, int peak_bin, double peak_score) {
    crux::Clip c;
    c.index = index;
    c.start_sec = start;
    c.end_sec   = end;
    c.peak_bin  = peak_bin;
    c.peak_score = peak_score;
    c.centroid_sec = (start + end) / 2.0;
    c.file = "clip_" + std::to_string(index) + ".mp4";
    return c;
}

} // namespace

TEST_CASE("manifest: schema shape (top-level keys are stable)") {
    auto dir = tempdir_for("schema");
    crux::Config cfg;
    cfg.max_clips = 3;
    cfg.format = crux::Format::Blur916;
    crux::Quality q; q.max_over_median = 8.6; q.flat = false;
    std::vector<crux::Clip> clips = {sample_clip(1, 715.0, 755.0, 59, 1.00)};
    crux::out::write_manifest(dir.string(), sample_meta(), cfg, true, q, clips);

    auto j = read_json(dir / "manifest.json");
    REQUIRE(j.contains("video"));
    REQUIRE(j.contains("params"));
    REQUIRE(j.contains("heatmapPresent"));
    REQUIRE(j.contains("quality"));
    REQUIRE(j.contains("clips"));

    // video.*
    CHECK(j["video"]["id"]       == "iIY9fPgY5wM");
    CHECK(j["video"]["title"]    == "WE WENT TO THE WORLD CUP!");
    CHECK(j["video"]["channel"]  == "MoreSidemen");
    CHECK(j["video"]["duration"] == 1201.0);
    CHECK(j["video"]["url"].is_string());

    // params.*
    CHECK(j["params"]["max_clips"] == 3);
    CHECK(j["params"]["format"]    == "916blur");
    CHECK(j["params"]["source"]    == "ytdlp");

    // quality.*
    CHECK(j["quality"]["maxOverMedian"] == doctest::Approx(8.6));
    CHECK(j["quality"]["flat"]           == false);

    // clips[*].*
    REQUIRE(j["clips"].is_array());
    REQUIRE(j["clips"].size() == 1);
    auto& c = j["clips"][0];
    CHECK(c["index"]     == 1);
    CHECK(c["start"]     == 715.0);
    CHECK(c["end"]       == 755.0);
    CHECK(c["duration"]  == 40.0);
    CHECK(c["peakBin"]   == 59);
    CHECK(c["peakScore"] == doctest::Approx(1.00));
    CHECK(c["file"]      == "clip_1.mp4");
    REQUIRE(c["snapped"].is_object());
    CHECK(c["snapped"].contains("start"));
    CHECK(c["snapped"].contains("end"));

    fs::remove_all(dir);
}

TEST_CASE("manifest: format enum → string mapping") {
    auto dir = tempdir_for("fmt");
    crux::VideoMeta v = sample_meta();
    crux::Quality q;
    struct Row { crux::Format f; const char* expected; };
    Row rows[] = {
        {crux::Format::Blur916, "916blur"},
        {crux::Format::Crop916, "916crop"},
        {crux::Format::Orig,    "orig"},
    };
    for (auto& row : rows) {
        crux::Config cfg; cfg.format = row.f;
        crux::out::write_manifest(dir.string(), v, cfg, true, q, {});
        auto j = read_json(dir / "manifest.json");
        CHECK(j["params"]["format"] == row.expected);
    }
    fs::remove_all(dir);
}

TEST_CASE("manifest: absent heatmap flag") {
    auto dir = tempdir_for("no_heat");
    crux::Config cfg;
    crux::Quality q;
    crux::out::write_manifest(dir.string(), sample_meta(), cfg, false, q, {});
    auto j = read_json(dir / "manifest.json");
    CHECK(j["heatmapPresent"] == false);
    CHECK(j["clips"].is_array());
    CHECK(j["clips"].empty());
    fs::remove_all(dir);
}

TEST_CASE("manifest: heatmap.json is exactly 100 bins + sparkline is 100 chars ASCII") {
    auto dir = tempdir_for("spark");
    crux::Heatmap h;
    h.bin_seconds = 12.01;
    for (std::size_t i = 0; i < crux::kBinCount; ++i) {
        h.bins[i].start_sec = i * h.bin_seconds;
        h.bins[i].end_sec   = (i + 1) * h.bin_seconds;
        // triangular: 0 → 1 → 0
        double frac = 1.0 - std::abs(0.5 - i / 100.0) * 2.0;
        h.bins[i].value = frac;
    }
    std::string spark = crux::out::write_heatmap_json_and_sparkline(dir.string(), h);

    // sparkline: 100 characters, all ASCII (0x20..0x7E)
    CHECK(spark.size() == 100);
    for (char c : spark) {
        CHECK(static_cast<unsigned char>(c) >= 0x20);
        CHECK(static_cast<unsigned char>(c) <= 0x7E);
    }
    // peak should be near the middle
    auto max_pos = static_cast<std::size_t>(
        std::distance(spark.begin(),
                      std::max_element(spark.begin(), spark.end())));
    CHECK(max_pos > 40);
    CHECK(max_pos < 60);

    // heatmap.json
    auto j = read_json(dir / "heatmap.json");
    CHECK(j["bins"].is_array());
    CHECK(j["bins"].size() == 100);
    CHECK(j["binSeconds"] == doctest::Approx(12.01));
    fs::remove_all(dir);
}
