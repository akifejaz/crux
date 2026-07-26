#include "media/transliterate.h"

#include <cstdint>
#include <cstddef>
#include <unordered_map>

namespace crux::media {

namespace {

// Decode one UTF-8 codepoint at `s[i]`. Returns cp=0xFFFD on malformed input
// so the caller keeps walking rather than getting stuck.
std::uint32_t decode_utf8(const std::string& s, std::size_t& i) {
    if (i >= s.size()) return 0;
    unsigned char c0 = static_cast<unsigned char>(s[i]);
    if (c0 < 0x80) { ++i; return c0; }
    std::uint32_t cp = 0;
    int extra = 0;
    if      ((c0 & 0xE0) == 0xC0) { cp = c0 & 0x1F; extra = 1; }
    else if ((c0 & 0xF0) == 0xE0) { cp = c0 & 0x0F; extra = 2; }
    else if ((c0 & 0xF8) == 0xF0) { cp = c0 & 0x07; extra = 3; }
    else { ++i; return 0xFFFD; }
    if (i + 1 + static_cast<std::size_t>(extra) > s.size()) { ++i; return 0xFFFD; }
    for (int k = 1; k <= extra; ++k) {
        unsigned char cc = static_cast<unsigned char>(s[i + k]);
        if ((cc & 0xC0) != 0x80) { ++i; return 0xFFFD; }
        cp = (cp << 6) | (cc & 0x3F);
    }
    i += 1 + extra;
    return cp;
}

// Emit a UTF-8 codepoint back into a std::string (used to pass through
// non-script characters like punctuation and Latin words).
void encode_utf8(std::uint32_t cp, std::string& out) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// ---- Devanagari mappings (U+0900..U+097F) -------------------------------
// Consonant lookup returns the Latin form WITHOUT the inherent 'a' — the
// state machine appends 'a' itself so it can drop or replace it later.
const char* deva_consonant(std::uint32_t cp) {
    switch (cp) {
    case 0x0915: return "k";   // क
    case 0x0916: return "kh";  // ख
    case 0x0917: return "g";   // ग
    case 0x0918: return "gh";  // घ
    case 0x0919: return "ng";  // ङ
    case 0x091A: return "ch";  // च
    case 0x091B: return "chh"; // छ
    case 0x091C: return "j";   // ज
    case 0x091D: return "jh";  // झ
    case 0x091E: return "ny";  // ञ
    case 0x091F: return "t";   // ट
    case 0x0920: return "th";  // ठ
    case 0x0921: return "d";   // ड
    case 0x0922: return "dh";  // ढ
    case 0x0923: return "n";   // ण
    case 0x0924: return "t";   // त
    case 0x0925: return "th";  // थ
    case 0x0926: return "d";   // द
    case 0x0927: return "dh";  // ध
    case 0x0928: return "n";   // न
    case 0x092A: return "p";   // प
    case 0x092B: return "ph";  // फ
    case 0x092C: return "b";   // ब
    case 0x092D: return "bh";  // भ
    case 0x092E: return "m";   // म
    case 0x092F: return "y";   // य
    case 0x0930: return "r";   // र
    case 0x0932: return "l";   // ल
    case 0x0935: return "v";   // व
    case 0x0936: return "sh";  // श
    case 0x0937: return "sh";  // ष
    case 0x0938: return "s";   // स
    case 0x0939: return "h";   // ह
    case 0x0958: return "q";   // क़
    case 0x0959: return "kh";  // ख़
    case 0x095A: return "gh";  // ग़
    case 0x095B: return "z";   // ज़
    case 0x095C: return "r";   // ड़
    case 0x095D: return "rh";  // ढ़
    case 0x095E: return "f";   // फ़
    case 0x095F: return "y";   // य़
    default: return nullptr;
    }
}

// Independent vowels — emitted directly, no schwa follows. We collapse
// long/short vowels ("aa"→"a", "ee"→"i", "oo"→"u") to match the chat-style
// Roman-Hindi the target audience actually writes ("dark" not "daark").
const char* deva_independent_vowel(std::uint32_t cp) {
    switch (cp) {
    case 0x0905: return "a";   // अ
    case 0x0906: return "a";   // आ
    case 0x0907: return "i";   // इ
    case 0x0908: return "i";   // ई
    case 0x0909: return "u";   // उ
    case 0x090A: return "u";   // ऊ
    case 0x090B: return "ri";  // ऋ
    case 0x090F: return "e";   // ए
    case 0x0910: return "ai";  // ऐ
    case 0x0913: return "o";   // ओ
    case 0x0914: return "au";  // औ
    default: return nullptr;
    }
}

// Matras (dependent vowel signs) — replace the previous inherent 'a'.
const char* deva_matra(std::uint32_t cp) {
    switch (cp) {
    case 0x093E: return "a";   // ा
    case 0x093F: return "i";   // ि
    case 0x0940: return "i";   // ी
    case 0x0941: return "u";   // ु
    case 0x0942: return "u";   // ू
    case 0x0943: return "ri";  // ृ
    case 0x0945: return "e";   // ॅ  candra e (English loans)
    case 0x0946: return "e";   // ॆ
    case 0x0947: return "e";   // े
    case 0x0948: return "ai";  // ै
    case 0x0949: return "o";   // ॉ  candra o (English loans — "log")
    case 0x094A: return "o";   // ॊ
    case 0x094B: return "o";   // ो
    case 0x094C: return "au";  // ौ
    default: return nullptr;
    }
}

// Nukta modifies the preceding consonant. Some inputs use the decomposed
// form (ड + ़) instead of the precomposed codepoint (ड़); we recover the
// precomposed one so the mapping table still applies.
std::uint32_t nukta_transform(std::uint32_t cp) {
    switch (cp) {
    case 0x0915: return 0x0958;   // क → क़
    case 0x0916: return 0x0959;   // ख → ख़
    case 0x0917: return 0x095A;   // ग → ग़
    case 0x091C: return 0x095B;   // ज → ज़
    case 0x0921: return 0x095C;   // ड → ड़
    case 0x0922: return 0x095D;   // ढ → ढ़
    case 0x092B: return 0x095E;   // फ → फ़
    case 0x092F: return 0x095F;   // य → य़
    default: return 0;
    }
}

bool is_deva_range(std::uint32_t cp) { return cp >= 0x0900 && cp <= 0x097F; }
bool is_arab_range(std::uint32_t cp) {
    return (cp >= 0x0600 && cp <= 0x06FF) ||     // Arabic
           (cp >= 0x0750 && cp <= 0x077F) ||     // Arabic Supplement
           (cp >= 0xFB50 && cp <= 0xFDFF) ||     // Arabic Presentation A
           (cp >= 0xFE70 && cp <= 0xFEFF);       // Arabic Presentation B
}

// ---- Arabic-script consonant mapping (Urdu focus) -----------------------
// Urdu doesn't write short vowels, so the mapping is intentionally
// consonant-heavy — that IS the informal Roman-Urdu look. Long vowels get
// their letter (ی → 'i', و → 'o', etc.).
const char* arab_letter(std::uint32_t cp, bool at_word_start) {
    switch (cp) {
    case 0x0627: return at_word_start ? "a" : "";  // ا (silent medial/final)
    case 0x0622: return "aa";                       // آ
    case 0x0628: return "b";                        // ب
    case 0x067E: return "p";                        // پ
    case 0x062A: return "t";                        // ت
    case 0x0679: return "t";                        // ٹ (retroflex)
    case 0x062B: return "s";                        // ث
    case 0x062C: return "j";                        // ج
    case 0x0686: return "ch";                       // چ
    case 0x062D: return "h";                        // ح
    case 0x062E: return "kh";                       // خ
    case 0x062F: return "d";                        // د
    case 0x0688: return "d";                        // ڈ
    case 0x0630: return "z";                        // ذ
    case 0x0631: return "r";                        // ر
    case 0x0691: return "r";                        // ڑ
    case 0x0632: return "z";                        // ز
    case 0x0698: return "zh";                       // ژ
    case 0x0633: return "s";                        // س
    case 0x0634: return "sh";                       // ش
    case 0x0635: return "s";                        // ص
    case 0x0636: return "z";                        // ض
    case 0x0637: return "t";                        // ط
    case 0x0638: return "z";                        // ظ
    case 0x0639: return "a";                        // ع (approximate)
    case 0x063A: return "gh";                       // غ
    case 0x0641: return "f";                        // ف
    case 0x0642: return "q";                        // ق
    case 0x06A9: return "k";                        // ک
    case 0x0643: return "k";                        // ك (Arabic form)
    case 0x06AF: return "g";                        // گ
    case 0x0644: return "l";                        // ل
    case 0x0645: return "m";                        // م
    case 0x0646: return "n";                        // ن
    case 0x06BA: return "n";                        // ں nun ghunna
    case 0x0648: return "o";                        // و (wao — o/u/v; pick 'o')
    case 0x0624: return "o";                        // ؤ
    case 0x06C1: return "h";                        // ہ gol he
    case 0x06BE: return "h";                        // ھ do-chashmi he
    case 0x0647: return "h";                        // ه (Arabic form)
    case 0x0629: return "h";                        // ة
    case 0x06CC: return "i";                        // ی (Urdu ya — i/y)
    case 0x064A: return "i";                        // ي (Arabic form)
    case 0x0626: return "i";                        // ئ
    case 0x06D2: return "e";                        // ے (barrey ye)
    case 0x0621: return "";                         // ء hamza
    default: return nullptr;
    }
}

// ---- Word-boundary helper ----------------------------------------------
// Any non-script, non-alphanumeric codepoint ends a Devanagari/Urdu word so
// we can drop the final schwa. Whitespace, punctuation, digits all count.
bool is_word_boundary_cp(std::uint32_t cp) {
    if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r') return true;
    if (cp < 0x0080) {
        return !((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') ||
                 (cp >= '0' && cp <= '9'));
    }
    return false;
}

// ---- Loanword dictionary (Devanagari → English) -----------------------
// Hindi captions spell English loanwords phonetically in Devanagari
// ("एफबीआई" for FBI), and a naive letter-by-letter transliterator turns
// them into gibberish ("ephabiai"). This table catches the ~top common
// terms in vlog captions so acronyms come out uppercase and loanwords come
// out spelled the way English readers expect. Fallback: transliterate.
//
// Curation notes:
//   * Acronyms: UPPERCASE target.
//   * Brand names: Titlecase.
//   * Common nouns/verbs: lowercase.
//   * Prefer the most common English spelling ("through" over "thru").
struct DevaLoan { const char* deva; const char* eng; };
const DevaLoan kDevaLoanwords[] = {
    // ---- Tech / internet ----
    {"कंप्यूटर",   "computer"},
    {"लैपटॉप",     "laptop"},
    {"मोबाइल",     "mobile"},
    {"फोन",        "phone"},
    {"स्मार्टफोन", "smartphone"},
    {"टैबलेट",     "tablet"},
    {"कीबोर्ड",    "keyboard"},
    {"माउस",       "mouse"},
    {"स्क्रीन",     "screen"},
    {"डिस्प्ले",    "display"},
    {"कैमरा",       "camera"},
    {"माइक",        "mic"},
    {"हेडफोन",      "headphone"},
    {"चार्जर",      "charger"},
    {"बैटरी",       "battery"},
    {"वाईफाई",      "WiFi"},
    {"ब्लूटूथ",     "Bluetooth"},
    {"इंटरनेट",     "internet"},
    {"ईमेल",        "email"},
    {"वेबसाइट",     "website"},
    {"वेब",         "web"},
    {"साइट",        "site"},
    {"ब्राउज़र",    "browser"},
    {"ब्राउजर",     "browser"},
    {"सर्वर",       "server"},
    {"नेटवर्क",     "network"},
    {"डाउनलोड",    "download"},
    {"अपलोड",      "upload"},
    {"इंस्टॉल",     "install"},
    {"अपडेट",       "update"},
    {"अपग्रेड",     "upgrade"},
    {"लिंक",        "link"},
    {"क्लिक",       "click"},
    {"पासवर्ड",    "password"},
    {"यूजरनेम",    "username"},
    {"यूजर",        "user"},
    {"अकाउंट",     "account"},
    {"लॉगिन",      "login"},
    {"लॉगआउट",    "logout"},
    {"ऐप",          "app"},
    {"एप",          "app"},
    {"सॉफ्टवेयर",   "software"},
    {"हार्डवेयर",   "hardware"},
    {"सिस्टम",     "system"},
    {"डेटा",        "data"},
    {"फाइल",       "file"},
    {"फोल्डर",     "folder"},
    {"मैसेज",      "message"},
    {"कॉल",         "call"},
    {"वीडियो",     "video"},
    {"ऑडियो",      "audio"},
    {"फोटो",       "photo"},
    {"स्टोरेज",    "storage"},
    {"मेमोरी",     "memory"},
    // ---- Social / media ----
    {"यूट्यूब",     "YouTube"},
    {"गूगल",        "Google"},
    {"फेसबुक",     "Facebook"},
    {"इंस्टाग्राम", "Instagram"},
    {"ट्विटर",      "Twitter"},
    {"व्हाट्सएप",   "WhatsApp"},
    {"वाट्सएप",     "WhatsApp"},
    {"टेलीग्राम",   "Telegram"},
    {"टिकटॉक",     "TikTok"},
    {"चैनल",        "channel"},
    {"कंटेंट",      "content"},
    {"सब्सक्राइबर", "subscriber"},
    {"सब्सक्राइब",  "subscribe"},
    {"लाइक",        "like"},
    {"कमेंट",      "comment"},
    {"शेयर",        "share"},
    {"पोस्ट",       "post"},
    {"स्टोरी",     "story"},
    {"रील",         "reel"},
    {"शॉर्ट",       "short"},
    {"स्ट्रीम",     "stream"},
    {"लाइव",        "live"},
    {"वायरल",      "viral"},
    {"ट्रेंडिंग",   "trending"},
    {"हैशटैग",     "hashtag"},
    // ---- Cybersecurity / crime (matches the sample video) ----
    {"हैकर",        "hacker"},
    {"हैकिंग",      "hacking"},
    {"स्कैम",       "scam"},
    {"स्कैमर",      "scammer"},
    {"फ्रॉड",       "fraud"},
    {"क्रैकडाउन",   "crackdown"},
    {"डार्क",       "dark"},
    {"डार्कवेब",    "dark web"},
    {"वीपीएन",      "VPN"},
    {"ब्लॉकचेन",    "blockchain"},
    {"क्रिप्टो",    "crypto"},
    {"बिटकॉइन",     "Bitcoin"},
    // ---- Acronyms ----
    {"एफबीआई",     "FBI"},
    {"सीआईए",       "CIA"},
    {"नासा",       "NASA"},
    {"यूएसए",       "USA"},
    {"यूके",        "UK"},
    {"यूएन",        "UN"},
    {"डब्ल्यूएचओ",  "WHO"},
    {"एआई",         "AI"},
    {"जीपीटी",      "GPT"},
    {"आईटी",        "IT"},
    {"एटीएम",       "ATM"},
    {"पिन",         "PIN"},
    {"ओटीपी",       "OTP"},
    {"आईपीएल",      "IPL"},
    {"बीसीसीआई",    "BCCI"},
    {"पीएम",        "PM"},
    {"सीएम",        "CM"},
    {"एमपी",        "MP"},
    {"पीडीएफ",     "PDF"},
    {"यूएसबी",     "USB"},
    {"जीबी",        "GB"},
    {"एमबी",        "MB"},
    {"केबी",        "KB"},
    {"एमपी3",       "MP3"},
    {"एमपी4",       "MP4"},
    // ---- Common English words in Hindi speech ----
    {"हैलो",        "hello"},
    {"ओके",         "OK"},
    {"सॉरी",       "sorry"},
    {"थैंक्स",      "thanks"},
    {"थैंक्यू",     "thank you"},
    {"प्लीज",       "please"},
    {"एक्चुअली",    "actually"},
    {"ऑब्वियसली",   "obviously"},
    {"सीरियसली",    "seriously"},
    {"एग्जैक्टली",  "exactly"},
    {"बेसिकली",     "basically"},
    {"डेफिनेटली",   "definitely"},
    {"प्रॉब्लम",    "problem"},
    {"सॉल्यूशन",    "solution"},
    {"लेवल",        "level"},
    {"स्टाइल",      "style"},
    {"ब्रांड",      "brand"},
    {"प्राइस",      "price"},
    {"ऑफर",         "offer"},
    {"डिस्काउंट",   "discount"},
    {"मार्केट",     "market"},
    {"बिज़नेस",     "business"},
    {"बिजनेस",      "business"},
    {"ऑफिस",        "office"},
    {"मीटिंग",     "meeting"},
    {"रोस्ट",       "roast"},
    {"रोस्टेड",     "roasted"},
    {"थ्रू",         "through"},
    {"थैंक्यू",     "thank you"},
    {"रिपोर्ट",     "report"},
    {"न्यूज़",      "news"},
    {"न्यूज",       "news"},
    {"इंटरव्यू",    "interview"},
    {"स्कैंडल",     "scandal"},
    {"पोलीस",       "police"},
    {"पुलिस",       "police"},
};

const std::unordered_map<std::string, const char*>& deva_loanword_map() {
    static const std::unordered_map<std::string, const char*> m = [] {
        std::unordered_map<std::string, const char*> map;
        map.reserve(sizeof(kDevaLoanwords) / sizeof(kDevaLoanwords[0]));
        for (const auto& e : kDevaLoanwords) map[e.deva] = e.eng;
        return map;
    }();
    return m;
}

const char* deva_loanword_lookup(const std::string& word) {
    const auto& m = deva_loanword_map();
    auto it = m.find(word);
    return (it == m.end()) ? nullptr : it->second;
}

} // namespace

int detect_script(const std::string& text) {
    int deva = 0, arab = 0;
    std::size_t i = 0;
    while (i < text.size()) {
        std::uint32_t cp = decode_utf8(text, i);
        if (is_deva_range(cp)) ++deva;
        else if (is_arab_range(cp)) ++arab;
    }
    if (deva == 0 && arab == 0) return 0;
    return deva >= arab ? 1 : 2;
}

namespace {

// A "Devanagari codepoint" for word-run segmentation: any codepoint in the
// Devanagari block, including matras, halant, nukta, nasals, digits, and
// dandas. Danda (।) actually acts as end-of-sentence and is emitted as a
// period on transliteration, but for segmentation it belongs to the run.
bool is_deva_wordchar(std::uint32_t cp) { return is_deva_range(cp); }

// Transliterate a single Devanagari-only word (no whitespace, no Latin,
// no dictionary lookup — this is the phonetic fallback). Extracted from
// devanagari_to_latin so the top-level walker can dispatch to either the
// loanword table or this function.
std::string transliterate_deva_word(const std::string& word) {
    std::string out;
    out.reserve(word.size() * 2);
    bool pending_a = false;
    std::uint32_t last_cons_cp = 0;
    std::size_t last_cons_out_pos = 0;
    auto reset_last_cons = [&]() { last_cons_cp = 0; };

    std::size_t i = 0;
    while (i < word.size()) {
        std::uint32_t cp = decode_utf8(word, i);

        if (const char* c = deva_consonant(cp)) {
            last_cons_cp = cp;
            last_cons_out_pos = out.size();
            out += c;
            out.push_back('a');
            pending_a = true;
            continue;
        }
        if (const char* v = deva_matra(cp)) {
            if (pending_a) out.pop_back();
            out += v;
            pending_a = false;
            reset_last_cons();
            continue;
        }
        if (cp == 0x094D) {   // ् virama/halant
            if (pending_a) out.pop_back();
            pending_a = false;
            reset_last_cons();
            continue;
        }
        if (cp == 0x093C) {   // ़ nukta — rewrite previous consonant
            if (last_cons_cp) {
                std::uint32_t nc = nukta_transform(last_cons_cp);
                const char* nm = nc ? deva_consonant(nc) : nullptr;
                if (nm) {
                    out.resize(last_cons_out_pos);
                    out += nm;
                    if (pending_a) out.push_back('a');
                    last_cons_cp = nc;
                }
            }
            continue;
        }
        if (const char* v = deva_independent_vowel(cp)) {
            out += v;
            pending_a = false;
            reset_last_cons();
            continue;
        }
        if (cp == 0x0902 || cp == 0x0901) {   // ं / ँ nasal
            out.push_back('n');
            pending_a = false;
            reset_last_cons();
            continue;
        }
        if (cp == 0x0903) {   // ः visarga
            out.push_back('h');
            pending_a = false;
            reset_last_cons();
            continue;
        }
        if (cp >= 0x0966 && cp <= 0x096F) {   // Devanagari digits
            out.push_back(static_cast<char>('0' + (cp - 0x0966)));
            pending_a = false;
            continue;
        }
        if (cp == 0x0964 || cp == 0x0965) {   // । ॥ danda
            if (pending_a) { out.pop_back(); pending_a = false; }
            out.push_back('.');
            continue;
        }
        // Unknown Devanagari mark — pass through raw so we don't lose data.
        encode_utf8(cp, out);
        pending_a = false;
        reset_last_cons();
    }
    if (pending_a) out.pop_back();   // trailing schwa deletion
    return out;
}

} // namespace

std::string devanagari_to_latin(const std::string& text) {
    std::string out;
    out.reserve(text.size() * 2);

    // Walk the input, collecting maximal runs of Devanagari codepoints
    // ("words"). Each word is first looked up in the loanword dictionary
    // (Devanagari-spelled English → real English); if not found, it goes
    // through the phonetic transliterator.
    std::string buf;
    std::size_t i = 0;
    while (i < text.size()) {
        std::size_t start = i;
        std::uint32_t cp = decode_utf8(text, i);
        if (is_deva_wordchar(cp)) {
            // Accumulate the raw Devanagari bytes for the dictionary key.
            buf.append(text, start, i - start);
            continue;
        }
        // End of a Devanagari run — flush.
        if (!buf.empty()) {
            if (const char* eng = deva_loanword_lookup(buf))
                out.append(eng);
            else
                out.append(transliterate_deva_word(buf));
            buf.clear();
        }
        // Emit the current non-Devanagari codepoint verbatim.
        encode_utf8(cp, out);
    }
    if (!buf.empty()) {
        if (const char* eng = deva_loanword_lookup(buf))
            out.append(eng);
        else
            out.append(transliterate_deva_word(buf));
    }
    return out;
}

std::string arabic_to_latin(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    bool at_word_start = true;

    std::size_t i = 0;
    while (i < text.size()) {
        std::uint32_t cp = decode_utf8(text, i);

        // Eastern Arabic digits (٠..٩ and ۰..۹) → Latin, before the general
        // letter path (they live inside is_arab_range).
        if (cp >= 0x0660 && cp <= 0x0669) {
            out.push_back(static_cast<char>('0' + (cp - 0x0660)));
            at_word_start = false;
            continue;
        }
        if (cp >= 0x06F0 && cp <= 0x06F9) {
            out.push_back(static_cast<char>('0' + (cp - 0x06F0)));
            at_word_start = false;
            continue;
        }
        if (is_arab_range(cp)) {
            // Arabic diacritics — short vowels, tanwin, shadda, sukun.
            // Skipping them is exactly what Roman-Urdu chat does.
            if (cp >= 0x064B && cp <= 0x065F) continue;
            if (cp >= 0x0670 && cp <= 0x0674) continue;
            const char* r = arab_letter(cp, at_word_start);
            if (r) out += r;
            at_word_start = false;
            continue;
        }

        // Non-Arabic character: pass through and reset word-start tracking.
        encode_utf8(cp, out);
        at_word_start = is_word_boundary_cp(cp);
    }
    return out;
}

std::string to_latin(const std::string& text, const std::string& lang_hint) {
    // Fast path: English/Latin captions never need conversion. Anything
    // starting with "en" (en, en-orig, en-US, …) counts as English.
    if (lang_hint.rfind("en", 0) == 0) return text;

    int script = 0;
    if      (lang_hint.rfind("hi", 0) == 0 ||
             lang_hint.rfind("mr", 0) == 0 ||
             lang_hint.rfind("ne", 0) == 0) script = 1;
    else if (lang_hint.rfind("ur", 0) == 0 ||
             lang_hint.rfind("ar", 0) == 0 ||
             lang_hint.rfind("fa", 0) == 0) script = 2;
    else script = detect_script(text);

    switch (script) {
    case 1: return devanagari_to_latin(text);
    case 2: return arabic_to_latin(text);
    default: return text;
    }
}

} // namespace crux::media
