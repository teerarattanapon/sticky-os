// The pager (bookpage.h). Three layers:
//
//   Win        a window on the flow file: 16 KB read ahead, a little kept
//              behind so a line can be taken back without a seek;
//   buildLine  one line from a place in the flow: greedy breaking, runs,
//              justification. nextItem wraps it with the blocks' prefixes;
//   layoutPage lines down a page until it is full, with the rules about
//              headings, lone lines and page breaks;
//
// and on top the grid (bookpage.h says what it promises): segments cut at
// anchors, a list of the page starts of the segment the reader is in, built
// forward as pages are shown. A page start carries the little state layout
// needs to go on from there (the block's kind, italic/bold, an item's
// indent), so nothing is ever read backwards.
#include "bookpage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <new>
#include <string>

#include "bookflow.h"
#include "thaiwords.h"

namespace {

const uint32_t WIN = 16384;       // the read window
const uint32_t KEEP = 2048;       // of it, what stays behind the reading point
const uint32_t SCAN = 8192;       // how far an anchor looks for a block's start
const uint32_t SCAN_SPACE = 256;  // then for a space
const uint32_t TICK_BYTES = 8192; // of flow counted by one tick()
const size_t TOC_MAX = 4000;
const size_t TITLE_MAX = 120;
const int LOOK_MAX = 6;           // lines looked at to keep a heading with its text
const uint32_t NO_KEY = 0xFFFFFFFF;
const uint16_t NO_ADV = 0xFFFF;

struct Win {
  FILE *f = nullptr;
  uint8_t *buf = nullptr;
  uint32_t start = 0, len = 0, size = 0, filePos = 0;
  bool err = false;
  BookPagerStats *st = nullptr;

  bool readAt(uint32_t off, uint8_t *to, uint32_t want) {
    if (filePos != off) {
      if (fseek(f, (long)off, SEEK_SET) != 0) return err = true, false;
      st->seeks++;
    }
    size_t got = fread(to, 1, want, f);
    filePos = off + (uint32_t)got;
    st->bytesRead += (uint32_t)got;
    if (got != want) return err = true, false;  // the file was that long when it was opened: the card is gone
    return true;
  }

  bool fill(uint32_t off) {
    if (err || off >= size) return false;
    uint32_t ns = off > KEEP ? off - KEEP : 0, have = 0;
    if (len && ns >= start && ns - start < len) {  // going forward: keep the tail, read on from where the window ended
      have = start + len - ns;
      memmove(buf, buf + (ns - start), have);
    }
    start = ns, len = have;
    uint32_t want = (size - ns < WIN ? size - ns : WIN) - have;
    if (want && !readAt(ns + have, buf + have, want)) return len = 0, false;
    len += want;
    return true;
  }

  // The bytes at off: n of them, at least 4 unless the file ends first.
  // nullptr at the file's end or when the card can't be read.
  const uint8_t *at(uint32_t off, uint32_t &n) {
    if (!(off >= start && off - start < len && (off - start + 4 <= len || start + len == size)) && !fill(off)) return nullptr;
    n = start + len - off;
    return buf + (off - start);
  }
};

// One UTF-8 character of p[0..n): its length. A broken one is U+FFFD, one byte long.
inline uint32_t decode(const uint8_t *p, uint32_t n, uint32_t &cp) {
  uint8_t b = p[0];
  uint32_t need, least;
  if (b < 0x80) return cp = b, 1;
  if (b >= 0xC2 && b <= 0xDF) need = 1, cp = b & 0x1F, least = 0x80;
  else if ((b & 0xF0) == 0xE0) need = 2, cp = b & 0x0F, least = 0x800;
  else if (b >= 0xF0 && b <= 0xF4) need = 3, cp = b & 0x07, least = 0x10000;
  else return cp = 0xFFFD, 1;
  if (n <= need) return cp = 0xFFFD, 1;
  for (uint32_t i = 1; i <= need; i++) {
    if ((p[i] & 0xC0) != 0x80) return cp = 0xFFFD, 1;
    cp = cp << 6 | (p[i] & 0x3F);
  }
  if (cp < least || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return cp = 0xFFFD, 1;
  return need + 1;
}

// Where layout stands in the flow.
struct Cur {
  uint32_t off = 0;
  uint16_t hang = 0;       // a list item's text x
  uint8_t kind = BK_PARA;  // of the block it is inside
  bool inBlock = false;    // else at a block's start (its prefix not read yet)
  bool italic = false, bold = false;
  bool afterHead = false;  // the paragraph starting next isn't indented
  bool brk = false;        // the line before ended with a forced break
};

// A page's start, as kept in the list of pages: a Cur in 8 bytes.
struct Mark {
  uint32_t off;
  uint16_t hang;
  uint8_t flags;
};
enum : uint8_t { MF_KIND = 7, MF_IN = 8, MF_ITALIC = 16, MF_BOLD = 32, MF_AFTER = 64 };

Mark markOf(const Cur &c) {
  return {c.off, c.hang, (uint8_t)((c.kind & MF_KIND) | (c.inBlock ? MF_IN : 0) | (c.italic ? MF_ITALIC : 0) | (c.bold ? MF_BOLD : 0) | (c.afterHead ? MF_AFTER : 0))};
}
Cur curOf(const Mark &m) {
  Cur c;
  c.off = m.off, c.hang = m.hang, c.kind = m.flags & MF_KIND;
  c.inBlock = m.flags & MF_IN, c.italic = m.flags & MF_ITALIC, c.bold = m.flags & MF_BOLD, c.afterHead = m.flags & MF_AFTER;
  return c;
}

// A stretch of the flow paginated on its own.
struct Seg {
  uint32_t k = 0, start = 0, limit = 0;
  bool startMid = false, limitMid = false;  // the anchor is inside a block (one too long to have a start near)
};

enum : uint8_t { IT_END, IT_LINE, IT_RULE };

// What comes next down the page.
struct Item {
  uint8_t type = IT_END;
  uint8_t kind = BK_PARA;
  bool pageBefore = false;  // its block asks for a new page
  bool first = false, last = false;  // of its block
  bool blank = false;
  int16_t asc = 0, desc = 0, lead = 0, height = 0;
  int16_t above = 0, below = 0;  // the block's space (counts before its first line, after its last)
  uint32_t firstRun = 0;
  uint16_t runCount = 0;
};

struct PageSize {
  size_t lines, runs, rules, arena;
};
PageSize sizeOf(const BookPage &p) { return {p.lines.size(), p.runs.size(), p.rules.size(), p.arena.size()}; }
void cutTo(BookPage &p, const PageSize &s) { p.lines.resize(s.lines), p.runs.resize(s.runs), p.rules.resize(s.rules), p.arena.resize(s.arena); }

struct TocRec {
  uint32_t off, title;
  uint8_t level;
};

}  // namespace

struct BookPager::Impl {
  BookLayout L;
  // From the layout: the body's line (the four body fonts share one box, so
  // lines stay evenly spaced whatever is in them) and the headings'.
  int16_t bAsc = 0, bDesc = 0, bH = 0, bLead = 0;
  int16_t hAsc[3] = {}, hDesc[3] = {}, hH[3] = {}, hLead[3] = {};
  int16_t headAbove = 0, headBelow = 0, blockSpace = 0;
  // Advances already asked for: every ASCII character per font, and a small
  // direct-mapped table for the rest. One block from PSRAM.
  uint16_t *ascii = nullptr;
  uint32_t *hkey = nullptr;
  uint16_t *hval = nullptr;

