// The reading fonts (bookfonts.h): the table, the line measures against the
// glyphs, and bookAdvance() against the canvas's own cursor arithmetic.
//
// It needs the firmware's glyphs.cpp and the shim for <Arduino.h>
// (shim/), which ./fonts.sh adds; under a run.sh without them there is
// nothing to test and it says so.
#include <stdio.h>
#if !__has_include(<Arduino.h>)
int main() {
  printf("fonts: skipped (no shim on the include path: run ./fonts.sh)\n");
  return 0;
}
#else
#include <stdlib.h>

#include <string>

#include "bookfonts.h"
#include "gfxdraw.h"
#include "src/fonts/ExtCodes.h"

static int failures = 0;
#define CHECK(cond, ...) \
  do { \
    if (!(cond)) { \
      failures++; \
      printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
      printf(__VA_ARGS__); \
      printf("\n"); \
    } \
  } while (0)

static const int EXT_N = sizeof(EXT_CODES) / sizeof(EXT_CODES[0]);

// Every font once: the 28 body fonts and the two heading-only bolds.
static std::vector<const BookFont *> allFonts() {
  std::vector<const BookFont *> out;
  auto add = [&](const BookFont *f) {
    for (const BookFont *o : out)
      if (o == f) return;
    out.push_back(f);
  };
  for (int s = 0; s < BOOK_SIZES; s++) {
    for (int style = 0; style < 4; style++) add(bookFont(s, style));
    for (int level = 1; level <= 3; level++) add(bookHeadingFont(s, level));
  }
  return out;
}

// The sketch's TextCanvas::write() and Adafruit_GFX::write() for a custom
// font, written again from their sources, keeping only what moves the
// cursor (text size 1, wrapping off). Byte by byte, as print() feeds it.
struct CanvasModel {
  const GFXfont *gfxFont;
  const ExtFont *ext;
  int cursor_x = 0;
  uint32_t cp = 0;
  uint8_t pending = 0;

  void gfxWrite(uint8_t c) {
    if (c == '\n') cursor_x = 0;
    else if (c != '\r') {
      uint8_t first = (uint8_t)gfxFont->first;
      if (c >= first && c <= (uint8_t)gfxFont->last) cursor_x += gfxFont->glyph[c - first].xAdvance;
    }
  }
  void drawCodePoint(uint32_t code) {
    // Thai comes from the font's own Thai table, with or without an extra one.
    int thai = thaiIndex(code);
    if (const ExtFont *t = thai >= 0 ? bookThaiFor(gfxFont) : nullptr) {
      cursor_x += t->glyph[thai].xAdvance;
      return;
    }
    int at = ext ? extIndex(code) : -1;
    if (at < 0) {
      char two[2];
      const char *f = foldCodePoint(code, two);
      for (; f && *f; f++) gfxWrite(*f);
      return;
    }
    cursor_x += ext->glyph[at].xAdvance;
  }
  void write(uint8_t c) {
    if (pending == 0 && c < 0x80) return gfxWrite(c);
    if (c >= 0xC0) {
      pending = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
      cp = c & (c >= 0xF0 ? 0x07 : c >= 0xE0 ? 0x0F : 0x1F);
      return;
    }
    if (c < 0x80 || !pending) {
      pending = 0;
      if (c < 0x80) gfxWrite(c);
      return;
    }
    cp = cp << 6 | (c & 0x3F);
    if (--pending == 0) drawCodePoint(cp);
  }
  int print(const char *s) {
    cursor_x = 0, pending = 0;
    for (; *s; s++) write((uint8_t)*s);
    return cursor_x;
  }
};

static void put(std::string &s, uint32_t cp) {
  if (cp < 0x80) s += (char)cp;
  else if (cp < 0x800) s += (char)(0xC0 | cp >> 6), s += (char)(0x80 | (cp & 0x3F));
  else if (cp < 0x10000) s += (char)(0xE0 | cp >> 12), s += (char)(0x80 | (cp >> 6 & 0x3F)), s += (char)(0x80 | (cp & 0x3F));
  else s += (char)(0xF0 | cp >> 18), s += (char)(0x80 | (cp >> 12 & 0x3F)), s += (char)(0x80 | (cp >> 6 & 0x3F)), s += (char)(0x80 | (cp & 0x3F));
}

