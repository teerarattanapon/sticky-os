// The Books app's reading typeface: GNU FreeSerif in regular, italic, bold
// and bold italic at seven text sizes, plus two larger bolds for chapter
// titles. Each is a pair as the rest of the firmware's fonts: an Adafruit GFX
// font for printable ASCII and an ExtFont for the 340 letters beyond it
// (glyphs.h), here in one generated header a font (src/fonts/BookSerif*.h,
// written by firmware/tools/fontext/make.sh) together with the line's measures.
//
// The data (about 740 KB, all of it const: flash, no RAM) is compiled once,
// in bookfonts.cpp; every other file sees only the functions below.
//
// The size ladder, in fontconvert's points at 141 dpi (the em in pixels; on
// the 235 ppi panel a "12" is 7 pt type in print, a "21" 12.5 pt):
//
//   text size   0        1        2        3        4        5        6
//   body        10 (20)  11 (22)  12 (24)  14 (27)  16 (31)  18 (35)  21 (41)   four styles each
//   heading 3   10       11       14       16       18       21       21        bold
//   heading 2   12       14       16       18       21       24       24        bold
//   heading 1   14       16       18       21       24       28       28        bold
//
// On a computer (the host tests, the desktop renderer) this needs the include
// path firmware/test/ebook/shim, which stands in for <Arduino.h> and
// Adafruit's gfxfont.h; glyphs.cpp and bookfonts.cpp are compiled in as they are.
#pragma once
#include <Arduino.h>
#ifdef ARDUINO
#include <Adafruit_GFX.h>
#else
#include <gfxfont.h>
#endif
#include "glyphs.h"

struct BookFont {
  const GFXfont *base;  // printable ASCII, 0x20..0x7E
  const ExtFont *ext;   // the letters beyond it, in EXT_CODES' order
  uint8_t pt;           // the size it was made at (141 dpi)
  uint8_t yAdvance;     // the typeface's own line distance (about 1.2 em)
  // In pixels from the baseline. A glyph drawn with the cursor at y inks the
  // rows y - ascent .. y + descent - 1.
  uint8_t ascent;       // the tall letters: b d f h k l
  uint8_t descent;      // the hanging ones: g j p q y
  uint8_t xHeight;      // x
  uint8_t capHeight;    // H
  uint8_t inkTop;       // the tallest glyph of all (an accented capital), and
  uint8_t inkBottom;    // the deepest: nothing of a line is outside these
  uint8_t space;        // the width of a space
};

static const int BOOK_SIZES = 7;         // the reader's text sizes, 0 the smallest
static const int BOOK_SIZE_DEFAULT = 4;  // 16 pt
enum BookStyle : uint8_t { BOOK_REGULAR = 0, BOOK_ITALIC = 1, BOOK_BOLD = 2, BOOK_BOLD_ITALIC = 3 };  // italic | bold

// The body font of a text size in a style. Sizes and styles out of range are
// clamped / masked, so this never returns nullptr.
const BookFont *bookFont(int size, int style);
// A chapter title's font for a text size: level 1 the largest, 3 the smallest
// (deeper levels get 3's). Bold; see the table above.
const BookFont *bookHeadingFont(int size, int level);

// Every font above, for a canvas that is handed only the GFXfont:
// TextCanvas::extFor() in the sketch asks this for the fonts it doesn't list.
const BookFont *bookFontOf(const GFXfont *base);  // nullptr: not a book font
const ExtFont *bookExtFor(const GFXfont *base);   // nullptr: not a book font
const ExtFont *bookThaiFor(const GFXfont *base);  // its Thai table (glyphs.h, thaiIndex); nullptr: not a book font

// How far one character moves the cursor, exactly as the firmware's canvas
// prints it (TextCanvas in sticky-os.ino) and textW() measures it: ASCII from
// `base`, a letter of the extra tables from `ext`, anything else as its ASCII
// stand-in (foldCodePoint: "..." for an ellipsis the tables lack, 'e' for an e
// with an unknown mark), and 0 for what is dropped: controls, 0x7F, a
// character with no stand-in, U+FFFD. `ext` may be nullptr (everything beyond
// ASCII is then folded, as the canvas does for a font without a table).
//
// Two things the pager must do itself, because the canvas doesn't:
//  - U+00AD, the soft hyphen, IS a glyph of the tables (a hyphen), so it is
//    measured and printed as one. Take it out of what is drawn, and measure
//    a '-' where a line is broken at one.
//  - '\n' and '\r' measure 0 here, but a '\n' printed on the canvas starts a
//    new line: never print one. The canvas also wraps at its right edge
//    unless setTextWrap(false) was called.
int bookAdvance(const GFXfont *base, const ExtFont *ext, uint32_t cp);
// The same for a run of UTF-8, `len` bytes (-1: up to the NUL): where the cursor ends up.
int bookTextAdvance(const GFXfont *base, const ExtFont *ext, const char *utf8, int len = -1);
