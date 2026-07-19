#include "core/caption_parse.h"

#include <cctype>
#include <sstream>

namespace crux::captions {

namespace {

// "hh:mm:ss.mmm" (hours may be 1+ digits) → seconds, or -1 on mismatch.
double parse_timestamp(const std::string& s) {
    int h = 0, m = 0, sec = 0, ms = 0;
    if (std::sscanf(s.c_str(), "%d:%d:%d.%d", &h, &m, &sec, &ms) == 4)
        return h * 3600.0 + m * 60.0 + sec + ms / 1000.0;
    if (std::sscanf(s.c_str(), "%d:%d.%d", &m, &sec, &ms) == 3)   // mm:ss.mmm
        return m * 60.0 + sec + ms / 1000.0;
    return -1.0;
}

// Strips <...> inline tags and collapses whitespace.
std::string strip_tags(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    bool in_tag = false, last_space = true;
    for (char ch : in) {
        if (ch == '<') { in_tag = true; continue; }
        if (ch == '>') { in_tag = false; continue; }
        if (in_tag) continue;
        if (ch == ' ' || ch == '\t' || ch == '\r') {
            if (!last_space) { out.push_back(' '); last_space = true; }
        } else {
            out.push_back(ch);
            last_space = false;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

bool is_metadata_line(const std::string& l) {
    return l.rfind("WEBVTT", 0) == 0 || l.rfind("Kind:", 0) == 0 ||
           l.rfind("Language:", 0) == 0 || l.rfind("NOTE", 0) == 0 ||
           l.rfind("STYLE", 0) == 0 || l.rfind("Style:", 0) == 0 ||
           l.rfind("Region:", 0) == 0;
}

} // namespace

CaptionDoc parse_vtt(const std::string& vtt_text) {
    CaptionDoc doc;
    std::istringstream in(vtt_text);
    std::string line;
    double cur_start = -1.0, cur_end = -1.0;
    std::string last_emitted;   // rolling-repeat dedupe across cues

    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();

        auto arrow = line.find("-->");
        if (arrow != std::string::npos) {
            std::string a = line.substr(0, arrow);
            std::string b = line.substr(arrow + 3);
            // trim
            while (!a.empty() && a.back() == ' ') a.pop_back();
            while (!b.empty() && b.front() == ' ') b.erase(b.begin());
            // drop cue settings after the end timestamp ("align:start …")
            if (auto sp = b.find(' '); sp != std::string::npos) b.resize(sp);
            cur_start = parse_timestamp(a);
            cur_end = parse_timestamp(b);
            continue;
        }
        if (line.empty() || is_metadata_line(line)) continue;
        if (cur_start < 0.0) continue;   // cue-number line or stray text

        std::string text = strip_tags(line);
        if (text.empty() || text == last_emitted) continue;
        last_emitted = text;

        doc.cues.push_back({cur_start, cur_end, text});
    }
    return doc;
}

} // namespace crux::captions