static uint32_t rnd(uint32_t &seed) { return seed = seed * 1664525u + 1013904223u, seed >> 8; }

static void testTable() {
  auto fonts = allFonts();
  CHECK(fonts.size() == 30, "%d fonts", (int)fonts.size());
  for (int s = 0; s < BOOK_SIZES; s++) {
    const BookFont *r = bookFont(s, BOOK_REGULAR);
    for (int style = 0; style < 4; style++) {
      const BookFont *f = bookFont(s, style);
      CHECK(f && f->base && f->ext, "size %d style %d", s, style);
      CHECK(f->pt == r->pt, "size %d style %d is %d pt, the regular %d", s, style, f->pt, r->pt);
      // The four styles of a size share a line: the same line distance, and
      // heights within a pixel of each other.
      CHECK(f->yAdvance == r->yAdvance, "size %d style %d: line %d, the regular's %d", s, style, f->yAdvance, r->yAdvance);
      CHECK(abs(f->ascent - r->ascent) <= 1 && abs(f->descent - r->descent) <= 1 && abs(f->xHeight - r->xHeight) <= 1,
            "size %d style %d: %d/%d/%d against %d/%d/%d", s, style, f->ascent, f->descent, f->xHeight, r->ascent, r->descent, r->xHeight);
    }
    if (s) CHECK(r->pt > bookFont(s - 1, BOOK_REGULAR)->pt, "size %d isn't larger than %d", s, s - 1);
    int h1 = bookHeadingFont(s, 1)->pt, h2 = bookHeadingFont(s, 2)->pt, h3 = bookHeadingFont(s, 3)->pt;
    CHECK(h1 > h2 && h2 > h3 && h3 >= r->pt, "size %d: headings %d %d %d over %d", s, h1, h2, h3, r->pt);
    CHECK(bookHeadingFont(s, 0) == bookHeadingFont(s, 1) && bookHeadingFont(s, 9) == bookHeadingFont(s, 3), "heading levels clamp");
  }
  CHECK(bookFont(-3, 0) == bookFont(0, 0) && bookFont(99, 0) == bookFont(BOOK_SIZES - 1, 0), "sizes clamp");
  CHECK(bookFont(1, 7) == bookFont(1, 3), "styles mask");
  CHECK(bookFont(BOOK_SIZE_DEFAULT, 0)->pt == 16, "the default text size is 16 pt");
  static const int LADDER[BOOK_SIZES] = {10, 11, 12, 14, 16, 18, 21};
  for (int s = 0; s < BOOK_SIZES; s++) CHECK(bookFont(s, 0)->pt == LADDER[s], "size %d is %d pt", s, bookFont(s, 0)->pt);
  for (const BookFont *f : fonts) {
    CHECK(bookFontOf(f->base) == f && bookExtFor(f->base) == f->ext, "%d pt: looked up by its GFXfont", f->pt);
    CHECK(f->base->first == 0x20 && f->base->last == 0x7E, "%d pt: ASCII run", f->pt);
  }
  GFXfont other = *fonts[0]->base;
  CHECK(!bookFontOf(&other) && !bookExtFor(&other) && !bookExtFor(nullptr), "a font that isn't ours");
}

