#include "bookflow.h"

#include <stdlib.h>
#include <string.h>

#include "psalloc.h"
#include "thaiwords.h"

static const size_t FLOW_BUF = 8192;

FlowWriter::FlowWriter() {}
FlowWriter::~FlowWriter() {
  if (flow_) fclose(flow_);
  if (toc_) fclose(toc_);
  free(buf_);
}

bool FlowWriter::open(const char *flowPath) {
  if (flow_ || toc_) return false;
  if (!buf_) buf_ = (char *)psMalloc(FLOW_BUF);
  if (!buf_) return false;
  flow_ = fopen(flowPath, "wb");
  std::string tocPath = std::string(flowPath) + ".toc";
  toc_ = flow_ ? fopen(tocPath.c_str(), "wb") : nullptr;
  if (!flow_ || !toc_) {
    if (flow_) fclose(flow_);
    flow_ = nullptr;
    return false;
  }
  written_ = textBytes_ = 0;
  bufLen_ = 0;
  failed_ = open_ = started_ = pageNext_ = space_ = italic_ = bold_ = lastRule_ = wantItalic_ = wantBold_ = false;
  breaks_ = need_ = have_ = 0;
  tocDropPending();
  return true;
}

void FlowWriter::put(const char *p, size_t n) {
  if (!flow_) return;
  written_ += n;
  while (n) {
    size_t take = FLOW_BUF - bufLen_ < n ? FLOW_BUF - bufLen_ : n;
    memcpy(buf_ + bufLen_, p, take);
    bufLen_ += take, p += take, n -= take;
    if (bufLen_ == FLOW_BUF) {
      if (fwrite(buf_, 1, bufLen_, flow_) != bufLen_) failed_ = true;
      bufLen_ = 0;
    }
  }
}

void FlowWriter::putByte(uint8_t b) { put((const char *)&b, 1); }

// The block's first byte is about to be written: its contents entry and prefix go first.
void FlowWriter::startIfNeeded() {
  if (started_) return;
  started_ = true;
  lastRule_ = false;
  for (int i = 0; i < nPending_ && toc_; i++)
    if (fprintf(toc_, "%lu\t%d\t%s\n", (unsigned long)written_, tocLevel_[i], tocTitle_[i].c_str()) < 0) failed_ = true;
  tocDropPending();
  if (pageNext_) putByte(FL_PAGE), pageNext_ = false;
  if (kind_ != BK_PARA) putByte((uint8_t)kind_);
}

// What was asked for since the last character: line breaks, a space, a change of style.
void FlowWriter::flushPending() {
  bool first = !started_;
  startIfNeeded();
  if (!first) {
    for (; breaks_ > 0; breaks_--) putByte(FL_BREAK), space_ = false;
    // A space belongs outside the style it ends, inside the one it starts.
    if (space_ && ((italic_ && !wantItalic_) || (bold_ && !wantBold_))) {
      if (italic_ && !wantItalic_) putByte(FL_ITALIC_OFF), italic_ = false;
      if (bold_ && !wantBold_) putByte(FL_BOLD_OFF), bold_ = false;
    }
    if (space_) putByte(' ');
  }
  breaks_ = 0, space_ = false;
  if (italic_ && !wantItalic_) putByte(FL_ITALIC_OFF);
  if (bold_ && !wantBold_) putByte(FL_BOLD_OFF);
  if (!bold_ && wantBold_) putByte(FL_BOLD_ON);
  if (!italic_ && wantItalic_) putByte(FL_ITALIC_ON);
  italic_ = wantItalic_, bold_ = wantBold_;
}

void FlowWriter::codePoint(uint32_t cp) {
  if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == '\f' || cp == 0x2028 || cp == 0x2029) {
    flushThai();
    space_ = true;
    return;
  }
  if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp < 0xA0) || cp == 0xFEFF) return;  // controls, a byte order mark
  if (cp == 0xAD && (!started_ || space_ || breaks_)) return;  // a soft hyphen with no word before it
  if (isThai(cp)) {
    // A run is one style: a change of style (or a break) ends it.
    if (!thai_.empty() && (wantItalic_ != italic_ || wantBold_ != bold_ || breaks_)) flushThai();
    if (thai_.empty()) {
      if (!open_) beginBlock(BK_PARA);
      flushPending();
    }
    // A run with no space in it for pages on end: broken in pieces, so the
    // memory the search takes stays small.
    if (thai_.size() >= 1024 && !isThaiMark(cp)) flushThai();
    thai_.push_back(cp);
    return;
  }
  flushThai();
  if (!open_) beginBlock(BK_PARA);
  flushPending();
  emit(cp);
}

void FlowWriter::emit(uint32_t cp) {
  char out[4];
  int n;
  if (cp < 0x80) out[0] = cp, n = 1;
  else if (cp < 0x800) out[0] = 0xC0 | cp >> 6, out[1] = 0x80 | (cp & 0x3F), n = 2;
  else if (cp < 0x10000) out[0] = 0xE0 | cp >> 12, out[1] = 0x80 | (cp >> 6 & 0x3F), out[2] = 0x80 | (cp & 0x3F), n = 3;
  else out[0] = 0xF0 | cp >> 18, out[1] = 0x80 | (cp >> 12 & 0x3F), out[2] = 0x80 | (cp >> 6 & 0x3F), out[3] = 0x80 | (cp & 0x3F), n = 4;
  put(out, n);
  textBytes_ += n;
}

