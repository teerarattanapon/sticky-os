// The board's text drawing, on a computer: a 1-bit picture and the firmware's
// fonts drawn into it pixel for pixel as the board does it, saved as a PNG.
// For the font specimens (tools/fontspecimen.cpp) and the desktop renderer of
// the Books app's screens.
//
// It draws what the sketch's TextCanvas draws (sticky-os.ino), with text size
// 1 and wrapping off:
//  - printable ASCII from the GFXfont, as Adafruit_GFX::drawChar() for a
//    custom font: the glyph's bitmap at cursor + (xOffset, yOffset), then the
//    cursor moves by xAdvance. y is the baseline. Only ink is drawn: there is
//    no background colour;
//  - a letter of the extra tables (glyphs.h) from the ExtFont, the same way;
//  - anything else as its ASCII stand-in (foldCodePoint), or not at all.
// UTF-8 is decoded by the firmware's own utf8Next(); a broken byte draws
// nothing, as on the board. Like the board, U+00AD (a soft hyphen) is a glyph
// and is drawn, and a '\n' is nothing here (on the board it starts a new
// line): callers lay out lines themselves.
//
// The picture is kept as the board keeps its canvas (GFXcanvas1): rows of
// bytes, the leftmost pixel in the high bit, 1 = paper (white), 0 = ink.
//
// Header only. Needs -I firmware/sticky-os -I firmware/test/ebook/shim, and
// glyphs.cpp (and bookfonts.cpp, for bookTextAdvance) compiled in.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <vector>

#include "bookfonts.h"

static const uint8_t GFX_INK = 0, GFX_PAPER = 1;

class GfxFrame {
 public:
  GfxFrame(int width, int height) : w_(width), h_(height), stride_((width + 7) / 8), bits_((size_t)stride_ * height, 0xFF) {}

  int width() const { return w_; }
  int height() const { return h_; }
  int stride() const { return stride_; }                 // bytes a row
  const uint8_t *data() const { return bits_.data(); }   // stride() * height() bytes
  void clear(uint8_t colour = GFX_PAPER) { memset(bits_.data(), colour ? 0xFF : 0x00, bits_.size()); }

  // Everything below clips to the picture.
  void setPixel(int x, int y, uint8_t colour = GFX_INK) {
    if (x < 0 || y < 0 || x >= w_ || y >= h_) return;
    uint8_t &b = bits_[(size_t)y * stride_ + x / 8], m = 0x80 >> (x & 7);
    if (colour) b |= m;
    else b &= ~m;
  }
  bool pixel(int x, int y) const {  // true: paper. Outside the picture is paper
    if (x < 0 || y < 0 || x >= w_ || y >= h_) return true;
    return bits_[(size_t)y * stride_ + x / 8] & (0x80 >> (x & 7));
  }
  void fillRect(int x, int y, int w, int h, uint8_t colour = GFX_INK) {
    for (int yy = y < 0 ? 0 : y; yy < y + h && yy < h_; yy++)
      for (int xx = x < 0 ? 0 : x; xx < x + w && xx < w_; xx++) setPixel(xx, yy, colour);
  }
  void hline(int x, int y, int w, uint8_t colour = GFX_INK) { fillRect(x, y, w, 1, colour); }
  void vline(int x, int y, int h, uint8_t colour = GFX_INK) { fillRect(x, y, 1, h, colour); }

  // Prints `len` bytes of UTF-8 (-1: up to the NUL) with the baseline at y,
  // starting at x; returns where the cursor ends, which is
  // x + bookTextAdvance(base, ext, utf8, len). `ext` may be nullptr.
  int drawText(int x, int y, const GFXfont *base, const ExtFont *ext, const char *utf8, int len = -1, uint8_t colour = GFX_INK) {
    char two[2];
    const ExtFont *thai = bookThaiFor(base);
    ThaiStack stack;
    for (int i = 0; (len < 0 || i < len) && utf8[i];) {
      uint32_t cp = utf8Next(utf8, i);
      int t = thai ? thaiIndex(cp) : -1;
      if (t >= 0) {  // Thai, its tone marks lifted as TextCanvas does
        int j = i;
        uint32_t next = (len < 0 || j < len) ? utf8Next(utf8, j) : 0;
        const ExtGlyph &g = thai->glyph[t];
        int lift = stack.raise(cp, g, next, next == 0x0E33 ? &thai->glyph[thaiIndex(0x0E33)] : nullptr, base->yAdvance);
        blit(thai->bitmap + g.bitmapOffset, g.width, g.height, x + g.xOffset, y + g.yOffset - lift, colour);
        x += g.xAdvance;
        continue;
      }
      int at = ext && cp >= 0x80 ? extIndex(cp) : -1;
      stack.base(at >= 0 ? -ext->glyph[at].yOffset : 0);
      if (at >= 0) {
        const ExtGlyph &g = ext->glyph[at];
        blit(ext->bitmap + g.bitmapOffset, g.width, g.height, x + g.xOffset, y + g.yOffset, colour);
        x += g.xAdvance;
        continue;
      }
      char one[2] = {(char)cp, 0};
      for (const char *p = cp < 0x80 ? one : foldCodePoint(cp, two); p && *p; p++) {
        uint8_t c = *p;
        if (c < base->first || c > base->last) continue;
        const GFXglyph &g = base->glyph[c - base->first];
        blit(base->bitmap + g.bitmapOffset, g.width, g.height, x + g.xOffset, y + g.yOffset, colour);
        x += g.xAdvance;
      }
    }
    return x;
  }
  int drawText(int x, int y, const BookFont *f, const char *utf8, int len = -1, uint8_t colour = GFX_INK) {
    return drawText(x, y, f->base, f->ext, utf8, len, colour);
  }