// The measures the generator wrote, measured again from the glyphs; and
// each glyph's bitmap starting where the one before ends.
static void testMeasures() {
  for (const BookFont *f : allFonts()) {
    const GFXfont *b = f->base;
    auto reach = [&](const char *chars, bool below) {
      int most = 0;
      for (; *chars; chars++) {
        const GFXglyph &g = b->glyph[*chars - b->first];
        int r = below ? g.yOffset + g.height : -g.yOffset;
        if (r > most) most = r;
      }
      return most;
    };
    CHECK(f->yAdvance == b->yAdvance, "%d pt: line distance", f->pt);
    CHECK(f->ascent == reach("bdfhkl", false), "%d pt: ascent %d, glyphs %d", f->pt, f->ascent, reach("bdfhkl", false));
    CHECK(f->descent == reach("gjpqy", true), "%d pt: descent %d, glyphs %d", f->pt, f->descent, reach("gjpqy", true));
    CHECK(f->xHeight == reach("xvwz", false) && f->capHeight == reach("HIEX", false), "%d pt: x and capital height", f->pt);
    CHECK(f->space == b->glyph[0].xAdvance && f->space > 0, "%d pt: space", f->pt);
    CHECK(f->xHeight < f->capHeight && f->capHeight <= f->ascent && f->ascent <= f->inkTop && f->descent <= f->inkBottom, "%d pt: order of the heights", f->pt);
    CHECK(f->inkTop + f->inkBottom <= f->yAdvance + f->yAdvance / 4, "%d pt: ink %d+%d on a line of %d", f->pt, f->inkTop, f->inkBottom, f->yAdvance);
    int top = 0, bottom = 0, at = 0;
    for (int i = 0; i <= b->last - b->first; i++) {
      const GFXglyph &g = b->glyph[i];
      CHECK(g.bitmapOffset == at, "%d pt: '%c' starts at %d, expected %d", f->pt, i + 0x20, g.bitmapOffset, at);
      at += (g.width * g.height + 7) / 8;
      if (g.height && -g.yOffset > top) top = -g.yOffset;
      if (g.height && g.yOffset + g.height > bottom) bottom = g.yOffset + g.height;
      CHECK(g.xAdvance > 0 && (i == 0) == (g.width == 0 && g.height == 0), "%d pt: '%c': only the space is empty", f->pt, i + 0x20);
    }
    at = 0;
    for (int i = 0; i < EXT_N; i++) {
      const ExtGlyph &g = f->ext->glyph[i];
      CHECK((int)g.bitmapOffset == at, "%d pt: U+%04X starts at %d, expected %d", f->pt, EXT_CODES[i], (int)g.bitmapOffset, at);
      at += (g.width * g.height + 7) / 8;
      if (g.height && -g.yOffset > top) top = -g.yOffset;
      if (g.height && g.yOffset + g.height > bottom) bottom = g.yOffset + g.height;
      CHECK(g.xAdvance > 0, "%d pt: U+%04X has no width", f->pt, EXT_CODES[i]);
      CHECK((EXT_CODES[i] == 0xA0) == (g.width == 0 && g.height == 0), "%d pt: U+%04X: only the no-break space is empty", f->pt, EXT_CODES[i]);
    }
    CHECK(f->inkTop == top && f->inkBottom == bottom, "%d pt: ink %d/%d, glyphs %d/%d", f->pt, f->inkTop, f->inkBottom, top, bottom);
  }
}

