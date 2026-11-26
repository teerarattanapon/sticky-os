// Just enough of Arduino for todo.cpp on a computer.
#pragma once
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>

class String {
 public:
  String() {}
  String(const char *s) : s_(s ? s : "") {}
  String(const std::string &s) : s_(s) {}
  unsigned length() const { return s_.size(); }
  const char *c_str() const { return s_.c_str(); }
  char operator[](unsigned i) const { return i < s_.size() ? s_[i] : 0; }
  bool startsWith(const char *p) const { return s_.rfind(p, 0) == 0; }
  int indexOf(char c) const { size_t at = s_.find(c); return at == std::string::npos ? -1 : (int)at; }
  String substring(unsigned from) const { return from < s_.size() ? s_.substr(from) : std::string(); }
  void remove(unsigned from) { if (from < s_.size()) s_.erase(from); }
  void replace(char a, char b) { for (char &c : s_) if (c == a) c = b; }
  void trim() {
    size_t b = s_.find_first_not_of(" \t\r\n"), e = s_.find_last_not_of(" \t\r\n");
    s_ = b == std::string::npos ? std::string() : s_.substr(b, e - b + 1);
  }
  bool operator==(const char *o) const { return s_ == o; }
 private:
  std::string s_;
};

struct SerialShim {
  void println(const char *s) { printf("%s\n", s); }
  int printf(const char *f, ...) { va_list a; va_start(a, f); int n = vprintf(f, a); va_end(a); return n; }
};
inline SerialShim Serial;
