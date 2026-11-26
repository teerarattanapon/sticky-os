// Thai in the Books app: the flow writer puts a U+200B between Thai words
// (thaiwords.h), the pager breaks lines only there, the reading fonts carry
// the Thai letters, and a tone mark after an upper vowel is drawn above it.
//   THAI_PNG=/tmp/thai.png ./run.sh thai    also draws a page, to look at
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "bookflow.h"
#include "bookfonts.h"
#include "bookpage.h"
#include "gfxdraw.h"
#include "thaiwords.h"

static int failures = 0;
#define CHECK(c)                                                  \
  do {                                                            \
    if (!(c)) printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #c), failures++; \
  } while (0)

static const char *ZWSP = "\xE2\x80\x8B";

static std::string readAll(const char *path) {
  std::string s;
  FILE *f = fopen(path, "rb");
  if (!f) return s;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
  fclose(f);
  return s;
}

static std::string withBars(std::string s) {
  for (size_t at; (at = s.find(ZWSP)) != std::string::npos;) s.replace(at, 3, "|");
  return s;
}

static int fontAdvance(void *, int role, uint32_t cp) {
  const BookFont *f = role >= BR_H1 ? bookHeadingFont(4, role - BR_H1 + 1) : bookFont(4, role);
  return bookAdvance(f->base, f->ext, cp);
}

int main(int, char **) {
  const char *flow = "build/thai.flow";

  // The writer: word breaks inside Thai, none next to a space, none before ๆ.
  {
    FlowWriter w;
    CHECK(w.open(flow));
    w.beginBlock();
    w.text("ภาษาไทยเป็นภาษาที่สวยงาม ");
    w.text("เด็กๆ ไปโรงเรียน");
    w.italic(true);
    w.text("ทุกวัน");
    w.italic(false);
    CHECK(w.close());
    std::string s = withBars(readAll(flow));
    printf("  flow: %s", s.c_str());
    CHECK(s.find("ภาษา|ไทย|เป็น|ภาษา|ที่|สวย|งาม เด็กๆ ไป|โรงเรียน") != std::string::npos);
    CHECK(s.find("|ๆ") == std::string::npos);
    CHECK(s.find("\x0Eทุก|วัน\x0F") != std::string::npos);  // italic run on its own
  }

  // The fonts have the letters, and marks have no width.
  const BookFont *body = bookFont(4, BOOK_REGULAR);
  CHECK(bookThaiFor(body->base) != nullptr);
  CHECK(bookThaiFor(bookFont(4, BOOK_BOLD_ITALIC)->base) != nullptr);
  CHECK(bookAdvance(body->base, body->ext, 0x0E01) > 0);   // ก
  CHECK(bookAdvance(body->base, body->ext, 0x0E48) == 0);  // ่
  CHECK(drawable(0x0E01) && drawable(0x0E5B) && !drawable(0x0E3B));

  // A tone mark over an upper vowel is lifted clear of it; over a short letter it isn't.
  {
    const ExtFont *t = bookThaiFor(body->base);
    ThaiStack st;
    const ExtGlyph &tho = t->glyph[thaiIndex(0x0E17)], &ii = t->glyph[thaiIndex(0x0E35)], &ek = t->glyph[thaiIndex(0x0E48)];
    CHECK(st.raise(0x0E17, tho, 0x0E35, nullptr, body->base->yAdvance) == 0);
    CHECK(st.raise(0x0E35, ii, 0x0E48, nullptr, body->base->yAdvance) == 0);
    int lift = st.raise(0x0E48, ek, 0, nullptr, body->base->yAdvance);
    int eeTop = -ii.yOffset, ekBottom = -ek.yOffset - ek.height + lift;
    printf("  ที่: tone mark lifted %d px (its bottom %d, the vowel's top %d)\n", lift, ekBottom, eeTop);
    CHECK(lift > 0 && ekBottom > eeTop);
    ThaiStack plain;
    plain.raise(0x0E19, t->glyph[thaiIndex(0x0E19)], 0x0E48, nullptr, body->base->yAdvance);  // น
    CHECK(plain.raise(0x0E48, ek, 0, nullptr, body->base->yAdvance) == 0);
  }

  // The pager: a long Thai paragraph breaks between words, never inside one.
  {
    FlowWriter w;
    CHECK(w.open(flow));
    w.beginBlock();
    const char *para = "ประเทศไทยมีประวัติศาสตร์ยาวนานและวัฒนธรรมที่หลากหลาย คนไทยส่วนใหญ่นับถือศาสนาพุทธ "
                       "อาหารไทยมีชื่อเสียงไปทั่วโลกเพราะรสชาติที่กลมกล่อม น้ำพริกและต้มยำกุ้งเป็นที่รู้จักกันดี";
    w.text(para);
    CHECK(w.close());
    std::string words = readAll(flow);

    BookLayout l;
    l.width = 300, l.height = 600, l.justify = true;
    for (int r = 0; r < BR_COUNT; r++) {
      const BookFont *f = r >= BR_H1 ? bookHeadingFont(4, r - BR_H1 + 1) : bookFont(4, r);
      l.font[r].lineHeight = f->yAdvance, l.font[r].ascent = f->ascent;
    }
    l.advance = fontAdvance;
    BookPager p;
    CHECK(p.open(flow, l, 0));
    const BookPage &pg = p.page();
    CHECK(pg.lines.size() >= 4);
    GfxFrame frame(l.width + 20, l.height);
    // Flow text without control bytes or ZWSP, plus the offsets where a
    // line may end (a ZWSP or a space follows).
    std::string plain;
    std::vector<bool> canEnd;
    for (size_t i = 0; i < words.size();) {
      if (words.compare(i, 3, ZWSP) == 0) {
        if (!canEnd.empty()) canEnd.back() = true;
        i += 3;
        continue;
      }
      unsigned char c = words[i];
      if (c < 0x20) { i++; continue; }
      if (c == ' ' && !canEnd.empty()) canEnd.back() = true;
      plain += words[i++];
      canEnd.push_back(false);
    }
    auto strip = [](std::string s) {
      for (size_t k; (k = s.find(ZWSP)) != std::string::npos;) s.erase(k, 3);
      return s;
    };
    std::string all;
    size_t cursor = 0;
    for (const PageLine &ln : pg.lines) {
      std::string line, last;
      for (int i = 0; i < ln.runCount; i++) {
        const PageRun &r = pg.runs[ln.firstRun + i];
        line += pg.text(r);
        last = strip(pg.text(r));
        frame.drawText(10 + r.x, ln.baseline, body, pg.text(r));
      }
      printf("  | %s\n", line.c_str());
      // Every line but the last ends where the flow has a break or a space.
      size_t at = last.empty() ? std::string::npos : plain.find(last, cursor);
      CHECK(at != std::string::npos);
      if (at != std::string::npos) {
        cursor = at + last.size();
        if (&ln != &pg.lines.back()) CHECK(canEnd[cursor - 1]);
      }
      all += line;
    }
    CHECK(all.find("ประเทศ") != std::string::npos);
    if (const char *png = getenv("THAI_PNG")) {
      CHECK(frame.savePng(png));
      printf("  drawn: %s\n", png);
    }
  }

  if (failures) printf("thai: %d failed\n", failures);
  else printf("thai: ok\n");
  return failures ? 1 : 0;
}