static void testAdvance() {
  // What the reader will meet: every ASCII byte, every extra letter, letters
  // that fold, letters that are dropped; and text with all of them mixed.
  std::vector<std::string> samples = {
      "", " ", "The quick brown fox jumps over the lazy dog, 0123456789!", "tab\there\rret\x7f del \x01\x1f ctl",
      "“It will come back,” he said — it always comes back…",
      "Știință și răbdare; Zażółć gęślą jaźń; Größe; ışık İstanbul",
      "Съешь ещё этих мягких булок",
      "soft­hyphen no break  thin  em",
      "folded: ‰ → ‟ ‐ ‑ Āā ẞ ⅓ ½ ″ ʼ ﬁ 中文 \U0001F600 � end",
  };
  std::string every;
  for (uint32_t c = 1; c < 0x250; c++)
    if (c != '\n') put(every, c);
  for (uint32_t c = 0x3F0; c < 0x470; c++) put(every, c);
  for (uint32_t c = 0x2000; c < 0x2220; c++) put(every, c);
  samples.push_back(every);
  uint32_t seed = 20261116;
  for (int n = 0; n < 200; n++) {  // random text: ASCII, the extra letters, anything up to U+2FFFF
    std::string s;
    for (int k = rnd(seed) % 60; k > 0; k--) {
      uint32_t pick = rnd(seed) % 10, c;
      if (pick < 4) c = 0x20 + rnd(seed) % 0x5F;
      else if (pick < 8) c = EXT_CODES[rnd(seed) % EXT_N];
      else if (pick < 9) c = 1 + rnd(seed) % 0x2FFF;
      else c = 0x80 + rnd(seed) % 0x2FF80;
      if (c != '\n' && (c < 0xD800 || c > 0xDFFF)) put(s, c);
    }
    samples.push_back(s);
  }
  // Broken UTF-8: a sequence cut short, a stray continuation byte, a lead
  // byte after a lead byte, bytes that are never valid.
  for (const char *s : {"caf\xc3", "caf\xc3!", "a\xe2\x82z", "a\xa9z", "\xc3\xc3\xa9t\xc3\xa9", "\xf0\x9f\x98x", "x\xff\xfey", "\xe2\x80\xe2\x80\x9d"})
    samples.push_back(s);
  for (int n = 0; n < 200; n++) {  // and random bytes
    std::string s;
    for (int k = 1 + rnd(seed) % 40; k > 0; k--) {
      uint8_t b = rnd(seed) % 3 ? 0x20 + rnd(seed) % 0x5F : 1 + rnd(seed) % 255;
      // An over-long form of a control (C0 8A for a line feed) is the one
      // thing the canvas and textW() read differently: the flow is checked
      // UTF-8 and never holds one.
      if (b != '\n' && b != 0xC0 && b != 0xC1 && b != 0xE0 && b != 0xF0) s += (char)b;
    }
    samples.push_back(s);
  }

  int compared = 0;
  for (const BookFont *f : allFonts()) {
    CanvasModel canvas{f->base, f->ext};
    CanvasModel plain{f->base, nullptr};  // a font the canvas has no extra table for
    GfxFrame frame(4000, f->yAdvance * 3);
    for (const std::string &s : samples) {
      const char *p = s.c_str();
      int sum = 0, sumPlain = 0;
      for (int i = 0; p[i];) {
        uint32_t cp = utf8Next(p, i);
        sum += bookAdvance(f->base, f->ext, cp);
        sumPlain += bookAdvance(f->base, nullptr, cp);
      }
      int want = canvas.print(p);
      CHECK(sum == want, "%d pt: bookAdvance adds up to %d, the canvas ends at %d: \"%.40s\"", f->pt, sum, want, p);
      CHECK(sumPlain == plain.print(p), "%d pt, no extra table: %d against %d: \"%.40s\"", f->pt, sumPlain, plain.print(p), p);
      CHECK(bookTextAdvance(f->base, f->ext, p) == want, "%d pt: bookTextAdvance", f->pt);
      // The desktop drawing moves the same way, and inks only inside the
      // font's limits and up to where textWidth() says.
      if (want >= frame.width() - 200) continue;
      frame.clear();
      const int x0 = 100, y0 = f->yAdvance * 3 / 2;
      CHECK(frame.drawText(x0, y0, f, p) == x0 + want, "%d pt: drawText's cursor", f->pt);
      int top = -1, bottom = -1, right = -1;
      for (int y = 0; y < frame.height(); y++)
        for (int x = 0; x < frame.width(); x++)
          if (!frame.pixel(x, y)) {
            if (top < 0) top = y;
            bottom = y;
            if (x > right) right = x;
          }
      if (top >= 0) {
        CHECK(top >= y0 - f->inkTop && bottom < y0 + f->inkBottom, "%d pt: ink on rows %d..%d, allowed %d..%d", f->pt, top, bottom, y0 - f->inkTop, y0 + f->inkBottom - 1);
        // textWidth() goes by the glyphs' boxes, as the board's textW(): the
        // hinted bitmaps sometimes keep an empty column or two at an edge.
        int width = textWidth(f->base, f->ext, p);
        CHECK(right + 1 <= x0 + width && right + 1 >= x0 + width - 2, "%d pt: ink ends at %d, textWidth %d: \"%.40s\"", f->pt, right + 1 - x0, width, p);
      } else {
        CHECK(textWidth(f->base, f->ext, p) == 0, "%d pt: nothing drawn but a width", f->pt);
      }
      compared++;
    }
    // One character at a time, the edges.
    const GFXfont *b = f->base;
    CHECK(bookAdvance(b, f->ext, 'A') == b->glyph['A' - 0x20].xAdvance, "A");
    CHECK(bookAdvance(b, f->ext, 0x1F) == 0 && bookAdvance(b, f->ext, 0x7F) == 0 && bookAdvance(b, f->ext, 0) == 0, "controls and DEL don't move");
    CHECK(bookAdvance(b, f->ext, '\n') == 0 && bookAdvance(b, f->ext, '\t') == 0, "line feed and tab don't move");
    CHECK(bookAdvance(b, f->ext, 0xE9) == f->ext->glyph[extIndex(0xE9)].xAdvance, "e acute from the extra table");
    CHECK(bookAdvance(b, nullptr, 0xE9) == bookAdvance(b, nullptr, 'e'), "e acute folds to e without a table");
    CHECK(bookAdvance(b, f->ext, 0x2026) == f->ext->glyph[extIndex(0x2026)].xAdvance, "the ellipsis is a glyph");
    CHECK(bookAdvance(b, nullptr, 0x2026) == 3 * bookAdvance(b, nullptr, '.'), "and three full stops without a table");
    CHECK(bookAdvance(b, f->ext, 0x2153) == bookAdvance(b, f->ext, '1') + bookAdvance(b, f->ext, '/') + bookAdvance(b, f->ext, '3'), "a third is 1/3");
    CHECK(bookAdvance(b, f->ext, 0x4E2D) == 0 && bookAdvance(b, f->ext, 0xFFFD) == 0 && bookAdvance(b, f->ext, 0x1F600) == 0, "what has no stand-in doesn't move");
    // The soft hyphen is a glyph of the tables (a hyphen): the pager takes it out.
    CHECK(extIndex(0xAD) >= 0 && bookAdvance(b, f->ext, 0xAD) == f->ext->glyph[extIndex(0xAD)].xAdvance && bookAdvance(b, f->ext, 0xAD) > 0, "soft hyphen");
    CHECK(bookAdvance(b, f->ext, 0xA0) == f->space, "a no-break space is as wide as a space");
  }
  printf("fonts: %d fonts, %d strings each, %d drawn and measured\n", (int)allFonts().size(), (int)samples.size(), compared);
}

