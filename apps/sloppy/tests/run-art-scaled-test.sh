#!/bin/sh
# The live tiles' covers made on a thread (art_scaled_test.c): builds it with
# the harness's include flags and runs it on an empty card folder. With
# ASAN=1, under AddressSanitizer.
set -eu
test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo=$(CDPATH= cd -- "$test_dir/../../.." && pwd)
support=${SLOPPY_SUPPORT:-$repo/../build-support}
work=$(mktemp -d "${TMPDIR:-/tmp}/sloppy-scaled.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work/root/.rockbox"
flags=$(python3 - "$support" "$repo" <<'PY'
import re, shlex, sys
from pathlib import Path
support, repo = Path(sys.argv[1]), sys.argv[2]
line = next(l for l in (support / 'compile-sloppy.log').read_text().splitlines() if 'gcc-15 -I' in l)
# the log was made in an older checkout: its folders, whatever they were, as the paths below expect
line = re.sub(r'/[^\s]*/work/sloppyos', '/tmp/sloppyos-r1', line)
line = re.sub(r'/[^\s]*/work/build-support', '/tmp/sloppy-build-r1', line)
a = shlex.split(line); a = a[1:a.index('-c')]
a = [x for x in a if x != '-Wimplicit-fallthrough=0']
a += ['-I' + str(support / 'build-sim'), '-I' + repo + '/firmware/include', '-I' + repo + '/firmware/kernel/include',
      '-I' + repo + '/firmware/export', '-I' + repo + '/firmware', '-I' + repo + '/firmware/target/hosted/sdl',
      '-I' + repo + '/firmware/target/hosted', '-I' + repo + '/firmware/target/hosted/hiby/r1', '-I' + repo + '/uisimulator/common',
      '-I' + repo + '/apps', '-I' + repo + '/apps/recorder', '-I' + repo + '/apps/gui', '-I' + repo + '/lib/skin_parser', '-I' + repo + '/apps/sloppy',
      '-I' + repo + '/lib/rbcodec/metadata', '-I' + repo + '/lib/rbcodec', '-I' + repo + '/lib/rbcodec/dsp', '-I' + repo + '/lib/lvgl',
      '-I' + str(support / 'build-sim/lib/lvgl'), '-O1', '-U_FORTIFY_SOURCE', '-D_FORTIFY_SOURCE=0', '-Wno-incompatible-function-pointer-types']
print(shlex.join(a))
PY
)
san=""
[ "${ASAN:-0}" = 1 ] && san="-fsanitize=address -g"
eval "clang $flags $san \"$test_dir/art_scaled_test.c\" -o \"$work/art-scaled-test\"" 2> "$work/build.log" || { grep -E "error" "$work/build.log" | head -20; exit 1; }
"$work/art-scaled-test" "$work/root"