  Win w;
  BookPagerStatus status = BP_CLOSED;
  PsVec<TocRec> toc;
  PsVec<char> titles;

  BookPage page, tmp;  // the one shown; the one tick() counts with
  PsVec<Mark> cache;   // the page starts of seg, from its anchor on, as far as they are known
  size_t idx = 0;      // of the page shown
  Seg seg;
  bool segValid = false, pageValid = false;
  uint32_t pos = 0;

  // Counting the pages left in the chapter.
  bool cntDone = true, cntNeedSeg = false;
  int cntN = -1;
  uint32_t cntEnd = 0;
  Mark cntMark = {0, 0, 0};
  Seg cntSeg;

  uint64_t sumBytes = 0;  // of the pages laid out in this layout, for the estimates
  uint32_t sumPages = 0;
  BookPagerStats st;

  ~Impl() { shut(); free(ascii); }
  void shut();
  bool setLayout(const BookLayout &in);
  int adv(int role, uint32_t cp);
  int spaceW(int role);
  void loadToc(const char *flowPath);
  uint32_t anchor(uint32_t k, bool &mid);
  bool findSeg(uint32_t off, Seg &s);
  bool nextSeg(Seg &s);
  Mark fresh(const Seg &s) const;
  bool buildLine(Cur &c, uint32_t limit, BookPage &pg, bool first, Item &it);
  void nextItem(Cur &c, uint32_t limit, BookPage &pg, Item &it);
  bool keepsWithNext(const Cur &at, uint32_t limit, BookPage &pg, int y, int prevBelow);
  Mark layoutPage(const Mark &from, const Seg &s, BookPage &pg);
  bool showAt(size_t i, uint32_t until);
  bool show(uint32_t off);
  bool failed();
  int tocAt(uint32_t off) const;
  uint32_t chapterEnd(uint32_t off) const;
  void restartCount();
};

// ---- the layout and the fonts' advances

bool BookPager::Impl::setLayout(const BookLayout &in) {
  if (!ascii) {
    size_t n = BR_COUNT * 96 * sizeof(uint16_t) + 256 * sizeof(uint32_t) + 256 * sizeof(uint16_t);
    ascii = (uint16_t *)psMalloc(n);
    if (!ascii) return false;
    hval = ascii + BR_COUNT * 96;
    hkey = (uint32_t *)(hval + 256);
  }
  for (int i = 0; i < BR_COUNT * 96; i++) ascii[i] = NO_ADV;
  for (int i = 0; i < 256; i++) hkey[i] = NO_KEY;
  L = in;
  // Whatever it is given, the pager works with sizes that make sense.
  if (L.width < 8) L.width = 8;
  if (L.height < 1) L.height = 1;
  if (L.lineSpacing < 50) L.lineSpacing = 50;
  if (L.lineSpacing > 300) L.lineSpacing = 300;
  for (BookFontMetrics &m : L.font) {
    if (m.lineHeight < 1) m.lineHeight = 1;
    if (m.lineHeight > 1000) m.lineHeight = 1000;
    if (m.ascent < 0) m.ascent = 0;
    if (m.ascent > m.lineHeight) m.ascent = m.lineHeight;
  }
  int16_t third = L.width / 3;
  for (int16_t *v : {&L.paraIndent, &L.quoteIndent, &L.itemIndent}) *v = *v < 0 ? 0 : *v > third ? third : *v;
  if (L.paraSpace < 0) L.paraSpace = 0;
  if (L.paraSpace > 1000) L.paraSpace = 1000;
  bAsc = bDesc = 0;
  int tall = 0;
  for (int r = BR_REGULAR; r <= BR_BOLD_ITALIC; r++) {
    const BookFontMetrics &m = L.font[r];
    if (m.ascent > bAsc) bAsc = m.ascent;
    if (m.lineHeight - m.ascent > bDesc) bDesc = m.lineHeight - m.ascent;
    if (m.lineHeight > tall) tall = m.lineHeight;
  }
  auto box = [&](int lineHeight, int asc, int desc, int16_t &h, int16_t &lead) {
    int v = lineHeight * L.lineSpacing / 100;
    if (v < asc + desc) v = asc + desc;
    h = v, lead = (v - asc - desc) / 2;
  };
  box(tall, bAsc, bDesc, bH, bLead);
  for (int i = 0; i < 3; i++) {
    const BookFontMetrics &m = L.font[BR_H1 + i];
    hAsc[i] = m.ascent, hDesc[i] = m.lineHeight - m.ascent;
    box(m.lineHeight, hAsc[i], hDesc[i], hH[i], hLead[i]);
  }
  headAbove = L.headAbove < 0 ? bH : L.headAbove > 1000 ? 1000 : L.headAbove;
  headBelow = L.headBelow < 0 ? bH / 2 : L.headBelow > 1000 ? 1000 : L.headBelow;
  blockSpace = L.paraSpace > bH / 2 ? L.paraSpace : bH / 2;
  sumBytes = 0, sumPages = 0;
  segValid = pageValid = false;
  return true;
}

int BookPager::Impl::adv(int role, uint32_t cp) {
  uint16_t *slot;
  if (cp >= 0x20 && cp < 0x80) {
    slot = &ascii[role * 96 + (cp - 0x20)];
    if (*slot != NO_ADV) return *slot;
  } else {
    uint32_t key = cp << 3 | (uint32_t)role, h = (key * 2654435761u) >> 24;
    slot = &hval[h];
    if (hkey[h] == key) return *slot;
    hkey[h] = key;
  }
  int a = L.advance ? L.advance(L.ctx, role, cp) : 0;
  *slot = a < 0 ? 0 : a > 4095 ? 4095 : a;
  return *slot;
}

// A font without a space still needs words apart.
int BookPager::Impl::spaceW(int role) {
  int a = adv(role, ' ');
  return a ? a : L.font[role].lineHeight / 4 + 1;
}

// ---- the contents file

void BookPager::Impl::loadToc(const char *flowPath) {
  toc.clear(), titles.clear();
  std::string path = std::string(flowPath) + ".toc";
  FILE *f = fopen(path.c_str(), "rb");
  if (!f) return;
  const size_t CH = 2048;
  char *buf = (char *)psMalloc(CH);
  char line[200];  // "<offset>\t<level>\t<title>": what's over is cut
  size_t ll = 0, n;
  bool over = false;
  auto take = [&]() {
    line[ll] = 0;
    const char *p = line;
    uint64_t off = 0;
    int digits = 0;
    for (; *p >= '0' && *p <= '9' && digits < 12; p++, digits++) off = off * 10 + (uint64_t)(*p - '0');
    if (!digits || *p != '\t' || p[1] < '1' || p[1] > '3' || p[2] != '\t') return;
    uint8_t level = p[1] - '0';
    p += 3;
    if (off >= w.size) off = w.size ? w.size - 1 : 0;
    if (toc.size() && off < toc.back().off) off = toc.back().off;  // they never go backwards
    TocRec r = {(uint32_t)off, (uint32_t)titles.size(), level};
    // The title: whole characters only, none of them a control, 120 bytes at most.
    size_t left = strlen(p), kept = 0;
    while (left) {
      uint32_t cp, len = decode((const uint8_t *)p, (uint32_t)left, cp);
      if (kept + len > TITLE_MAX) break;
      if (cp >= 0x20 && cp != 0x7F && !(cp == 0xFFFD && len == 1)) titles.insert(titles.end(), p, p + len), kept += len;
      p += len, left -= len;
    }
    if (!kept) return;
    titles.push_back(0);
    toc.push_back(r);
  };
  while (buf && toc.size() < TOC_MAX && (n = fread(buf, 1, CH, f)) > 0) {
    for (size_t i = 0; i < n && toc.size() < TOC_MAX; i++) {
      char c = buf[i];
      if (c == '\n') {
        // A line cut by the buffer may end inside a character: decode() drops the piece.
        take();
        ll = 0, over = false;
      } else if (!over) {
        if (ll + 1 < sizeof line) line[ll++] = c;
        else over = true;
      }
    }
  }
  if (ll && toc.size() < TOC_MAX) take();  // no new line at the end
  free(buf);
  fclose(f);
}

int BookPager::Impl::tocAt(uint32_t off) const {
  size_t lo = 0, hi = toc.size();  // the first entry after off
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (toc[mid].off <= off) lo = mid + 1;
    else hi = mid;
  }
  return (int)lo - 1;
}

