#include "core/caption_scorer.h"
#include "core/caption_lexicon.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace crux::captions {

namespace {

// ---- text normalization ---------------------------------------------------

// Lowercases ASCII and maps Devanagari digits (U+0966-096F, bytes E0 A5 A6-AF)
// to ASCII so one numeric detector covers both scripts. Other UTF-8 bytes pass
// through untouched.
std::string normalize(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(in[i]);
        if (c == 0xE0 && i + 2 < in.size() &&
            static_cast<unsigned char>(in[i + 1]) == 0xA5) {
            unsigned char c3 = static_cast<unsigned char>(in[i + 2]);
            if (c3 >= 0xA6 && c3 <= 0xAF) {          // ०..९
                out.push_back(static_cast<char>('0' + (c3 - 0xA6)));
                i += 2;
                continue;
            }
        }
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

// ---- per-cue analysis -----------------------------------------------------

struct CueHits {
    double pre = 0.0, payload = 0.0, trailing = 0.0;
    int numeric_hits = 0;
    bool negative = false;
    bool coldopen_echo = false;      // this cue replays a cold-open line
    std::vector<const char*> signals;
};

// True when a digit run in `text` has a magnitude token within `radius` bytes.
int count_numeric_payloads(const std::string& text, std::size_t radius = 30) {
    int hits = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(text[i]))) continue;
        std::size_t run_end = i;
        while (run_end < text.size() &&
               std::isdigit(static_cast<unsigned char>(text[run_end]))) ++run_end;
        std::size_t lo = i > radius ? i - radius : 0;
        std::size_t hi = std::min(text.size(), run_end + radius);
        std::string ctx = text.substr(lo, hi - lo);
        for (const char* unit : magnitude_tokens()) {
            if (ctx.find(unit) != std::string::npos) { ++hits; break; }
        }
        i = run_end;
    }
    return hits;
}

// Tokenizes on spaces (bytes; Devanagari words separate fine on spaces).
std::vector<std::string> tokens_of(const std::string& s) {
    std::vector<std::string> t;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && s[i] == ' ') ++i;
        std::size_t j = i;
        while (j < s.size() && s[j] != ' ') ++j;
        if (j > i) t.push_back(s.substr(i, j - i));
        i = j;
    }
    return t;
}

// 5-gram shingles as joined strings (exact match — transliteration noise is
// consistent within one video, so exact repeats do occur for real replays).
void add_shingles(const std::vector<std::string>& toks,
                  std::unordered_set<std::string>& out) {
    if (toks.size() < 5) return;
    for (std::size_t i = 0; i + 5 <= toks.size(); ++i) {
        std::string s;
        for (std::size_t k = 0; k < 5; ++k) {
            if (k) s.push_back(' ');
            s += toks[i + k];
        }
        out.insert(std::move(s));
    }
}

bool has_shingle_match(const std::vector<std::string>& toks,
                       const std::unordered_set<std::string>& shingles) {
    if (toks.size() < 5) return false;
    for (std::size_t i = 0; i + 5 <= toks.size(); ++i) {
        std::string s;
        for (std::size_t k = 0; k < 5; ++k) {
            if (k) s.push_back(' ');
            s += toks[i + k];
        }
        if (shingles.count(s)) return true;
    }
    return false;
}

// Position prior from the research aggregate (never a gate: 0.85x..1.15x).
double position_prior(double center_sec, double duration_sec) {
    if (duration_sec <= 0.0) return 1.0;
    double pct = 100.0 * center_sec / duration_sec;
    if (pct < 10.0) return 1.0;
    if (pct < 30.0) return 0.85;
    if (pct < 60.0) return 1.1;
    if (pct < 90.0) return 1.15;
    return 1.0;
}

} // namespace

