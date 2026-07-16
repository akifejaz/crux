#include "core/detector.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <vector>

namespace ytshorts::detector {

namespace {

constexpr double kFlatMaxOverMedian = 2.0;      // PLAN §4 step 4
constexpr double kThreshK = 1.5;                 // med + K·sd
constexpr double kThreshFracOfMax = 0.4;         // max(*, 0.4·mx)
constexpr double kHysteresisFrac = 0.6;          // extend edges while H >= 0.6·T
constexpr int kMergeGapBins = 1;                 // merge regions separated by ≤ 1 bin

double median_of(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const auto n = v.size();
    return (n % 2 == 1) ? v[n / 2]
                        : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

} // namespace

DetectResult detect(const Heatmap& h, double duration_sec, const Config& cfg) {
    DetectResult res;

    // Step 1 — CLEAN: copy raw values.
    std::array<double, kBinCount> H{};
    for (std::size_t i = 0; i < kBinCount; ++i) H[i] = h.bins[i].value;

    // Candidate mask. Exclude bin 0 always; exclude bin 1 iff monotone-decay
    // intro (Lex/Rick profile). --keep-intro disables both.
    std::array<bool, kBinCount> candidate{};
    candidate.fill(true);
    if (!cfg.keep_intro) {
        candidate[0] = false;
        if (H[1] > H[2] && H[2] > H[3]) candidate[1] = false;
    }

    // Step 2 — SMOOTH (skip when bins already coarse).
    const double bin_s = h.bin_seconds;
    std::array<double, kBinCount> Hs = H;
    if (bin_s < 60.0) {
        for (std::size_t i = 1; i + 1 < kBinCount; ++i)
            Hs[i] = (H[i - 1] + 2.0 * H[i] + H[i + 1]) / 4.0;
    }

    // Step 3 — STATS over candidate bins only.
    std::vector<double> vals;
    vals.reserve(kBinCount);
    for (std::size_t i = 0; i < kBinCount; ++i)
        if (candidate[i]) vals.push_back(Hs[i]);
    if (vals.empty()) return res;

    const double med = median_of(vals);
    const double mean = std::accumulate(vals.begin(), vals.end(), 0.0) /
                        static_cast<double>(vals.size());
    double var = 0.0;
    for (double v : vals) { double d = v - mean; var += d * d; }
    const double sd = std::sqrt(var / static_cast<double>(vals.size()));
    // Max over CANDIDATE bins only. Using the full-array max would let bin 0
    // (always ~1.0 on long-form) pin 0.4·mx up to 0.4 and drown real peaks
    // (verified against MrBeast Squid + Bob Lazar fixtures).
    const double mx = *std::max_element(vals.begin(), vals.end());

    res.quality.max_over_median = med > 0.0 ? mx / med : mx * 1000.0;

    // Step 4 — GATE.
    res.quality.flat = res.quality.max_over_median < kFlatMaxOverMedian;

    // Step 5 — THRESH.
    const double T = std::max(med + kThreshK * sd, kThreshFracOfMax * mx);
    const double Tlow = kHysteresisFrac * T;

    // Step 6 — REGIONS with hysteresis-extended edges, then merge.
    struct Raw { int lo, hi; };
    std::vector<Raw> raw;
    int i = 0;
    while (i < static_cast<int>(kBinCount)) {
        if (candidate[i] && Hs[i] >= T) {
            int r = i;
            while (r + 1 < static_cast<int>(kBinCount) &&
                   candidate[r + 1] && Hs[r + 1] >= Tlow) ++r;
            int l = i;
            while (l - 1 >= 0 && candidate[l - 1] && Hs[l - 1] >= Tlow) --l;
            raw.push_back({l, r});
            i = r + 1;
        } else {
            ++i;
        }
    }
    std::vector<Raw> merged;
    for (const auto& r : raw) {
        if (!merged.empty() && r.lo - merged.back().hi - 1 <= kMergeGapBins) {
            merged.back().hi = std::max(merged.back().hi, r.hi);
        } else {
            merged.push_back(r);
        }
    }

    // Step 7 — SCORE.
    for (const auto& r : merged) {
        Region reg;
        reg.start_bin = r.lo;
        reg.end_bin   = r.hi;
        double peak = -1.0; int peakBin = r.lo;
        double num = 0.0, den = 0.0;
        for (int k = r.lo; k <= r.hi; ++k) {
            if (Hs[k] > peak) { peak = Hs[k]; peakBin = k; }
            const double t_center = (k + 0.5) * bin_s;
            num += t_center * Hs[k];
            den += Hs[k];
        }
        reg.peak_score = peak;
        reg.peak_bin   = peakBin;
        reg.centroid_sec = den > 0.0 ? num / den : (r.lo + 0.5) * bin_s;
        if (reg.centroid_sec > duration_sec) reg.centroid_sec = duration_sec;
        res.regions.push_back(reg);
    }
    return res;
}

} // namespace ytshorts::detector