uint32_t BookPager::Impl::chapterEnd(uint32_t off) const {
  size_t next = (size_t)(tocAt(off) + 1);
  return next < toc.size() ? toc[next].off : w.size;
}

// ---- anchors and segments

// The k-th anchor: the first block start at or after k * BOOK_STRIDE. Found
// by reading there, never by going through the file, and the same whatever
// the layout. When no block starts within SCAN bytes (a paragraph of many
// pages, a broken flow) the anchor is put inside the block, after a space
// if one is near: mid says so, and the text goes on from there as a paragraph.
uint32_t BookPager::Impl::anchor(uint32_t k, bool &mid) {
  mid = false;
  if (k == 0) return 0;
  uint64_t base = (uint64_t)k * BOOK_STRIDE;
  if (base >= w.size) return w.size;
  uint8_t buf[256];
  // Its own small reads: the window stays where the reader is.
  auto find = [&](uint32_t from, uint32_t to, uint8_t what, uint32_t &at) {
    for (uint32_t off = from; off < to;) {
      uint32_t n = to - off < sizeof buf ? to - off : (uint32_t)sizeof buf;
      if (!w.readAt(off, buf, n)) return false;
      const uint8_t *q = (const uint8_t *)memchr(buf, what, n);
      if (q) return at = off + (uint32_t)(q - buf), true;
      off += n;
    }
    return false;
  };
  uint32_t from = (uint32_t)base - 1, to = w.size - from < SCAN ? w.size : from + SCAN, at = 0;
  if (find(from, to, '\n', at)) return at + 1;
  if (w.err || to == w.size) return w.size;  // the flow's last block: it belongs to the segment before
  uint32_t end = w.size - to < SCAN_SPACE ? w.size : to + SCAN_SPACE;
  if (find(to, end, ' ', at)) return mid = at + 1 < w.size, at + 1;
  if (w.err || !w.readAt(to, buf, w.size - to < 4 ? w.size - to : 4)) return w.size;
  uint32_t skip = 0;
  while (skip < 3 && to + skip < w.size && (buf[skip] & 0xC0) == 0x80) skip++;  // not inside a character
  return mid = to + skip < w.size, to + skip;
}