  // A PNG of the picture (1 bit a pixel, grey; stored, not compressed).
  bool savePng(const char *path) const {
    std::vector<uint8_t> z = {0x78, 0x01};  // a zlib stream of stored blocks: each row a filter byte (none) and its pixels
    uint32_t a = 1, b = 0;                  // Adler-32 of what is stored
    uint8_t pad = (w_ & 7) ? 0xFF >> (w_ & 7) : 0;
    std::vector<uint8_t> row(stride_ + 1);
    for (int y = 0; y < h_; y++) {
      row[0] = 0;
      memcpy(&row[1], &bits_[(size_t)y * stride_], stride_);
      row[stride_] &= ~pad;  // the bits past the right edge
      size_t n = row.size();
      z.push_back(y == h_ - 1);
      z.push_back(n & 0xFF), z.push_back(n >> 8), z.push_back(~n & 0xFF), z.push_back((~n >> 8) & 0xFF);
      z.insert(z.end(), row.begin(), row.end());
      for (uint8_t v : row) a = (a + v) % 65521, b = (b + a) % 65521;
    }
    uint32_t adler = b << 16 | a;
    for (int s = 24; s >= 0; s -= 8) z.push_back(adler >> s);
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    uint8_t head[13] = {(uint8_t)(w_ >> 24), (uint8_t)(w_ >> 16), (uint8_t)(w_ >> 8), (uint8_t)w_,
                        (uint8_t)(h_ >> 24), (uint8_t)(h_ >> 16), (uint8_t)(h_ >> 8), (uint8_t)h_, 1, 0, 0, 0, 0};
    chunk(f, "IHDR", head, sizeof head);
    chunk(f, "IDAT", z.data(), z.size());
    chunk(f, "IEND", nullptr, 0);
    return fclose(f) == 0;
  }

 private:
  int w_, h_, stride_;
  std::vector<uint8_t> bits_;

  // A glyph's bitmap: rows of w bits, packed without padding between the rows.
  void blit(const uint8_t *bits, int w, int h, int x, int y, uint8_t colour) {
    uint8_t byte = 0, bit = 0;
    for (int yy = 0; yy < h; yy++)
      for (int xx = 0; xx < w; xx++) {
        if (!(bit++ & 7)) byte = *bits++;
        if (byte & 0x80) setPixel(x + xx, y + yy, colour);
        byte <<= 1;
      }
  }
  static uint32_t crc(uint32_t c, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
      c ^= p[i];
      for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    }
    return c;
  }
  static void chunk(FILE *f, const char *type, const uint8_t *p, size_t n) {
    uint8_t len[4] = {(uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n};
    fwrite(len, 1, 4, f);
    fwrite(type, 1, 4, f);
    if (n) fwrite(p, 1, n, f);
    uint32_t c = ~crc(crc(0xFFFFFFFFu, (const uint8_t *)type, 4), p, n);
    uint8_t sum[4] = {(uint8_t)(c >> 24), (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c};
    fwrite(sum, 1, 4, f);
  }
};

// Where the ink of `utf8` ends when it is printed at x = 0, as the sketch's
// textW() (and Adafruit's getTextBounds: x + w): what centring and right
// alignment on the board use. 0 for nothing visible. The cursor's travel,
// which is what line breaking adds up, is bookTextAdvance().
static inline int textWidth(const GFXfont *base, const ExtFont *ext, const char *utf8, int len = -1) {
  int x = 0, minX = INT16_MAX, maxX = INT16_MIN;
  char two[2];
  for (int i = 0; (len < 0 || i < len) && utf8[i];) {
    uint32_t cp = utf8Next(utf8, i);
    int t = thaiIndex(cp);
    if (const ExtFont *thai = t >= 0 ? bookThaiFor(base) : nullptr) {
      const ExtGlyph &g = thai->glyph[t];
      if (g.width > 0 && g.height > 0) {
        if (x + g.xOffset < minX) minX = x + g.xOffset;
        if (x + g.xOffset + g.width - 1 > maxX) maxX = x + g.xOffset + g.width - 1;
      }
      x += g.xAdvance;
      continue;
    }
    int at = ext && cp >= 0x80 ? extIndex(cp) : -1;
    int w, h, xo, adv;
    char one[2] = {(char)cp, 0};
    const char *p = at >= 0 ? "?" : cp < 0x80 ? one : foldCodePoint(cp, two);  // "?": once round the loop for the extra letter
    for (; p && *p; p++) {
      if (at >= 0) {
        const ExtGlyph &g = ext->glyph[at];
        w = g.width, h = g.height, xo = g.xOffset, adv = g.xAdvance;
      } else {
        uint8_t c = *p;
        if (c < base->first || c > base->last) continue;
        const GFXglyph &g = base->glyph[c - base->first];
        w = g.width, h = g.height, xo = g.xOffset, adv = g.xAdvance;
      }
      if (w > 0 && h > 0) {
        if (x + xo < minX) minX = x + xo;
        if (x + xo + w - 1 > maxX) maxX = x + xo + w - 1;
      }
      x += adv;
    }
  }
  return maxX < minX ? 0 : maxX + 1;
}
