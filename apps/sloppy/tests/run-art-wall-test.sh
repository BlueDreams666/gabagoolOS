#!/bin/sh
# "Set as wallpaper"'s maker with the real stb_image (art_wall_test.c):
# makes its test pictures, builds it with the harness's include flags, runs it.
set -eu
test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo=$(CDPATH= cd -- "$test_dir/../../.." && pwd)
support=${SLOPPY_SUPPORT:-$repo/../build-support}
work=$(mktemp -d "${TMPDIR:-/tmp}/sloppy-wall.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work/root/pics" "$work/root/.rockbox"
python3 - "$work/root/pics" <<'PY'
import struct, sys, zlib
d = sys.argv[1]
def png(name, w, h, px):
    raw = b''.join(b'\0' + bytes(c for x in range(w) for c in px(x, y)) for y in range(h))
    def chunk(t, b): return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
    open(f'{d}/{name}', 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                                    + chunk(b'IDAT', zlib.compress(raw, 1)) + chunk(b'IEND', b''))
png('wide.png', 1600, 900, lambda x, y: (230, 20, 20) if 530 <= x < 1070 else (20, 20, 230))
png('tall.png', 900, 3000, lambda x, y: (20, 230, 20) if 750 <= y < 2250 else (20, 20, 230))
png('small.png', 120, 90, lambda x, y: (30, 30, 30) if x < 60 else (220, 220, 220))
png('white.png', 400, 300, lambda x, y: (255, 255, 255))
open(f'{d}/notes.txt', 'w').write('not a picture\n')
PY
sips -s format jpeg "$test_dir/../wallpapers/pillars.png" --out "$work/root/pics/photo.jpg" >/dev/null
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
eval "clang $flags \"$test_dir/art_wall_test.c\" -o \"$work/art-wall-test\"" 2> "$work/build.log" || { grep -E "error" "$work/build.log" | head -20; exit 1; }
"$work/art-wall-test" "$work/root"