// The segment holding off (off < the flow's size).
bool BookPager::Impl::findSeg(uint32_t off, Seg &s) {
  s.k = off / BOOK_STRIDE;
  s.start = anchor(s.k, s.startMid);
  if (s.start > off) s.k--, s.start = anchor(s.k, s.startMid);  // off lies before its stride's first block start
  s.limit = anchor(s.k + 1, s.limitMid);
  return !w.err;
}

bool BookPager::Impl::nextSeg(Seg &s) {
  s.k++, s.start = s.limit, s.startMid = s.limitMid;
  s.limit = anchor(s.k + 1, s.limitMid);
  return !w.err;
}

// Layout starts at an anchor knowing nothing of what came before it.
Mark BookPager::Impl::fresh(const Seg &s) const {
  Cur c;
  c.off = s.start, c.inBlock = s.startMid, c.afterHead = s.start == 0 || s.startMid;
  return markOf(c);
}

// ---- a line

// Lays out one line of the block c is inside, from c.off: appends its runs
// to pg and fills it. False: the block ended with nothing to show (what was
// read is consumed all the same). first: this would be the block's first line.
bool BookPager::Impl::buildLine(Cur &c, uint32_t limit, BookPage &pg, bool first, Item &it) {
  const uint8_t kind = c.kind;
  const bool head = kind >= BK_H1 && kind <= BK_H3;
  const size_t run0 = pg.runs.size(), arena0 = pg.arena.size();
  uint32_t off = c.off;
  int left = 0, right = 0;
  bool centre = false, just = L.justify;
  if (head) centre = L.centreHeadings, just = false;
  else if (kind == BK_CENTER) centre = true, just = false;
  else if (kind == BK_QUOTE) left = right = L.quoteIndent;
  else if (kind == BK_PARA) left = first && !c.afterHead ? L.paraIndent : 0;
  else if (kind == BK_ITEM) {
    if (first) {
      // "12. text" hangs after its own number; anything else gets a bullet
      // (unless it starts with a digit: a number of some other shape).
      uint32_t n = 0, i = 0;
      const uint8_t *p = w.at(off, n);
      if (!p) n = 0;
      if (n > limit - off) n = limit - off;
      while (i < n && i < 8 && p[i] >= '0' && p[i] <= '9') i++;
      int sw = spaceW(BR_REGULAR), mw = 0, textX = L.itemIndent;
      uint32_t mlen = 0;
      bool bullet = false;
      char mark[12];
      if (i > 0 && i + 1 < n && (p[i] == '.' || p[i] == ')') && p[i + 1] == ' ') {
        mlen = i + 1;
        memcpy(mark, p, mlen);
        bool drawn = true;
        for (uint32_t j = 0; j < mlen; j++) {
          int a = adv(BR_REGULAR, p[j]);
          if (!a) drawn = false;
          mw += a;
        }
        if (drawn) off += mlen + 1;
        else mlen = 0, mw = 0;
      } else {
        bullet = true;
        if ((mw = adv(BR_REGULAR, 0x2022))) memcpy(mark, "\xE2\x80\xA2", mlen = 3);
        else if ((mw = adv(BR_REGULAR, '-'))) mark[0] = '-', mlen = 1;
      }
      if (mlen) {
        if (textX < mw + sw) textX = mw + sw;
        if (textX > L.width / 2) textX = L.width / 2;
        int mx = textX - sw - mw;
        PageRun r = {(int16_t)(mx < 0 ? 0 : mx), (uint16_t)mw, (uint32_t)pg.arena.size(), (uint16_t)mlen, BR_REGULAR, (uint8_t)(bullet ? PR_BULLET : PR_MARKER)};
        pg.arena.insert(pg.arena.end(), mark, mark + mlen);
        pg.arena.push_back(0);
        pg.runs.push_back(r);
      }
      c.hang = (uint16_t)textX;
    }
    left = c.hang;
  }
  int avail = L.width - left - right;
  if (avail < L.width / 3) left = right = 0, avail = L.width;  // margins that leave no room: none

  bool italic = c.italic, bold = c.bold;
  uint8_t role = head ? BR_H1 + (kind - BK_H1) : (bold ? 2 : 0) | (italic ? 1 : 0);
  int x = 0;                 // the pen, from the line's left
  int pendW = 0;             // space waiting for the next character (never left at a line's end)
  bool pendStretch = false;  // a real space is in it (a no-break space alone isn't stretched)
  int gaps = 0, gapW = 0;    // the stretchable spaces placed, and their width
  int wordChars = 0;
  bool content = false, blank = false;
  bool open = false;         // a run is being filled
  uint32_t rStart = 0;
  int rX = 0;
  uint8_t rRole = 0, rFlags = 0;
  // The last place the line may end, and everything needed to go back to it.
  struct {
    bool valid, soft, open, italic, bold;
    uint32_t off, rStart;
    size_t runs, arena;
    int x, rX, gaps, gapW;
    uint8_t rRole, rFlags, role;
  } B;
  B.valid = false;
  enum { WRAP, BREAK, BLOCK } why = BLOCK;

  auto closeRun = [&]() {
    if (!open) return;
    PageRun r = {(int16_t)rX, (uint16_t)(x - rX), rStart, (uint16_t)(pg.arena.size() - rStart), rRole, rFlags};
    pg.runs.push_back(r);
    pg.arena.push_back(0);
    open = false;
  };
  auto remember = [&](uint32_t resume, bool soft) {
    B.valid = true, B.soft = soft, B.open = open, B.italic = italic, B.bold = bold;
    B.off = resume, B.rStart = rStart, B.runs = pg.runs.size(), B.arena = pg.arena.size();
    B.x = x, B.rX = rX, B.gaps = gaps, B.gapW = gapW, B.rRole = rRole, B.rFlags = rFlags, B.role = role;
  };

  for (;;) {
    uint32_t n = 0;
    const uint8_t *p = off < limit ? w.at(off, n) : nullptr;
    if (!p) break;  // the segment's end (or the card's): the block ends here
    if (n > limit - off) n = limit - off;
    uint32_t cp = p[0], len = 1;
    if (cp < 0x80) {
      if (cp == ' ') {
        closeRun();
        if (content) {
          remember(off + 1, false);
          pendW += spaceW(role), pendStretch = true;
        }
        wordChars = 0, off++;
        continue;
      }
      if (cp == '\n') {
        off++;
        break;
      }
      if (cp < 0x20 || cp == 0x7F) {
        off++;
        if (cp == FL_BREAK) {
          if (content) {
            why = BREAK;
            break;
          }
          // No text before it: the second of two breaks is a blank line, any other is nothing.
          if (c.brk && !first) {
            why = BREAK, blank = true;
            break;
          }
        } else if (cp >= FL_ITALIC_ON && cp <= FL_BOLD_OFF) {
          if (cp == FL_ITALIC_ON || cp == FL_ITALIC_OFF) italic = cp == FL_ITALIC_ON;
          else bold = cp == FL_BOLD_ON;
          uint8_t now = head ? role : (bold ? 2 : 0) | (italic ? 1 : 0);
          if (now != role) closeRun(), role = now;
        }
        continue;  // any other control byte has no business here: skipped
      }
    } else {
      len = decode(p, n, cp);
      if (cp == 0xA0) {  // a no-break space: room, but no place to break and nothing to print
        closeRun();
        if (content) pendW += spaceW(role);
        off += len;
        continue;
      }
      if (cp == 0x200B) {  // a zero-width space (between Thai words): a place to break, nothing to print
        if (wordChars && !pendW) remember(off + len, false);
        off += len;
        continue;
      }
      if (cp == 0xAD) {  // a soft hyphen: a place to break if a "-" still fits
        int hw = adv(role, '-');
        if (wordChars && !pendW && x + hw <= avail) remember(off + len, true);
        off += len;
        continue;
      }
    }
    int a = adv(role, cp);
    if (!a && !isThaiMark(cp)) {  // the font hasn't got it (a Thai mark has no width of its own)
      off += len;
      continue;
    }
    if (x + pendW + a > avail) {
      if (B.valid) {
        pg.runs.resize(B.runs), pg.arena.resize(B.arena);
        open = B.open, rStart = B.rStart, rX = B.rX, rRole = B.rRole, rFlags = B.rFlags;
        x = B.x, gaps = B.gaps, gapW = B.gapW, italic = B.italic, bold = B.bold, off = B.off;
        if (B.soft) {
          if (!open) open = true, rStart = (uint32_t)pg.arena.size(), rX = x, rRole = B.role, rFlags = 0;
          pg.arena.push_back('-');
          x += adv(B.role, '-');
          rFlags |= PR_HYPHEN;
        }
        why = WRAP;
        break;
      }
      if (content) {  // a word wider than the line: cut before this character
        why = WRAP;
        break;
      }
      off += len;  // a character wider than the line itself: left out
      continue;
    }
    if (pendW) {
      x += pendW;
      if (pendStretch) gaps++, gapW += pendW;
    }
    if (!open) open = true, rStart = (uint32_t)pg.arena.size(), rX = x, rRole = role, rFlags = pendW && pendStretch ? PR_SPACE_BEFORE : 0;
    pendW = 0, pendStretch = false;
    if (cp == 0xFFFD && len == 1) pg.arena.insert(pg.arena.end(), {'\xEF', '\xBF', '\xBD'});
    else pg.arena.insert(pg.arena.end(), (const char *)p, (const char *)p + len);
    x += a, off += len, content = true;
    // After a hyphen inside a word, after a dash: the line may end.
    if ((cp == '-' && wordChars) || cp == 0x2013 || cp == 0x2014) remember(off, false);
    wordChars++;
  }
  closeRun();
  st.bytesLaidOut += off - c.off;
  c.off = off, c.italic = italic, c.bold = bold;
  c.brk = why == BREAK && !blank;
  if (why == BLOCK) c.inBlock = false, c.italic = c.bold = false, c.brk = false, c.afterHead = head;
  if (!content && !blank) {  // why is BLOCK: nothing in the block could be shown
    pg.runs.resize(run0), pg.arena.resize(arena0);
    return false;
  }

  // The runs' places: centred, stretched to the right edge, or as they fell.
  int extra = avail - x, k = 0;
  bool stretch = just && why == WRAP && gaps > 0 && extra > 0 && extra * 2 <= gapW * 3;  // spaces up to 2.5 times their width
  for (size_t i = run0; i < pg.runs.size(); i++) {
    PageRun &r = pg.runs[i];
    if (r.flags & (PR_BULLET | PR_MARKER)) continue;
    if (r.flags & PR_SPACE_BEFORE) k++;
    r.x = (int16_t)(r.x + left + (centre ? extra / 2 : stretch ? extra * k / gaps : 0));
  }
  it.type = IT_LINE, it.kind = kind, it.first = first, it.last = why == BLOCK, it.blank = blank;
  it.firstRun = (uint32_t)run0, it.runCount = (uint16_t)(pg.runs.size() - run0);
  if (head) {
    int h = kind - BK_H1;
    it.asc = hAsc[h], it.desc = hDesc[h], it.height = hH[h], it.lead = hLead[h];
    it.above = headAbove, it.below = headBelow;
  } else {
    it.asc = bAsc, it.desc = bDesc, it.height = bH, it.lead = bLead;
    it.above = it.below = kind == BK_QUOTE || kind == BK_CENTER ? blockSpace : L.paraSpace;
  }
  return true;
}

