#!/bin/sh
# The tag readers (lib/rbcodec/metadata) against damaged files, under ASan
# and UBSan: every format the generators and afconvert can make, mutated
# the same way each run, plus the files in meta/ that once hung or crashed
# them. A divide by zero is a crash on the R1 (MIPS traps it).
#
#   run-meta-fuzz.sh [mutants a seed]      default 4000
set -eu
test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo=$(CDPATH= cd -- "$test_dir/../../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/sloppy-meta.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work/bin" "$work/build" "$work/seeds" "$work/gen"
# Xcode's ASan runtime: the Command Line Tools' hangs before main on macOS 27
[ -d /Applications/Xcode.app ] && export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
unset SDKROOT || true
san="-fsanitize=address,undefined -fno-sanitize-recover=undefined -g"
printf '#!/bin/sh\nexec clang -Wno-everything %s -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -I"%s/lib/skin_parser" "$@"\n' "$san" "$repo" > "$work/bin/gcc-15"
printf '#!/bin/sh\nexec clang -E "$@"\n' > "$work/bin/cpp-15"
printf '#!/bin/sh\nexec ar "$@"\n' > "$work/bin/gcc-ar-15"
printf '#!/bin/sh\n[ "$1" = --version ] && echo 2.30.0\nexit 0\n' > "$work/bin/sdl2-config"
chmod +x "$work/bin/"*
export PATH="$work/bin:$PATH"
B="$work/build"
# the database tool's objects (its own link is not needed, and fails under ASan)
(cd "$B" && "$repo/tools/configure" --target=hibyr1 --type=d >configure.log 2>&1 && make -j8 >make.log 2>&1) || true
clang $san -O1 -w -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -std=gnu99 \
    -DROCKBOX -DMEMORYSIZE=12 -DHIBY_R1 -DTARGET_ID=124 '-DTARGET_NAME="hibyr1"' -DAPPLICATION -D__PCTOOL__ -DDBTOOL -DDEBUG \
    -I"$B" -I"$B/lang" -I"$repo/firmware/target/hosted/sdl/database" -I"$repo/firmware/target/hosted/sdl" \
    -I"$repo/firmware/target/hosted" -I"$repo/lib/fixedpoint" -I"$repo/apps/gui" -I"$repo/firmware/kernel/include" \
    -I"$repo/firmware/export" -I"$repo/firmware/include" -I"$repo/apps" -I"$repo/apps/recorder" -I"$repo/lib/rbcodec" \
    -I"$repo/lib/rbcodec/metadata" -I"$repo/lib/rbcodec/dsp" -I"$repo/tools/database" -I"$repo/lib/skin_parser" \
    -I"$repo/apps/sloppy" -I"$repo/lib/lvgl" -I"$repo/uisimulator/sdl" \
    -c "$test_dir/meta_fuzz.c" -o "$B/meta_fuzz.o"
objs=$(cd "$B" && find apps firmware lib -name '*.o' ! -path 'lib/fixedpoint/*' ! -name database.o ! -name main.o | sed "s|^|$B/|")
clang $san -o "$B/meta-fuzz" "$B/meta_fuzz.o" $objs "$B/lib/libfixedpoint.a"

# the seeds: one file of each kind
S="$work/seeds"
python3 -I "$test_dir/gen_dsd.py" "$work/gen" >/dev/null
python3 -I "$test_dir/gen_midi.py" "$work/gen" >/dev/null
cp "$work/gen/tone64.dsf" "$work/gen/arp.mid" "$work/gen/tone440.rmi" "$test_dir/tone.flac" "$test_dir/tone-flac.ogg" \
   "$test_dir/tone-opus.ogg" "$test_dir/wvdsd/tone64-fast.wv" "$test_dir/wvdsd/mono64-fast.wv" "$S/"
python3 -I - "$test_dir" "$S" <<'PY'
import sys, os, importlib.util, wave, struct, math
spec = importlib.util.spec_from_file_location('g', sys.argv[1] + '/gen_library.py')
g = importlib.util.module_from_spec(spec); spec.loader.exec_module(g)
g.song(os.path.join(sys.argv[2], 'id3.mp3'), 'Title', 'Artist', 'Album', 'AA', 3, 1, 1999, 'Rock')
g.flac(os.path.join(sys.argv[2], 'cue.flac'), ['TITLE=Inside', 'ARTIST=Cue Artist', 'CUESHEET=' + g.SHEET % ('Sheet inside', 'Inside.flac')])
w = wave.open(os.path.join(sys.argv[2], 't.wav'), 'wb'); w.setnchannels(2); w.setsampwidth(2); w.setframerate(44100)
w.writeframes(b''.join(struct.pack('<hh', int(8000 * math.sin(i * 0.06)), 0) for i in range(22050))); w.close()
PY
afconvert -f m4af -d aac "$S/t.wav" "$S/aac.m4a"
afconvert -f m4af -d alac "$S/t.wav" "$S/alac.m4a"
afconvert -f AIFF -d BEI16 "$S/t.wav" "$S/t.aiff"

export ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1
status=0
mkdir -p "$work/card"; cd "$work/card"
"$B/meta-fuzz" -1 "$test_dir"/meta/* >/dev/null 2>"$work/err" || { echo "FAIL: a file in meta/"; tail -20 "$work/err"; status=1; }
for s in "$S"/*; do
    if "$B/meta-fuzz" "${1:-4000}" "$s" >/dev/null 2>"$work/err"; then echo "ok  $(basename "$s"): ${1:-4000} damaged copies read"
    else echo "FAIL $(basename "$s"):"; grep -m3 -E 'ERROR: AddressSanitizer|runtime error|HANG|SUMMARY' "$work/err"; status=1; fi
done
exit $status
