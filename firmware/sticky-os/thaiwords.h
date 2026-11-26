// Thai word breaking for the Books app. Thai is written without spaces
// between words, so a line may only end where the dictionary says a word
// ends. thaiBreaks() finds those places with a dictionary of 37,654 words
// (the Royal Institute's list, from PyThaiNLP; src/thai/thai_word_dawg.h,
// written by firmware/tools/generate_thai_word_dawg.py), choosing the split
// that leaves the fewest letters outside a known word.
//
// Plain C++ (no Arduino): the host tests (firmware/test/ebook) use it too.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <vector>

// The Thai letters and marks the fonts carry (U+0E01..U+0E5B).
inline bool isThai(uint32_t cp) { return cp >= 0x0E01 && cp <= 0x0E5B; }

// Marks drawn over or under the letter before them: zero width, never a place to break.
inline bool isThaiMark(uint32_t cp) {
  return cp == 0x0E31 || (cp >= 0x0E34 && cp <= 0x0E3A) || (cp >= 0x0E47 && cp <= 0x0E4E);
}
// The ones above the letter, and of those the tone marks and the others that
// stack on top of an upper vowel (U+0E47..U+0E4E but the nikhahit pair).
inline bool isThaiUpperMark(uint32_t cp) {
  return cp == 0x0E31 || (cp >= 0x0E34 && cp <= 0x0E37) || (cp >= 0x0E47 && cp <= 0x0E4E);
}
inline bool isThaiToneMark(uint32_t cp) { return cp >= 0x0E48 && cp <= 0x0E4C; }

// Where a run of Thai may be broken between words: indexes into `cps` (a
// break before cps[i]), ascending, never 0 or cps.size().
std::vector<size_t> thaiBreaks(const std::vector<uint32_t> &cps);