// The next thing down the page from c: a line, a rule, or the segment's end.
void BookPager::Impl::nextItem(Cur &c, uint32_t limit, BookPage &pg, Item &it) {
  it = Item();
  bool pageBefore = false;
  for (;;) {
    bool first = false;
    if (!c.inBlock) {
      // The block's prefix. A broken flow may repeat it: the last kind wins.
      uint8_t kind = BK_PARA;
      bool rule = false;
      for (;;) {
        uint32_t n;
        const uint8_t *p = c.off < limit ? w.at(c.off, n) : nullptr;
        if (!p) return;
        uint8_t b = *p;
        if (b == FL_PAGE) pageBefore = true;
        else if (b >= FL_H1 && b <= FL_ITEM) kind = b;
        else if (b == FL_RULE) rule = true;
        else break;
        c.off++;
        if (rule) break;
      }
      if (rule) {
        uint32_t n;
        const uint8_t *p = c.off < limit ? w.at(c.off, n) : nullptr;
        if (p && *p == '\n') c.off++;  // (text after a rule's byte: a paragraph of its own)
        c.afterHead = true;
        it.type = IT_RULE, it.pageBefore = pageBefore;
        it.height = bH, it.above = it.below = blockSpace;
        return;
      }
      c.inBlock = true, c.kind = kind, c.italic = c.bold = c.brk = false, c.hang = 0;
      if (pageBefore) c.afterHead = true;
      first = true;
    }
    if (buildLine(c, limit, pg, first, it)) {
      it.pageBefore = pageBefore;
      return;
    }
  }
}

