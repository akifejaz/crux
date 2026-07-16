#include "out/manifest.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace crux::out {

namespace {

const char* fmt_string(Format f) {
    switch (f) {
    case Format::Blur916: return "916blur";
    case Format::Crop916: return "916crop";
    case Format::Orig:    return "orig";
    }
    return "?";
}

} // namespace

void write_manifest(const std::string& dir,
                    const VideoMeta& video,
                    const Config& cfg,
                    bool heatmap_present,
                    const Quality& quality,
                    const std::vector<Clip>& clips) {
    fs::create_directories(dir);
    json j;
    j["video"] = {
        {"id", video.id},
        {"title", video.title},
        {"channel", video.channel},
        {"duration", video.duration_sec},
        {"url", video.url},
    };
    j["params"] = {
        {"max_clips", cfg.max_clips},
        {"format",    fmt_string(cfg.format)},
        {"keep_intro", cfg.keep_intro},
        {"strict",    cfg.strict},
        {"clip_len",  cfg.clip_len ? *cfg.clip_len : 0.0},
        {"min_gap",   cfg.min_gap  ? *cfg.min_gap  : 0.0},
        {"source",    cfg.source == SourceKind::Native ? "native" : "ytdlp"},
        {"dry_run",   cfg.dry_run},
        {"full_download", cfg.full_download},
        {"try_sections",  cfg.try_sections},
    };
    j["heatmapPresent"] = heatmap_present;
    j["quality"] = {
        {"maxOverMedian", quality.max_over_median},
        {"flat",           quality.flat},
    };
    json arr = json::array();
    for (const auto& c : clips) {
        arr.push_back({
            {"index",     c.index},
            {"start",     c.start_sec},
            {"end",       c.end_sec},
            {"duration",  c.end_sec - c.start_sec},
            {"peakBin",   c.peak_bin},
            {"peakScore", c.peak_score},
            {"centroid",  c.centroid_sec},
            {"label",     c.label},
            {"file",      c.file},
            {"snapped",   {{"start", c.snapped_start}, {"end", c.snapped_end}}},
        });
    }
    j["clips"] = arr;

    std::ofstream ofs(fs::path(dir) / "manifest.json");
    ofs << j.dump(2) << "\n";
}

std::string write_heatmap_json_and_sparkline(const std::string& dir, const Heatmap& h) {
    fs::create_directories(dir);
    json j;
    j["binSeconds"] = h.bin_seconds;
    json arr = json::array();
    for (const auto& b : h.bins)
        arr.push_back({{"start", b.start_sec}, {"end", b.end_sec}, {"value", b.value}});
    j["bins"] = arr;
    std::ofstream ofs(fs::path(dir) / "heatmap.json");
    ofs << j.dump(2) << "\n";

    // ASCII sparkline (safe across cmd.exe / PowerShell fonts).
    // 10 density steps: space + 9 chars. Space + '.' for the lowest values so
    // baseline noise doesn't dominate the visual.
    static const char kSteps[] = " .,-=+*#%@";
    const int max_idx = static_cast<int>(sizeof(kSteps) - 2);   // 9
    std::string spark;
    spark.reserve(kBinCount);
    for (const auto& b : h.bins) {
        double v = std::max(0.0, std::min(1.0, b.value));
        int idx = static_cast<int>(std::floor(v * max_idx + 0.5));
        if (idx > max_idx) idx = max_idx;
        spark.push_back(kSteps[idx]);
    }
    return spark;
}

} // namespace crux::out
