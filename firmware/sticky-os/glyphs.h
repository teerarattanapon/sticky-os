// Text beyond ASCII. The screen's fonts are Adafruit GFX fonts, which hold one
// run of characters each (printable ASCII, 0x20..0x7E). Every font the
// screens use has a second table for the letters of the board's languages
// (src/fonts/*ptExt.h, made by firmware/tools/fontext): Latin-1, Latin
// Extended-A, Romanian's comma-below letters, basic Cyrillic and some
// punctuation, 340 code points in all. The same sorted list of code points
// (EXT_CODES) indexes every font's table, so a lookup is one binary search.
//
// Text is UTF-8 everywhere. The canvas (sticky-os.ino) decodes it as it
// prints, and measures it the same way; a character no font has gets an ASCII
// stand-in (foldCodePoint) or nothing.
#pragma once
#include <Arduino.h>

struct ExtGlyph {
  uint32_t bitmapOffset;
  uint8_t width, height, xAdvance;
  int8_t xOffset, yOffset;
};
struct ExtFont {
  const uint8_t *bitmap;
  const ExtGlyph *glyph;  // EXT_COUNT of them, in EXT_CODES' order
};

int extIndex(uint32_t cp);          // where cp is in every ExtFont, -1: not there

// Thai, U+0E01..U+0E3A and U+0E3F..U+0E5B (87 code points), lives in a third
// table beside a font's own (src/fonts/thai, made by firmware/tools/fontext/thai.sh),
// an ExtFont in this order. Its marks have no advance: they are drawn over or
// under the letter before them.
static const int THAI_COUNT = 87;
inline int thaiIndex(uint32_t cp) {
  if (cp >= 0x0E01 && cp <= 0x0E3A) return (int)(cp - 0x0E01);
  if (cp >= 0x0E3F && cp <= 0x0E5B) return (int)(cp - 0x0E3F) + 58;
  return -1;
}

// Thai fonts drawn without OpenType's mark positioning set every mark above
// the letter at the same height, so a tone mark after an upper vowel ("ที่")
// lands on it, and one over a tall letter (ป ฝ ฟ) on its stem. Fed each
// glyph drawn, in order, this says how many pixels to lift a tone mark so it
// clears what is under it (0 for anything else).
struct ThaiStack {
  int baseTop = 0, upperTop = 0;  // pixels above the baseline
  bool hasUpper = false;
  void base(int top) { baseTop = top, hasUpper = false; }  // a letter (any script) was drawn
  // `next`: the character after cp (0: none); `saraAm`: the font's U+0E33,
  // whose ring the tone mark goes over in "น้ำ"; `lineH`: sets the gap.
  int raise(uint32_t cp, const ExtGlyph &g, uint32_t next, const ExtGlyph *saraAm, int lineH);
};
bool drawable(uint32_t cp);         // printable ASCII, or in the extra tables
// An ASCII look-alike for a character the fonts can't draw ("..." for an
// ellipsis, "1/2", 'e' for an e with a mark the tables don't have); nullptr: none, drop it.
const char *foldCodePoint(uint32_t cp, char *two);

// UTF-8, one code point at a time. Returns 0 at the end; a broken byte comes back as 0xFFFD.
uint32_t utf8Next(const char *s, int &i);
int utf8Back(const char *s, int end);     // where the character before byte `end` starts
int utf8Count(const char *s);             // characters, not bytes
void utf8Append(String &out, uint32_t cp);
// The longest start of `s` that is whole characters and at most `bytes` long.
int utf8Cut(const char *s, int bytes);
void utf8Copy(char *dst, const char *src, size_t cap);  // strlcpy that never splits a character

// Upper case for the letters the fonts have (Latin-1, Latin Extended-A,
// comma-below, Cyrillic); `turkish`: i -> İ, ı -> I.
uint32_t cpUpper(uint32_t cp, bool turkish);
String utf8Upper(const String &s, bool turkish);

// Text from the internet or the AI as the screen can show it: what the
// fonts have stays, the rest is folded (curly quotes, dashes, fractions...)
// or dropped; controls become spaces.
String textFold(const String &s);
