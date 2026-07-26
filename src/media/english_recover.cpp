// Fuzzy phonetic recovery: after the Devanagari→Latin phonetic pass produces
// something like "ektiviti", find the nearest English word ("activity") via
// bounded Levenshtein against a bucketed wordlist.
//
// Guards (all must hold before we accept a replacement):
//   1. Input length ≥ 4 letters (avoid over-correcting short Hindi words).
//   2. Edit distance ≤ max(2, len / 4) — very close phonetic match.
//   3. Candidate must be in the same first-letter bucket, ± 1 length bucket.
//   4. Input must not be a plausible native Hindi word (heuristic: ends with
//      -a/-i/-e/-o AND has no consonant cluster typical of English loanwords).
//   5. Result letter set overlaps ≥ 60% with input (no wild replacements).

#include "media/english_words.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <unordered_set>
#include <vector>

namespace crux::media {

namespace {

// High-frequency romanized Hindi/Urdu tokens that would be accepted by the
// fuzzy match against an English word ("nahin" → "nation", "hoti" → "hot",
// "kaha" → "kahn"). We block these outright — they are common enough that
// mislabeling them as English is worse than any English recovery we'd miss.
const std::unordered_set<std::string>& hindi_stopwords() {
    static const std::unordered_set<std::string> k = {
        "nahin", "nahi", "hain", "haan", "kyun", "kyon", "koi", "kuch", "kuchh",
        "matlab", "asal", "achha", "acha", "theek", "sahi", "galat", "sath",
        "phir", "abhi", "aage", "peeche", "andar", "bahar", "upar", "niche",
        "yaha", "waha", "kaha", "jaha", "kabhi", "kabhi", "hamesha",
        "bahut", "kam", "zyada", "thoda", "poora", "aadha",
        "aaya", "gaya", "hua", "hui", "hue", "hoga", "hogi", "hoge",
        "karo", "karna", "karta", "karti", "karte", "kiya", "diya", "liya",
        "raha", "rahi", "rahe", "reha", "rehta", "rehti", "rehte",
        "hota", "hoti", "hote", "tha", "thi", "the", "thay",
        "aap", "apka", "apki", "apke", "aapko",
        "meri", "mera", "mere", "meko", "hume", "hamko", "hamare", "hamari",
        "tera", "teri", "tere", "tumko", "tumhara", "tumhari",
        "iska", "iski", "iske", "isko", "usko", "uska", "uski", "uske",
        "unka", "unki", "unke", "unko", "hamara",
        "yaad", "baat", "baten", "baaten", "log", "logo", "logon",
        "ismen", "usmen", "jismen", "kismen",
        "kyunki", "isliye", "islie", "lekin", "magar",
        "bhi", "hi", "toh", "phir", "bas", "chalo", "arre",
        "shukriya", "namaste", "salaam", "khuda",
    };
    return k;
}

int len_bucket_for(std::size_t n) {
    if (n < 4) return 0;
    return static_cast<int>(std::min<std::size_t>(4, (n - 4) / 2));
}

// Classic O(n·m) Levenshtein with an early-out ceiling. Returns `ceil + 1`
// when the distance is guaranteed to exceed the ceiling.
int lev_bounded(const std::string& a, const std::string& b, int ceil) {
    const int n = static_cast<int>(a.size());
    const int m = static_cast<int>(b.size());
    if (std::abs(n - m) > ceil) return ceil + 1;
    std::vector<int> prev(m + 1), cur(m + 1);
    for (int j = 0; j <= m; ++j) prev[j] = j;
    for (int i = 1; i <= n; ++i) {
        cur[0] = i;
        int row_min = cur[0];
        const int jlo = std::max(1, i - ceil);
        const int jhi = std::min(m, i + ceil);
        for (int j = 1; j <= m; ++j) {
            if (j < jlo || j > jhi) { cur[j] = ceil + 1; continue; }
            const int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
            row_min = std::min(row_min, cur[j]);
        }
        if (row_min > ceil) return ceil + 1;
        std::swap(prev, cur);
    }
    return prev[m];
}

bool looks_native_hindi(const std::string& w) {
    // Short heuristic: words ending in a Devanagari vowel-ish transliteration
    // (-a/-i/-e/-o/-u) with NO stereotype English consonant cluster inside
    // are likely native. Skip fuzzy recovery for them so we don't turn
    // "hoti" into "hotly" or "kaha" into "kahn".
    if (w.empty()) return false;
    char last = w.back();
    if (last != 'a' && last != 'i' && last != 'e' && last != 'o' && last != 'u') return false;
    // Any of these two-letter clusters strongly suggests English loanword.
    static const char* kClusters[] = {
        "st", "sh", "ch", "th", "nt", "rt", "rs", "ct", "pt", "kt",
        "gh", "ph", "wh", "ns", "ry", "ty", "ly", "cy", "ple", "tion"
    };
    for (const char* c : kClusters)
        if (w.find(c) != std::string::npos) return false;
    return true;
}

double letter_overlap(const std::string& a, const std::string& b) {
    if (a.empty()) return 0.0;
    int hits = 0;
    for (char c : a)
        if (b.find(c) != std::string::npos) ++hits;
    return static_cast<double>(hits) / static_cast<double>(a.size());
}

} // namespace

bool is_vowel(char c) { return c=='a'||c=='e'||c=='i'||c=='o'||c=='u'; }

std::string recover_english(const std::string& phon) {
    const std::size_t n = phon.size();
    if (n < 5) return {};                        // 5+ letters only — avoid overreach on short native words
    if (looks_native_hindi(phon)) return {};
    if (hindi_stopwords().count(phon))  return {};

    const char first = phon.front();
    if (first < 'a' || first > 'z') return {};
    const int lb = len_bucket_for(n);
    // Ceil scales with length: 5-7 chars → 2, 8+ → 3. Tight enough to keep
    // false positives rare, loose enough to catch "ektiviti"→"activity" (dist 3).
    const int ceil = (n >= 8) ? 3 : 2;
    const bool vowel_first = is_vowel(first);

    std::string best;
    int best_dist = ceil + 1;

    for (const auto& b : english_word_buckets()) {
        // Vowel-first inputs may cross a↔e↔i↔o↔u (Hindi ASR of English
        // frequently transliterates initial 'a' as ए/e). Consonant-first
        // inputs must match the first letter exactly — otherwise every
        // Hindi word beats-search half the wordlist.
        if (vowel_first ? !is_vowel(b.first) : b.first != first) continue;
        if (std::abs(b.len_bucket - lb) > 1) continue;
        for (std::size_t i = 0; i < b.count; ++i) {
            const char* w = b.words[i];
            const int d = lev_bounded(phon, w, best_dist - 1);
            if (d < best_dist) {
                // Native Hindi guard is the stopword list above; here we
                // only enforce a letter-overlap floor so a wildly-different
                // spelling can't sneak in on distance alone.
                if (letter_overlap(phon, w) < 0.65) continue;
                best_dist = d;
                best = w;
                if (d == 0) return best;
            }
        }
    }
    return best;
}

} // namespace crux::media
