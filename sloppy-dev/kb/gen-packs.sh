#!/bin/sh
# GabagoolOS letter packs (apps/sloppy/fonts/packs/*.slf), read from the
# card by sloppy_xfont.inc the first time a word needs them.
#   sh sloppy-dev/kb/gen-packs.sh <folder with the fonts below>
# All under the SIL Open Font License 1.1:
#   Noto Sans SC and KR: https://github.com/notofonts/noto-cjk/raw/main/Sans/SubsetOTF/{SC,KR}/NotoSans{SC,KR}-{Regular,Medium}.otf
#     and its licence, https://github.com/notofonts/noto-cjk/raw/main/Sans/LICENSE, saved as LICENSE
#   Noto Sans Thai: NotoSansThai-{Regular,SemiBold}.ttf (hinted) and OFL.txt, saved as
#     OFL-NotoSansThai.txt, from https://github.com/notofonts/thai/releases (NotoSansThai-v2.002.zip)
# Run gen-pinyin.py, gen-korean.py and gen-thai.py first: they write the code point lists.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
NOTO=$1
OUT=$ROOT/apps/sloppy/fonts/packs
BIN=${TMPDIR:-/tmp}/slfpack
swiftc -O "$ROOT/sloppy-dev/slfpack.swift" -o "$BIN"
mkdir -p "$OUT"
# Chinese: 16 px from the regular weight (body text), 20 and 24 from medium (titles)
"$BIN" "$NOTO/NotoSansSC-Regular.otf" 16 "$HERE/cjk-codepoints.txt" "$OUT/cjk16.slf"
"$BIN" "$NOTO/NotoSansSC-Regular.otf" 16 "$HERE/cjkt-codepoints.txt" "$OUT/cjkt16.slf"
for px in 20 24; do
    "$BIN" "$NOTO/NotoSansSC-Medium.otf" $px "$HERE/cjk-codepoints.txt" "$OUT/cjk$px.slf"
    "$BIN" "$NOTO/NotoSansSC-Medium.otf" $px "$HERE/cjkt-codepoints.txt" "$OUT/cjkt$px.slf"
done
# Korean: as Chinese (the same sizes, weights and family)
"$BIN" "$NOTO/NotoSansKR-Regular.otf" 16 "$HERE/kor-codepoints.txt" "$OUT/kor16.slf"
"$BIN" "$NOTO/NotoSansKR-Regular.otf" 16 "$HERE/korx-codepoints.txt" "$OUT/korx16.slf"
for px in 20 24; do
    "$BIN" "$NOTO/NotoSansKR-Medium.otf" $px "$HERE/kor-codepoints.txt" "$OUT/kor$px.slf"
    "$BIN" "$NOTO/NotoSansKR-Medium.otf" $px "$HERE/korx-codepoints.txt" "$OUT/korx$px.slf"
done
# Thai: for the Arabic fonts' sizes and Source Sans's weights, as Cyrillic, its marks shaped in
# context. Noto Sans Thai is drawn to Noto Sans's x-height (0.536 em), Source Sans's is 0.486:
# nine tenths of the size sits it level, and keeps a tone over a vowel inside the line
for s in 16:14 18:16; do "$BIN" "$NOTO/NotoSansThai-Regular.ttf" ${s#*:} "$HERE/thai-codepoints.txt" "$OUT/th${s%:*}.slf"; done
for s in 21:19 23:21 26:23 28:25 30:27; do "$BIN" "$NOTO/NotoSansThai-SemiBold.ttf" ${s#*:} "$HERE/thai-codepoints.txt" "$OUT/th${s%:*}.slf"; done
# Cyrillic: Source Sans 3 itself, at the Arabic fonts' sizes
printf '400-45f\n490-491\n' > "$HERE/cyr-codepoints.txt"
for px in 16 18; do "$BIN" "$ROOT/sloppy-dev/fonts-src/SourceSans3-Regular.ttf" $px "$HERE/cyr-codepoints.txt" "$OUT/cyr$px.slf"; done
for px in 21 23 26 28 30; do "$BIN" "$ROOT/sloppy-dev/fonts-src/SourceSans3-Semibold.ttf" $px "$HERE/cyr-codepoints.txt" "$OUT/cyr$px.slf"; done
cp "$NOTO/LICENSE" "$OUT/OFL-NotoSansSC.txt" 2>/dev/null || cp "$NOTO/../LICENSE" "$OUT/OFL-NotoSansSC.txt" 2>/dev/null || true
cp "$OUT/OFL-NotoSansSC.txt" "$OUT/OFL-NotoSansKR.txt"     # one licence for the whole Noto CJK family
cp "$NOTO/OFL-NotoSansThai.txt" "$OUT/OFL-NotoSansThai.txt" 2>/dev/null || true
ls -l "$OUT"