// Writes the Thai run waiting, with a U+200B at each place between two words.
// Not before ๆ (repeat) or ฯ (abbreviation): they belong to the word before.
void FlowWriter::flushThai() {
  if (thai_.empty()) return;
  std::vector<size_t> breaks = thaiBreaks(thai_);
  size_t b = 0;
  for (size_t i = 0; i < thai_.size(); i++) {
    if (b < breaks.size() && breaks[b] == i) {
      b++;
      if (thai_[i] != 0x0E46 && thai_[i] != 0x0E2F) emit(0x200B);
    }
    emit(thai_[i]);
  }
  thai_.clear();
}

void FlowWriter::text(const char *utf8) { text(utf8, strlen(utf8)); }

void FlowWriter::text(const char *utf8, size_t n) {
  for (size_t i = 0; i < n; i++) {
    uint8_t c = utf8[i];
    if (need_) {
      if ((c & 0xC0) == 0x80) {
        cp_ = cp_ << 6 | (c & 0x3F);
        if (++have_ == need_) {
          static const uint32_t least[] = {0, 0x80, 0x800, 0x10000};
          bool bad = cp_ < least[need_] || cp_ > 0x10FFFF || (cp_ >= 0xD800 && cp_ <= 0xDFFF);
          need_ = 0;
          codePoint(bad ? 0xFFFD : cp_);
        }
        continue;
      }
      need_ = 0;  // cut short: one replacement, then this byte on its own
      codePoint(0xFFFD);
    }
    if (c < 0x80) codePoint(c);
    else if (c >= 0xC2 && c <= 0xDF) cp_ = c & 0x1F, need_ = 1, have_ = 0;
    else if (c >= 0xE0 && c <= 0xEF) cp_ = c & 0x0F, need_ = 2, have_ = 0;
    else if (c >= 0xF0 && c <= 0xF4) cp_ = c & 0x07, need_ = 3, have_ = 0;
    else codePoint(0xFFFD);
  }
}

void FlowWriter::beginBlock(BlockKind kind) {
  endBlock();
  open_ = true;
  started_ = false;
  kind_ = kind;
}

void FlowWriter::endBlock() {
  if (need_) need_ = 0, codePoint(0xFFFD);  // a character cut off at the block's end
  flushThai();
  if (open_ && started_) {
    if (italic_) putByte(FL_ITALIC_OFF);
    if (bold_) putByte(FL_BOLD_OFF);
    putByte('\n');
  }
  open_ = started_ = space_ = italic_ = bold_ = wantItalic_ = wantBold_ = false;
  breaks_ = 0;
}

void FlowWriter::pageBreak() {
  endBlock();
  pageNext_ = true;
}

void FlowWriter::italic(bool on) { wantItalic_ = on; }
void FlowWriter::bold(bool on) { wantBold_ = on; }

void FlowWriter::lineBreak() {
  if (need_) need_ = 0, codePoint(0xFFFD);
  flushThai();
  if (open_ && started_ && breaks_ < 2) breaks_++;
  space_ = false;
}

void FlowWriter::rule() {
  endBlock();
  if (lastRule_ || !written_) return;  // not twice, and not before any text
  kind_ = BK_PARA;
  started_ = false;
  startIfNeeded();
  putByte(FL_RULE);
  putByte('\n');
  started_ = false;
  lastRule_ = true;
}

void FlowWriter::tocEntry(const char *title, int level) {
  endBlock();
  // The title on one line, trimmed, without controls.
  std::string t;
  bool gap = false;
  for (const char *p = title ? title : ""; *p; p++) {
    uint8_t c = *p;
    if (c <= 0x20 || c == 0x7F) {
      gap = t.size() > 0;
      continue;
    }
    if (gap) t += ' ', gap = false;
    t += (char)c;
  }
  if (t.empty()) return;
  if (t.size() > 200) {
    size_t cut = 200;
    while (cut > 0 && ((uint8_t)t[cut] & 0xC0) == 0x80) cut--;  // not inside a character
    t.resize(cut);
  }
  int i = nPending_ < TOC_PENDING_MAX ? nPending_++ : TOC_PENDING_MAX - 1;
  tocTitle_[i] = t;
  tocLevel_[i] = level < 1 ? 1 : level > 3 ? 3 : level;
}

void FlowWriter::tocDropPending() {
  for (int i = 0; i < nPending_; i++) tocTitle_[i].clear();
  nPending_ = 0;
}

uint32_t FlowWriter::offset() const {
  const_cast<FlowWriter *>(this)->flushThai();  // the Thai waiting comes before the next block
  return written_;
}

bool FlowWriter::close() {
  if (!flow_) return false;
  endBlock();
  if (bufLen_ && fwrite(buf_, 1, bufLen_, flow_) != bufLen_) failed_ = true;
  bufLen_ = 0;
  if (fclose(flow_) != 0) failed_ = true;
  if (toc_ && fclose(toc_) != 0) failed_ = true;
  flow_ = toc_ = nullptr;
  return !failed_;
}