// ---- a page

// A heading about to be placed at y: do it and the start of what follows
// (two lines, or a block's only one) fit on the page? If not it starts the next.
bool BookPager::Impl::keepsWithNext(const Cur &at, uint32_t limit, BookPage &pg, int y, int prevBelow) {
  Cur t = at;
  PageSize keep = sizeOf(pg);
  int body = 0;
  for (int n = 0; n < LOOK_MAX; n++) {
    Item it;
    nextItem(t, limit, pg, it);
    cutTo(pg, keep);
    if (it.type != IT_LINE || (it.pageBefore && n)) return true;  // nothing follows on this page anyway
    int top = y + (it.first ? (prevBelow > it.above ? prevBelow : it.above) : 0);
    if (top + it.lead + it.asc + it.desc > L.height) return false;
    y = top + it.height;
    if (it.last) prevBelow = it.below;
    bool head = it.kind >= BK_H1 && it.kind <= BK_H3;
    if (!head && (++body == 2 || it.last)) return true;
  }
  return true;
}

// Fills pg with the page starting at from, in segment s. Returns where the
// next page starts (when pg.end is the segment's limit, that is the next
// segment's business: the mark returned isn't to be used).
Mark BookPager::Impl::layoutPage(const Mark &from, const Seg &s, BookPage &pg) {
  pg.clear();
  pg.start = from.off;
  Cur c = curOf(from);
  int y = 0, prevBelow = 0;
  bool ruleLast = false;
  // The line placed last, to take it back when the next one would be left alone.
  Cur lastCur = c;
  PageSize lastSize = sizeOf(pg);
  int blockLines = 0;            // of the block being placed, on this page
  bool blockStartedHere = false;
  for (;;) {
    Cur save = c;
    PageSize size = sizeOf(pg);
    Item it;
    nextItem(c, s.limit, pg, it);
    if (it.type == IT_END) break;
    bool empty = pg.lines.empty();
    if (it.pageBefore && !empty) {  // a chapter's start: on the next page
      c = save, cutTo(pg, size);
      break;
    }
    if (it.type == IT_RULE) {
      if (empty) continue;  // at a page's top: dropped
      int top = y + (prevBelow > it.above ? prevBelow : it.above);
      if (top + it.height > L.height) break;  // no room: dropped at the bottom, not carried over
      pg.rules.push_back({(int16_t)(L.width * 3 / 8), (int16_t)(top + it.height / 2), (int16_t)(L.width / 4)});
      y = top + it.height, prevBelow = it.below, ruleLast = true, blockLines = 0;
      continue;
    }
    if (it.blank && empty) {  // a page doesn't start with a blank line
      cutTo(pg, size);
      continue;
    }
    bool head = it.kind >= BK_H1 && it.kind <= BK_H3;
    int top = y + (it.first && !empty ? (prevBelow > it.above ? prevBelow : it.above) : 0);
    bool fits = top + it.lead + it.asc + it.desc <= L.height;
    if (fits && !empty && it.first) {
      if (head) {
        cutTo(pg, size);
        fits = keepsWithNext(save, s.limit, pg, y, prevBelow);
        if (fits) c = save, nextItem(c, s.limit, pg, it);  // the look-ahead used the page's vectors: the line again
      } else if (!it.last) {
        fits = top + it.height + it.lead + it.asc + it.desc <= L.height;  // not a first line alone at the bottom
      }
    }
    if (!fits && !empty) {
      c = save, cutTo(pg, size);
      // A paragraph's last line alone on the next page: the line before goes
      // with it, when that leaves no lone first line here.
      if (it.last && !it.first && !head && blockLines >= 2 && (blockLines >= 3 || !blockStartedHere)) c = lastCur, cutTo(pg, lastSize);
      break;
    }
    if (it.first) blockLines = 0, blockStartedHere = true;
    blockLines++;
    lastCur = save, lastSize = size;
    pg.lines.push_back({(int16_t)(top + it.lead + it.asc), (int16_t)top, it.height, it.runCount, it.firstRun});
    y = top + it.height, ruleLast = false;
    if (it.last) prevBelow = it.below;
  }
  if (ruleLast) pg.rules.pop_back();  // a rule with nothing under it
  pg.end = c.off;
  st.pages++;
  if (!w.err) sumBytes += pg.end - pg.start, sumPages++;
  return markOf(c);
}

