#include <doctest/doctest.h>

#include "core/detector.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

struct Fixture {
    std::string id;
    double len_sec;
    double bin_ms;
    std::vector<double> scores;  // already normalized to 0..1
};

std::vector<Fixture> load_fixtures() {
    std::vector<Fixture> out;
    auto path = fs::path(YTSHORTS_FIXTURES_DIR) / "heatmap_scores.json";
    std::ifstream f(path);
    std::ostringstream oss; oss << f.rdbuf();
    auto j = json::parse(oss.str());
    for (const auto& fx : j["fixtures"]) {
        Fixture f;
        f.id = fx["id"].get<std::string>();
        f.len_sec = fx["lenSec"].get<double>();
        f.bin_ms  = fx["binMs"].get<double>();
        double scale = fx["scale"].get<double>();
        for (const auto& s : fx["scores"]) f.scores.push_back(s.get<double>() / scale);
        out.push_back(std::move(f));
    }
    return out;
}

ytshorts::Heatmap to_heatmap(const Fixture& f) {
    ytshorts::Heatmap h;
    double bin_s = f.bin_ms / 1000.0;
    h.bin_seconds = bin_s;
    for (std::size_t i = 0; i < ytshorts::kBinCount; ++i) {
        h.bins[i].start_sec = i * bin_s;
        h.bins[i].end_sec   = (i + 1) * bin_s;
        h.bins[i].value     = f.scores[i];
    }
    return h;
}

ytshorts::Config default_cfg() {
    ytshorts::Config c; c.max_clips = 6; return c;
}

const Fixture* find_fixture(const std::vector<Fixture>& v, const std::string& id) {
    for (const auto& f : v) if (f.id == id) return &f;
    return nullptr;
}

} // namespace

TEST_CASE("detector: bin 0 excluded from candidacy by default") {
    ytshorts::Heatmap h;
    h.bin_seconds = 12.0;
    for (std::size_t i = 0; i < ytshorts::kBinCount; ++i) {
        h.bins[i].start_sec = i * 12.0;
        h.bins[i].end_sec   = (i + 1) * 12.0;
        h.bins[i].value     = (i == 0 ? 1.0 : 0.10);   // bin0 = max, all others flat low
    }
    auto cfg = default_cfg();
    auto d = ytshorts::detector::detect(h, 1200.0, cfg);
    // No region should include bin 0 as its peak
    for (const auto& r : d.regions) {
        CHECK(r.peak_bin != 0);
    }
}

TEST_CASE("detector: end spike (bin 99) is kept") {
    ytshorts::Heatmap h;
    h.bin_seconds = 25.0;
    for (std::size_t i = 0; i < ytshorts::kBinCount; ++i) {
        h.bins[i].start_sec = i * 25.0;
        h.bins[i].end_sec   = (i + 1) * 25.0;
        h.bins[i].value     = 0.1;
    }
    h.bins[99].value = 1.0;
    h.bins[98].value = 0.8;
    h.bins[97].value = 0.6;
    auto cfg = default_cfg();
    auto d = ytshorts::detector::detect(h, 2500.0, cfg);
    bool found = false;
    for (const auto& r : d.regions) if (r.end_bin == 99) found = true;
    CHECK(found);
}

TEST_CASE("detector: MrBeast Squid Game fixture yields several regions") {
    auto fixtures = load_fixtures();
    auto* fx = find_fixture(fixtures, "0e3GPea1Tyg");
    REQUIRE(fx != nullptr);
    auto h = to_heatmap(*fx);
    auto cfg = default_cfg();
    auto d = ytshorts::detector::detect(h, fx->len_sec, cfg);
    // Expected notable peaks: bin 49 (.47), bin 23 (.41), bin 35 (.34)
    bool has49 = false, has23 = false;
    for (const auto& r : d.regions) {
        if (r.start_bin <= 49 && r.end_bin >= 49) has49 = true;
        if (r.start_bin <= 23 && r.end_bin >= 23) has23 = true;
    }
    CHECK(has49);
    CHECK(has23);
    CHECK(!d.quality.flat);
}

TEST_CASE("detector: Dream speedrunner ending spike detected (bins 96-99)") {
    auto fixtures = load_fixtures();
    auto* fx = find_fixture(fixtures, "tylNqtyj0gs");
    REQUIRE(fx != nullptr);
    auto h = to_heatmap(*fx);
    auto cfg = default_cfg();
    auto d = ytshorts::detector::detect(h, fx->len_sec, cfg);
    bool has_end = false;
    for (const auto& r : d.regions) {
        if (r.end_bin >= 96) { has_end = true; break; }
    }
    CHECK(has_end);
}

TEST_CASE("detector: Bob Lazar podcast fixture — end peak survives coarse bins") {
    auto fixtures = load_fixtures();
    auto* fx = find_fixture(fixtures, "BEWz4SXfyCQ");
    REQUIRE(fx != nullptr);
    auto h = to_heatmap(*fx);
    auto cfg = default_cfg();
    auto d = ytshorts::detector::detect(h, fx->len_sec, cfg);
    // bin 44 (.444) and bin 99 (.385) are the strongest non-intro peaks.
    bool has44 = false, has99 = false;
    for (const auto& r : d.regions) {
        if (r.start_bin <= 44 && r.end_bin >= 44) has44 = true;
        if (r.end_bin >= 99) has99 = true;
    }
    CHECK(has44);
    CHECK(has99);
}
