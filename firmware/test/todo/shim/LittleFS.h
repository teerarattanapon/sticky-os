// LittleFS over a directory on the computer (TODO_FS_DIR).
#pragma once
#include <stdio.h>
#include <string>
#include "Arduino.h"

class File {
 public:
  File(FILE *f = nullptr) : f_(f) {}
  explicit operator bool() const { return f_; }
  int printf(const char *fmt, ...) { va_list a; va_start(a, fmt); int n = vfprintf(f_, fmt, a); va_end(a); return n; }
  bool available() { int c = fgetc(f_); if (c == EOF) return false; ungetc(c, f_); return true; }
  String readStringUntil(char end) {
    std::string s;
    for (int c; (c = fgetc(f_)) != EOF && c != end;) s += (char)c;
    return s;
  }
  void close() { if (f_) fclose(f_), f_ = nullptr; }
 private:
  FILE *f_;
};

struct LittleFSShim {
  std::string dir = getenv("TODO_FS_DIR") ? getenv("TODO_FS_DIR") : ".";
  std::string at(const char *p) { return dir + p; }
  File open(const char *p, const char *mode) { return File(fopen(at(p).c_str(), mode)); }
  bool rename(const char *a, const char *b) { return ::rename(at(a).c_str(), at(b).c_str()) == 0; }
  bool remove(const char *p) { return ::remove(at(p).c_str()) == 0; }
};
inline LittleFSShim LittleFS;