// ---- moving

bool BookPager::Impl::failed() {
  page.clear();
  page.start = page.end = pos;
  segValid = pageValid = false;
  cntDone = true, cntN = -1;
  status = BP_IO;
  return false;
}

// Lays out the cached page i, and the pages after it until the one holding
// `until` (or the segment's last). The page starts found on the way are kept.
bool BookPager::Impl::showAt(size_t i, uint32_t until) {
  for (;; i++) {
    Mark next = layoutPage(cache[i], seg, page);
    if (w.err) return failed();
    if (i + 1 == cache.size() && page.end < seg.limit) {
      if (page.end <= page.start) return failed();  // can't happen: a page always takes something
      cache.push_back(next);
    }
    if (until < page.end || page.end >= seg.limit) break;
  }
  idx = i, pageValid = true, status = BP_OK;
  return true;
}

// Shows the page of the grid that holds off.
bool BookPager::Impl::show(uint32_t off) {
  if (!w.f) return false;
  if (!w.size) return status = BP_EMPTY, false;
  w.err = false;
  clearerr(w.f);
  if (off >= w.size) off = w.size - 1;
  pos = off;
  if (!segValid || off < seg.start || off >= seg.limit) {
    segValid = false;
    if (!findSeg(off, seg)) return failed();
    cache.clear();
    cache.push_back(fresh(seg));
    segValid = true;
  }
  size_t lo = 0, hi = cache.size();  // the last page start known at or before off
  while (hi - lo > 1) {
    size_t mid = (lo + hi) / 2;
    if (cache[mid].off <= off) lo = mid;
    else hi = mid;
  }
  if (!showAt(lo, off)) return false;
  restartCount();
  return true;
}

void BookPager::Impl::restartCount() {
  cntN = 0, cntDone = false;
  cntEnd = chapterEnd(page.start);
  cntSeg = seg;
  cntNeedSeg = idx + 1 >= cache.size();
  if (!cntNeedSeg) cntMark = cache[idx + 1];
  if (page.end >= cntEnd || page.end >= w.size) cntDone = true;
}

void BookPager::Impl::shut() {
  if (w.f) fclose(w.f);
  free(w.buf);
  w = Win();
  w.st = &st;
  page.clear(), tmp.clear();
  page.start = page.end = 0;
  cache.clear(), toc.clear(), titles.clear();
  segValid = pageValid = false;
  cntDone = true, cntN = -1;
  pos = 0;
  status = BP_CLOSED;
}

// ---- the public face

// The pager's own state (about 600 bytes) goes to PSRAM too.
BookPager::BookPager() {
  void *m = psMalloc(sizeof(Impl));
  if (!m) abort();  // out of every heap, as PsAlloc does
  d_ = new (m) Impl;
  d_->w.st = &d_->st;
}
BookPager::~BookPager() {
  d_->~Impl();
  free(d_);
}

bool BookPager::open(const char *flowPath, const BookLayout &layout, uint32_t position) {
  Impl &d = *d_;
  d.shut();
  if (!d.setLayout(layout)) return false;
  d.status = BP_IO;
  d.w.f = fopen(flowPath, "rb");
  if (!d.w.f) return false;
  // Left buffered: on the board an unbuffered stream reads the card a byte at
  // a time (newlib), 15 times slower; a buffered one reads a big fread() straight in.
  long size = fseek(d.w.f, 0, SEEK_END) == 0 ? ftell(d.w.f) : -1;
  d.w.buf = (uint8_t *)psMalloc(WIN);
  if (size < 0 || !d.w.buf) return d.shut(), d.status = BP_IO, false;
  d.w.size = size > 0x7FFFFFFF ? 0x7FFFFFFF : (uint32_t)size;
  d.w.filePos = NO_KEY;  // unknown: the first read seeks
  d.page.arena.reserve(4096), d.page.runs.reserve(512), d.page.lines.reserve(64);
  d.loadToc(flowPath);
  return d.show(position);
}

void BookPager::close() { d_->shut(); }
BookPagerStatus BookPager::status() const { return d_->status; }