static void testPng(const char *folder) {
  GfxFrame page(37, 5);  // a width that isn't whole bytes
  page.fillRect(1, 1, 35, 3);
  page.setPixel(-1, 0), page.setPixel(37, 4), page.setPixel(0, 5);  // outside: ignored
  CHECK(!page.pixel(1, 1) && page.pixel(0, 0) && page.pixel(36, 4) && page.pixel(99, 99), "pixels");
  std::string path = std::string(folder) + "/../build/fonts_test.png";
  CHECK(page.savePng(path.c_str()), "can't write %s", path.c_str());
  FILE *f = fopen(path.c_str(), "rb");
  uint8_t head[33] = {0};
  size_t n = f ? fread(head, 1, sizeof head, f) : 0;
  if (f) fclose(f);
  CHECK(n == 33 && !memcmp(head, "\x89PNG\r\n\x1a\n\0\0\0\rIHDR\0\0\0\x25\0\0\0\x05\x01\0\0\0\0", 29), "PNG header");
  remove(path.c_str());
}

int main(int argc, char **argv) {
  testTable();
  testMeasures();
  testAdvance();
  testPng(argc > 1 ? argv[1] : "fixtures");
  if (failures) return printf("fonts: %d checks failed\n", failures), 1;
  printf("fonts: ok\n");
  return 0;
}
#endif
