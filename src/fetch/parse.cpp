#include "fetch/parse.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <stdexcept>

using json = nlohmann::json;

namespace ytshorts::fetch {

namespace {

double as_number(const json& v, double dflt = 0.0) {
    if (v.is_number()) return v.get<double>();
    if (v.is_string()) {
        try { return std::stod(v.get<std::string>()); }
        catch (...) { return dflt; }
    }
    return dflt;
}

std::string as_string(const json& v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_null()) return {};
    return v.dump();
}

} // namespace

FetchResult parse_ytdlp_json(const std::string& text) {
    json j = json::parse(text, /*cb*/ nullptr, /*allow_exceptions*/ true);
    FetchResult r;

    if (!j.contains("id") || j["id"].is_null())
        throw std::runtime_error("yt-dlp JSON: missing 'id'");
    r.video.id = as_string(j["id"]);
    r.video.title   = j.contains("title")       ? as_string(j["title"])       : "";
    r.video.channel = j.contains("channel")     ? as_string(j["channel"])     : "";
    r.video.url     = j.contains("webpage_url") ? as_string(j["webpage_url"]) : "";
    r.video.duration_sec = j.contains("duration") ? as_number(j["duration"]) : 0.0;

    // Detect live/premiere.
    if (j.contains("is_live") && j["is_live"].is_boolean() && j["is_live"].get<bool>())
        r.video.is_live = true;
    if (j.contains("live_status") && j["live_status"].is_string()) {
        auto s = j["live_status"].get<std::string>();
        if (s == "is_live" || s == "is_upcoming" || s == "post_live") r.video.is_live = true;
    }

    // Chapters (may be null / absent).
    if (j.contains("chapters") && j["chapters"].is_array()) {
        for (const auto& c : j["chapters"]) {
            Chapter ch;
            ch.start_sec = as_number(c.value("start_time", json(0.0)));
            ch.end_sec   = as_number(c.value("end_time",   json(0.0)));
            ch.title     = as_string(c.value("title", json("")));
            r.chapters.push_back(std::move(ch));
        }
    }

    // Heatmap. yt-dlp emits: heatmap: [ {start_time, end_time, value}, ... ] (100 entries).
    if (j.contains("heatmap") && j["heatmap"].is_array() && !j["heatmap"].empty()) {
        const auto& arr = j["heatmap"];
        if (arr.size() != kBinCount)
            throw std::runtime_error("yt-dlp heatmap has " + std::to_string(arr.size()) +
                                     " bins, expected 100");
        Heatmap h;
        for (std::size_t i = 0; i < kBinCount; ++i) {
            const auto& b = arr[i];
            h.bins[i].start_sec = as_number(b.value("start_time", json(0.0)));
            h.bins[i].end_sec   = as_number(b.value("end_time",   json(0.0)));
            h.bins[i].value     = as_number(b.value("value",      json(0.0)));
        }
        // Derive bin_seconds from the first span (rounded like durationMillis/100).
        h.bin_seconds = h.bins[0].end_sec - h.bins[0].start_sec;
        if (h.bin_seconds <= 0.0 && r.video.duration_sec > 0.0)
            h.bin_seconds = r.video.duration_sec / static_cast<double>(kBinCount);
        r.heatmap = h;
    }
    return r;
}

} // namespace ytshorts::fetch
