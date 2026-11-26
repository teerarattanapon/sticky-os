#!/bin/bash
# The Books app's readers, pager and library, on this computer:
#   ./run.sh                 every test
#   ./run.sh page lib        only page_test.cpp and lib_test.cpp
#   BOOKS_DIR=~/books ./run.sh    also against real books in that folder (each test picks what it reads)
# Every *_test.cpp here is a program of its own, built with all of the
# firmware's book code (firmware/sticky-os/book*.cpp, ebook.cpp, glyphs.cpp, thaiwords.cpp;
# shim/ stands in for Arduino.h and the GFX font types). A test gets
# the fixtures folder as its first argument and exits 0 when it passed.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build
CXX=${CXX:-c++}
SRC=../../sticky-os
FLAGS=(-std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -fno-exceptions -fno-rtti -fsanitize=undefined -fno-sanitize-recover=undefined -I"$SRC" -I. -Ishim)
# Some macOS command line tools don't find libc++'s headers by themselves.
if ! echo '#include <new>' | "$CXX" -std=c++17 -x c++ -fsyntax-only - 2>/dev/null; then
  SDK=$(xcrun --show-sdk-path 2>/dev/null || true)
  [ -n "$SDK" ] && FLAGS+=(-isysroot "$SDK" -I"$SDK/usr/include/c++/v1")
fi
# The firmware's book code, built once into objects shared by the tests.
objs=()
for f in "$SRC"/book*.cpp "$SRC"/ebook.cpp "$SRC"/glyphs.cpp "$SRC"/thaiwords.cpp; do
  o=build/$(basename "$f" .cpp).o
  if [ ! -e "$o" ] || [ "$f" -nt "$o" ] || [ -n "$(find "$SRC" -maxdepth 1 -name '*.h' -newer "$o" | head -1)" ]; then
    "$CXX" "${FLAGS[@]}" -c -o "$o" "$f"
  fi
  objs+=("$o")
done
failed=0
for t in *_test.cpp; do
  name=${t%_test.cpp}
  if [ $# -gt 0 ] && [[ ! " $* " == *" $name "* ]]; then continue; fi
  "$CXX" "${FLAGS[@]}" -o "build/$name" "$t" "${objs[@]}"
  echo "== $name"
  if ! "./build/$name" "$PWD/fixtures"; then failed=$((failed + 1)); echo "== $name FAILED"; fi
done
[ "$failed" = 0 ] && echo "ebook tests: all passed" || { echo "ebook tests: $failed failed"; exit 1; }
