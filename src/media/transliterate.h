// Devanagari / Arabic-script → Latin transliteration for burned-in subtitles.
// Aims for readable Hinglish/Roman-Urdu (e.g. "maine dark web par 30 din")
// rather than academic ITRANS/IAST. Deterministic, table-driven, zero deps —
// this is not perfect, but it beats an unreadable script on a phone.
#pragma once

#include <string>

namespace crux::media {

// Detects the dominant script in `text`:
//   0 = Latin/other (skip)
//   1 = Devanagari  (Hindi, Marathi, …)
//   2 = Arabic script (Urdu, Arabic, Farsi)
int detect_script(const std::string& text);

// Word-level Devanagari → Latin with schwa-deletion at word boundaries.
// Matras replace the inherent 'a'; halant suppresses it; anusvara → 'n';
// visarga → 'h'. Trailing schwa is dropped so "raama" → "raam" reads right.
std::string devanagari_to_latin(const std::string& text);

// Arabic-script → Latin. Short vowels aren't written in Urdu so output is
// consonant-heavy on purpose ("krein gy") — that IS how Roman Urdu is
// written informally. Handles the Urdu-specific letters (پ چ ٹ ڈ ڑ گ etc.)
// on top of the base Arabic set.
std::string arabic_to_latin(const std::string& text);

// Picks the right converter based on `lang_hint` (e.g. "hi", "ur", "en")
// and falls back to script detection when the hint is empty/unknown.
// Latin text passes through unchanged.
std::string to_latin(const std::string& text, const std::string& lang_hint);

} // namespace crux::media