bool BookPager::setLayout(const BookLayout &layout) {
  if (!d_->setLayout(layout)) return false;
  return d_->w.f ? d_->show(d_->pos) : true;
}

const BookPage &BookPager::page() const { return d_->page; }
uint32_t BookPager::position() const { return d_->pos; }
uint32_t BookPager::flowSize() const { return d_->w.size; }
int BookPager::permille() const { return d_->w.size ? (int)((uint64_t)d_->pos * 1000 / d_->w.size) : 0; }
bool BookPager::atStart() const { return !d_->pageValid || d_->page.start == 0; }
bool BookPager::atEnd() const { return !d_->pageValid || d_->page.end >= d_->w.size; }

bool BookPager::nextPage() {
  Impl &d = *d_;
  if (!d.pageValid || d.page.end >= d.w.size) return false;
  d.w.err = false;
  uint32_t chapter = d.chapterEnd(d.page.start);
  if (d.idx + 1 < d.cache.size()) {
    if (!d.showAt(d.idx + 1, 0)) return false;
  } else {  // the segment's last page: on to the next segment
    Seg s = d.seg;
    if (!d.nextSeg(s)) return d.failed();
    d.seg = s;
    d.cache.clear();
    d.cache.push_back(d.fresh(s));
    if (!d.showAt(0, 0)) return false;
  }
  d.pos = d.page.start;
  // The count goes on: one page fewer is left.
  if (d.cntN >= 1 && d.chapterEnd(d.page.start) == chapter && d.cntEnd == chapter) d.cntN--;
  else d.restartCount();
  return true;
}

bool BookPager::prevPage() {
  Impl &d = *d_;
  if (!d.pageValid || d.page.start == 0) return false;
  d.w.err = false;
  uint32_t chapter = d.chapterEnd(d.page.start);
  if (d.idx > 0) {
    if (!d.showAt(d.idx - 1, 0)) return false;
  } else {  // the segment's first page: the one before is the last of the segment before, laid out from its anchor
    Seg s;
    if (!d.findSeg(d.seg.start - 1, s)) return d.failed();
    d.seg = s;
    d.cache.clear();
    d.cache.push_back(d.fresh(s));
    if (!d.showAt(0, s.limit - 1)) return false;
  }
  d.pos = d.page.start;
  if (d.cntN >= 0 && d.chapterEnd(d.page.start) == chapter && d.cntEnd == chapter) d.cntN++;
  else d.restartCount();
  return true;
}

bool BookPager::gotoOffset(uint32_t offset) { return d_->show(offset); }

bool BookPager::gotoPermille(int permille) {
  if (permille < 0) permille = 0;
  if (permille > 1000) permille = 1000;
  return d_->show((uint32_t)((uint64_t)d_->w.size * (uint32_t)permille / 1000));
}

bool BookPager::gotoToc(int index) {
  if (index < 0 || (size_t)index >= d_->toc.size()) return false;
  return d_->show(d_->toc[index].off);
}

int BookPager::tocCount() const { return (int)d_->toc.size(); }
int BookPager::currentToc() const { return d_->toc.empty() ? -1 : d_->tocAt(d_->pos); }

BookTocItem BookPager::toc(int index) const {
  BookTocItem t;
  if (index < 0 || (size_t)index >= d_->toc.size()) return t;
  const TocRec &r = d_->toc[index];
  t.offset = r.off, t.level = r.level, t.title = d_->titles.data() + r.title;
  t.current = index == currentToc();
  return t;
}

const char *BookPager::chapterTitle() const { return toc(currentToc()).title; }

bool BookPager::tick() {
  Impl &d = *d_;
  if (!d.pageValid || d.cntDone) return false;
  d.w.err = false;
  bool bad = false;
  for (uint32_t done = 0; done < TICK_BYTES;) {
    if (d.cntNeedSeg) {
      if (d.cntSeg.limit >= d.w.size) {
        d.cntDone = true;
        break;
      }
      if ((bad = !d.nextSeg(d.cntSeg))) break;
      d.cntMark = d.fresh(d.cntSeg), d.cntNeedSeg = false;
    }
    if (d.cntMark.off >= d.cntEnd) {
      d.cntDone = true;
      break;
    }
    Mark next = d.layoutPage(d.cntMark, d.cntSeg, d.tmp);
    if ((bad = d.w.err || d.tmp.end <= d.tmp.start)) break;
    d.cntN++;
    done += d.tmp.end - d.tmp.start;
    if (d.tmp.end >= d.cntSeg.limit) d.cntNeedSeg = true;
    else d.cntMark = next;
  }
  if (bad) d.w.err = false, d.cntDone = true, d.cntN = -1;  // the card went: not known, and no more trying
  return !d.cntDone;
}

int BookPager::pagesLeftInChapter() const { return d_->cntDone ? d_->cntN : -1; }

int BookPager::estimatedPages() const {
  const Impl &d = *d_;
  if (!d.sumPages || !d.sumBytes) return -1;
  uint64_t n = ((uint64_t)d.w.size * d.sumPages + d.sumBytes - 1) / d.sumBytes;
  return n < 1 ? 1 : n > 1000000 ? 1000000 : (int)n;
}

int BookPager::estimatedPageNumber() const {
  const Impl &d = *d_;
  int total = estimatedPages();
  if (total < 0 || !d.pageValid) return -1;
  uint64_t n = (uint64_t)d.page.start * d.sumPages / d.sumBytes + 1;
  return n > (uint64_t)total ? total : (int)n;
}

const BookPagerStats &BookPager::stats() const { return d_->st; }
void BookPager::resetStats() { d_->st = BookPagerStats(); }