CaptionScore score_captions(const CaptionDoc& doc,
                            double duration_sec,
                            const ScoreParams& p) {
    CaptionScore out;
    if (doc.cues.size() < 10 || duration_sec <= 0.0) return out;
    out.usable = true;

    const auto& lex = default_lexicon();
    const std::size_t n = doc.cues.size();

    // Pass 1 — per-cue marker + numeric hits, on normalized text.
    std::vector<CueHits> hits(n);
    std::vector<std::string> norm(n);
    for (std::size_t i = 0; i < n; ++i) {
        norm[i] = normalize(doc.cues[i].text);
        CueHits& h = hits[i];
        for (const auto& m : lex) {
            if (norm[i].find(m.pattern) == std::string::npos) continue;
            switch (m.role) {
            case MarkerRole::Pre:      h.pre      += m.weight; break;
            case MarkerRole::Payload:  h.payload  += m.weight; break;
            case MarkerRole::Trailing: h.trailing += m.weight; break;
            case MarkerRole::Negative: h.negative = true; break;
            }
            h.signals.push_back(m.signal);
        }
        h.numeric_hits = count_numeric_payloads(norm[i]);
    }

    // Pass 2 — cold-open replay match (finding 4). Only meaningful when the
    // video is long enough to have a teaser + body. Shingles that repeat all
    // over the video (catchphrases, filler) are excluded — only rare teaser
    // lines mark an editor-pre-validated crux.
    if (duration_sec > 2.0 * p.coldopen_min_body_sec) {
        std::unordered_map<std::string, int> shingle_count;
        std::vector<std::vector<std::string>> toks(n);
        for (std::size_t i = 0; i < n; ++i) {
            toks[i] = tokens_of(norm[i]);
            std::unordered_set<std::string> s;
            add_shingles(toks[i], s);
            for (const auto& sh : s) ++shingle_count[sh];
        }
        std::unordered_set<std::string> teaser;
        for (std::size_t i = 0; i < n && doc.cues[i].start_sec < p.coldopen_end_sec; ++i) {
            std::unordered_set<std::string> s;
            add_shingles(toks[i], s);
            for (const auto& sh : s)
                if (shingle_count[sh] <= 3) teaser.insert(sh);
        }
        if (!teaser.empty()) {
            for (std::size_t i = 0; i < n; ++i) {
                if (doc.cues[i].start_sec < p.coldopen_min_body_sec) continue;
                if (has_shingle_match(toks[i], teaser))
                    hits[i].coldopen_echo = true;
            }
        }
    }

    // Promo mask spans: a Negative marker poisons its surroundings, not just
    // its own cue — sponsor reads run tens of seconds (research suppressor).
    std::vector<std::pair<double, double>> promo_spans;
    for (std::size_t i = 0; i < n; ++i)
        if (hits[i].negative)
            promo_spans.emplace_back(doc.cues[i].start_sec - 5.0,
                                     doc.cues[i].start_sec + 30.0);

    // Pass 3 — windowed scoring. Every cue start is a candidate window start
    // (natural sentence-ish snapping; auto captions have no punctuation).
    struct Win {
        std::size_t first_cue = 0, last_cue = 0;
        double start = 0.0, end = 0.0, score = 0.0;
        bool four_beat = false, coldopen = false, masked = false;
        std::size_t hook_cue = 0;
        std::set<std::string> signals;
    };
    std::vector<Win> wins;
    wins.reserve(n);

    for (std::size_t i = 0; i < n; ++i) {
        Win w;
        w.first_cue = i;
        w.start = doc.cues[i].start_sec;
        double wend = w.start + p.window_sec;
        if (w.start + 15.0 > duration_sec) break;

        double pre = 0.0, payload = 0.0, trailing = 0.0, coldopen = 0.0;
        double first_pre_t = -1.0, first_payload_t = -1.0, last_trailing_t = -1.0;
        double first_echo_t = -1.0;
        std::vector<double> numeric_times;
        double best_hook_w = 0.0;
        w.hook_cue = i;

        for (const auto& span : promo_spans)
            if (w.start < span.second && wend > span.first) { w.masked = true; break; }

        std::size_t j = i;
        for (; j < n && doc.cues[j].start_sec < wend; ++j) {
            const CueHits& h = hits[j];
            const double t = doc.cues[j].start_sec;
            const double frac = (t - w.start) / p.window_sec;
            if (h.pre > 0.0 && frac < 0.55) {
                pre += h.pre;
                if (first_pre_t < 0.0) first_pre_t = t;
                if (h.pre > best_hook_w) { best_hook_w = h.pre; w.hook_cue = j; }
            }
            if (h.numeric_hits > 0) {
                payload += p.w_payload_numeric * h.numeric_hits;
                for (int k = 0; k < h.numeric_hits; ++k) numeric_times.push_back(t);
                if (first_payload_t < 0.0) first_payload_t = t;
                if (best_hook_w == 0.0) w.hook_cue = j;
            }
            if (h.payload > 0.0) {
                payload += h.payload;
                if (first_payload_t < 0.0) first_payload_t = t;
            }
            if (h.trailing > 0.0 && frac > 0.55) {
                trailing += h.trailing;
                last_trailing_t = t;
            }
            if (h.coldopen_echo) {
                coldopen = p.w_coldopen;
                if (first_echo_t < 0.0) first_echo_t = t;
            }
            for (const char* s : h.signals) w.signals.insert(s);
        }
        w.last_cue = j > i ? j - 1 : i;
        w.end = std::min(doc.cues[w.last_cue].end_sec, w.start + p.window_sec + 8.0);

        // X-vs-Y contrast bonus: >=2 numeric payloads within 15 s (research:
        // the single strongest pattern).
        for (std::size_t a = 0; a + 1 < numeric_times.size(); ++a) {
            if (numeric_times[a + 1] - numeric_times[a] <= 15.0) {
                payload += p.w_payload_pair;
                break;
            }
        }

        w.score = p.w_pre * pre + payload + p.w_trailing * trailing + coldopen;
        w.four_beat = first_pre_t >= 0.0 && first_payload_t >= first_pre_t &&
                      last_trailing_t > first_payload_t;
        if (w.four_beat) w.score *= p.four_beat_mult;
        w.coldopen = coldopen > 0.0;
        if (w.masked) w.score = 0.0;

        // Refine the start onto the first contributing hit so clips open on
        // the hook, not on preceding filler.
        double refined = -1.0;
        for (double t : {first_pre_t, first_payload_t, first_echo_t})
            if (t >= 0.0 && (refined < 0.0 || t < refined)) refined = t;
        if (refined > w.start) w.start = refined;
        if (w.end - w.start < 25.0)
            w.end = std::min(duration_sec, w.start + 30.0);

        w.score *= position_prior((w.start + w.end) / 2.0, duration_sec);
        wins.push_back(std::move(w));
    }

    // Per-bin envelope for fusion: max window score covering each bin center.
    double max_score = 0.0;
    for (const auto& w : wins) max_score = std::max(max_score, w.score);
    if (max_score > 0.0) {
        const double bin_s = duration_sec / static_cast<double>(kBinCount);
        for (std::size_t b = 0; b < kBinCount; ++b) {
            const double center = (static_cast<double>(b) + 0.5) * bin_s;
            double best = 0.0;
            for (const auto& w : wins)
                if (center >= w.start && center <= w.end)
                    best = std::max(best, w.score);
            out.bins[b] = best / max_score;
        }
    }

    // NMS → candidates.
    std::vector<const Win*> order;
    order.reserve(wins.size());
    for (const auto& w : wins) if (w.score > 0.0) order.push_back(&w);
    std::sort(order.begin(), order.end(),
              [](const Win* a, const Win* b) { return a->score > b->score; });

    std::vector<double> kept_centers;
    for (const Win* w : order) {
        if (static_cast<int>(out.candidates.size()) >= p.max_candidates) break;
        const double center = (w->start + w->end) / 2.0;
        bool clash = false;
        for (double c : kept_centers)
            if (std::abs(c - center) < p.nms_gap_sec) { clash = true; break; }
        if (clash) continue;
        kept_centers.push_back(center);

        CruxCandidate c;
        c.start_sec = w->start;
        c.end_sec = w->end;
        c.score = w->score;
        c.four_beat = w->four_beat;
        c.cold_open_match = w->coldopen;
        c.hook_text = doc.cues[w->hook_cue].text;
        c.signals.assign(w->signals.begin(), w->signals.end());
        out.candidates.push_back(std::move(c));
    }
    return out;
}

void snap_to_cues(const CaptionDoc& doc, double slack,
                  double& start_sec, double& end_sec) {
    if (doc.cues.empty()) return;
    // Start: nearest cue start at or before start_sec, within slack.
    double best_start = start_sec;
    for (const auto& cue : doc.cues) {
        if (cue.start_sec > start_sec) break;
        if (start_sec - cue.start_sec <= slack) { best_start = cue.start_sec; break; }
    }
    // End: extend to the end of the cue that contains end_sec, within slack.
    double best_end = end_sec;
    for (const auto& cue : doc.cues) {
        if (cue.start_sec > end_sec) break;
        if (cue.end_sec >= end_sec && cue.end_sec - end_sec <= slack)
            best_end = cue.end_sec;
    }
    start_sec = best_start;
    end_sec = best_end;
}

} // namespace crux::captions
