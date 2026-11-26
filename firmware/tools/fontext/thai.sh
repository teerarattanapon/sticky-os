#!/bin/bash
# The Thai letters (U+0E01..U+0E3A, U+0E3F..U+0E5B: 87 code points) for the
# board's fonts, as a third table beside each font's extra letters (glyphs.h,
# thaiIndex()). GNU FreeFont has no Thai that matches its Latin, so these
# come from Google's Noto Thai (SIL Open Font License 1.1): Noto Sans Thai for
# FreeSans, Noto Serif Thai for FreeSerif and the Books app. Thai is made at
# 4/5 of the font's size, which sets its consonants close to the Latin
# x-height and keeps the tone marks inside the line.
#   firmware/tools/fontext/thai.sh firmware/tools/fontext/noto-thai
# OUT=<folder> writes there instead of the firmware's src/fonts/thai.
set -euo pipefail
TTF=${1:?usage: thai.sh <folder with the Noto Thai .ttf files>}
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-"$HERE/../../sticky-os/src/fonts/thai"}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
${CC:-cc} ${CFLAGS:-} -O2 -o "$TMP/fontext" "$HERE/fontext.c" $(pkg-config --cflags --libs freetype2 2>/dev/null || freetype-config --cflags --libs)
mkdir -p "$OUT"
{
  for c in $(seq $((0x0E01)) $((0x0E3A))) $(seq $((0x0E3F)) $((0x0E5B))); do printf '%04X\n' "$c"; done
} > "$TMP/codes.txt"

thai() {  # <face> <latin pt> <name>
  local face=$1 pt=$2 name=$3 tpt=$(( ($2 * 4 + 2) / 5 ))
  "$TMP/fontext" "$TTF/$face.ttf" "$tpt" "$name" "$TMP/codes.txt" |
    sed -e "1s#letters beyond ASCII (glyphs.h)#Thai letters (glyphs.h, thaiIndex)#" \
        -e "2s#.*#// Google's $face at $tpt pt (for $pt pt Latin), 141 dpi, as Adafruit GFX's own fonts.#" \
        -e "3s#.*#// Noto is under the SIL Open Font License 1.1 (OFL.txt). Generated: don't edit.#" \
        -e 's#"../../glyphs.h"#"../../../glyphs.h"#' > "$OUT/$name.h"
  echo "$name: $(tail -1 "$OUT/$name.h")"
}

# The screens' fonts (sticky-os.ino, FONT_EXT).
for pt in 9 12 18; do thai NotoSansThai-Regular $pt ThaiSans$pt; done
for pt in 9 12 18 24; do thai NotoSansThai-Bold $pt ThaiSansBold$pt; done
for pt in 9 12 18 24 32 40; do thai NotoSerifThai-Regular $pt ThaiSerif$pt; done
# The Books app (bookfonts.cpp): italic takes the regular, bold italic the bold.
for pt in 10 11 12 14 16 18 21; do thai NotoSerifThai-Regular $pt ThaiBook$pt; done
for pt in 10 11 12 14 16 18 21 24 28; do thai NotoSerifThai-Bold $pt ThaiBookBold$pt; done
