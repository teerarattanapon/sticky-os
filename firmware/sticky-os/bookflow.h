// The Books app's own text format, the "flow": what every book (EPUB, MOBI,
// FB2, PDF, plain text, HTML) is turned into once, when it's first opened,
// and what the pages are then laid out from (bookpage.h). One file of UTF-8
// with a few control bytes, kept beside the book on the card, so a book
// opens at once the second time and a place in it is a byte offset that
// stays the same whatever the text size or the way the board is held.
//
// Plain C++ (no Arduino): the same code runs in the tests on a computer
// (firmware/test/ebook).
//
// The format, version FLOW_VERSION:
//
//   file    = block*
//   block   = [FL_PAGE] [kind] inline* '\n'
//   kind    = FL_H1 | FL_H2 | FL_H3 | FL_QUOTE | FL_CENTER | FL_ITEM | FL_RULE
//             (none: an ordinary paragraph)
//   inline  = UTF-8 text | FL_BREAK | FL_ITALIC_ON | FL_ITALIC_OFF | FL_BOLD_ON | FL_BOLD_OFF
//
// - Text is valid UTF-8 with no bytes below 0x20 other than the ones above;
//   spaces are single, and a block neither starts nor ends with one.
//   U+00A0 (no-break space) and U+00AD (soft hyphen: a place a word may be
//   broken) pass through. U+200B (zero-width space) is a place a line may
//   end with nothing drawn: the writer puts one between Thai words
//   (thaiwords.h), which are written without spaces.
// - FL_PAGE: this block starts a new page (a chapter's first block).
// - FL_RULE: a break in the text ("* * *", <hr>); the block has no inline.
// - FL_ITEM: a list item; the text carries its own number ("1. ") if it had
//   one, a bullet is the pager's to draw otherwise.
// - Italic and bold end with the block: the writer closes what was left on,
//   and every block starts plain.
// - An empty block is never written.
//
// Beside the flow, the table of contents: "<flow>.toc", a line an entry,
//   <offset of the block it points at>\t<level 1..3>\t<title>\n
// in the order of the book; offsets never go backwards.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string>
#include <vector>

static const int FLOW_VERSION = 2;  // 2: U+200B between Thai words

enum : uint8_t {
  FL_H1 = 0x01,
  FL_H2 = 0x02,
  FL_H3 = 0x03,  // h3 and smaller
  FL_QUOTE = 0x04,
  FL_CENTER = 0x05,
  FL_ITEM = 0x06,
  FL_RULE = 0x07,
  FL_BREAK = 0x0B,  // a line break inside a block (<br>, a verse's lines)
  FL_PAGE = 0x0C,
  FL_ITALIC_ON = 0x0E,
  FL_ITALIC_OFF = 0x0F,
  FL_BOLD_ON = 0x10,
  FL_BOLD_OFF = 0x11,
};

enum BlockKind : uint8_t { BK_PARA = 0, BK_H1 = FL_H1, BK_H2 = FL_H2, BK_H3 = FL_H3, BK_QUOTE = FL_QUOTE, BK_CENTER = FL_CENTER, BK_ITEM = FL_ITEM };

// Writes a flow and its table of contents. The converters call it with
// whatever they have: it collapses white space (spaces, tabs, new lines:
// one space), trims blocks, drops empty ones, repairs broken UTF-8 (a bad
// byte becomes U+FFFD), drops control characters and keeps italic / bold
// balanced. Text given with no block open starts a paragraph.
class FlowWriter {
 public:
  FlowWriter();
  ~FlowWriter();
  FlowWriter(const FlowWriter &) = delete;
  FlowWriter &operator=(const FlowWriter &) = delete;

  // Creates flowPath and flowPath + ".toc" (replacing them). False: can't write there.
  bool open(const char *flowPath);
  // The block written next starts a new page (a chapter). Ends the open block.
  void pageBreak();
  // Ends the open block and starts one of this kind.
  void beginBlock(BlockKind kind = BK_PARA);
  void endBlock();
  // Text of the open block, UTF-8, in pieces of any size (a character may be
  // split between two calls).
  void text(const char *utf8, size_t n);
  void text(const char *utf8);
  void italic(bool on);
  void bold(bool on);
  void lineBreak();  // nothing at a block's start
  void rule();       // a block of its own; two in a row are one
  // An entry of the table of contents, pointing at the block written next
  // (call it before that block's beginBlock). level 1..3. An empty title is
  // dropped. Several may wait for the same block (a part, then its first
  // chapter): all are written, in order (at most TOC_PENDING_MAX; beyond that
  // the last one waiting is replaced).
  void tocEntry(const char *title, int level = 1);
  // Forgets the entries waiting for the next block (they pointed at text
  // that turned out to have none, a cover page).
  void tocDropPending();
  int tocPending() const { return nPending_; }
  static const int TOC_PENDING_MAX = 8;
  // Where the block written next will start: for anchors (an EPUB's "#id").
  uint32_t offset() const;
  // Bytes of text written so far, and blocks: has the book any text at all?
  uint32_t textBytes() const { return textBytes_; }
  // Ends the open block, writes the contents and closes both files.
  // False if any write failed (a full or removed card).
  bool close();
  bool failed() const { return failed_; }

 private:
  void put(const char *p, size_t n);
  void putByte(uint8_t b);
  void flushPending();
  void codePoint(uint32_t cp);
  void emit(uint32_t cp);
  void flushThai();
  void startIfNeeded();
  FILE *flow_ = nullptr, *toc_ = nullptr;
  uint32_t written_ = 0, textBytes_ = 0;
  bool failed_ = false;
  bool open_ = false;        // a block is open
  bool started_ = false;     // its first byte (prefix) has been written
  bool pageNext_ = false, space_ = false, italic_ = false, bold_ = false, lastRule_ = false;
  bool wantItalic_ = false, wantBold_ = false;
  int breaks_ = 0;           // line breaks waiting for the text after them
  BlockKind kind_ = BK_PARA;
  std::vector<uint32_t> thai_;  // a run of Thai waiting for its word breaks
  uint32_t cp_ = 0;          // a UTF-8 character split between two text() calls
  int need_ = 0, have_ = 0;
  std::string tocTitle_[TOC_PENDING_MAX];  // entries waiting for their block
  int tocLevel_[TOC_PENDING_MAX] = {};
  int nPending_ = 0;
  char *buf_ = nullptr;      // the write buffer
  size_t bufLen_ = 0;
};

// Reading the contents file back.
struct TocEntry {
  uint32_t offset;
  uint8_t level;
  std::string title;
};
