#!/bin/sh
# The library scan on a generated card, end to end: Rockbox's tag database
# (built as the database tool is) scans it, then GabagoolOS's loader reads
# it; every file must come out as a song. Then songs are added, deleted and
# retagged and an update (not a rebuild) must find exactly that.
#
#   run-scan-test.sh [songs ...]      default: 50000 70000
#   SCAN_UNICODE=1 ...                 also the names that break naive code
#   SCAN_KEEP=<dir>                    keep the cards and the build there
set -eu
test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo=$(CDPATH= cd -- "$test_dir/../../.." && pwd)
work=${SCAN_KEEP:-$(mktemp -d "${TMPDIR:-/tmp}/sloppy-scan.XXXXXX")}
[ -n "${SCAN_KEEP:-}" ] || trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work/bin" "$work/build"

# configure asks for Homebrew's gcc-15; on a Mac without it, clang stands in
for t in gcc-15 cpp-15 gcc-ar-15; do
    command -v $t >/dev/null 2>&1 && continue
    case $t in
        gcc-15) printf '#!/bin/sh\nexec clang -Wno-everything -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -I"%s/lib/skin_parser" "$@"\n' "$repo" ;;
        cpp-15) printf '#!/bin/sh\nexec clang -E "$@"\n' ;;
        gcc-ar-15) printf '#!/bin/sh\nexec ar "$@"\n' ;;
    esac > "$work/bin/$t"
    chmod +x "$work/bin/$t"
done
# the tool needs no SDL; configure only asks for its version
printf '#!/bin/sh\n[ "$1" = --version ] && echo 2.30.0\nexit 0\n' > "$work/bin/sdl2-config"; chmod +x "$work/bin/sdl2-config"
export PATH="$work/bin:$PATH"
B="$work/build"
if [ ! -f "$B/database.hibyr1" ]; then
    (cd "$B" && "$repo/tools/configure" --target=hibyr1 --type=d >configure.log 2>&1 && make -j8 >make.log 2>&1) \
        || { tail -20 "$B/make.log"; exit 1; }
fi
# the tool's objects, with scan_test.c in place of its main
clang -O1 -g -w -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -std=gnu99 \
    -DROCKBOX -DMEMORYSIZE=12 -DHIBY_R1 -DTARGET_ID=124 '-DTARGET_NAME="hibyr1"' -DAPPLICATION -D__PCTOOL__ -DDBTOOL -DDEBUG \
    -I"$B" -I"$B/lang" -I"$repo/firmware/target/hosted/sdl/database" -I"$repo/firmware/target/hosted/sdl" \
    -I"$repo/firmware/target/hosted" -I"$repo/lib/fixedpoint" -I"$repo/apps/gui" -I"$repo/firmware/kernel/include" \
    -I"$repo/firmware/export" -I"$repo/firmware/include" -I"$repo/apps" -I"$repo/apps/recorder" -I"$repo/lib/rbcodec" \
    -I"$repo/lib/rbcodec/metadata" -I"$repo/lib/rbcodec/dsp" -I"$repo/tools/database" -I"$repo/lib/skin_parser" \
    -I"$repo/apps/sloppy" -I"$repo/lib/lvgl" -I"$repo/uisimulator/sdl" \
    -c "$test_dir/scan_test.c" -o "$B/scan_test.o"
objs=$(cd "$B" && find apps firmware lib -name '*.o' ! -path 'lib/fixedpoint/*' | sed "s|^|$B/|")
clang -o "$B/scan-test" "$B/scan_test.o" $objs "$B/lib/libfixedpoint.a"
clang -dynamiclib -o "$B/fail_master_open.dylib" "$test_dir/fail_master_open.c"

status=0
for n in ${*:-50000 70000}; do
    card="$work/card-$n"
    rm -rf "$card"; mkdir -p "$card"
    t0=$(date +%s)
    files=$(python3 -I "$test_dir/gen_library.py" "$card" "$n" ${SCAN_UNICODE:+--unicode})
    echo "== $n songs asked for: $files files written in $(( $(date +%s) - t0 )) s"
    "$B/scan-test" "$card" || status=1
    # the start-up check with nothing changed must leave the database alone
    "$B/scan-test" "$card" --same || status=1
    # the card changes: 100 songs added, 100 deleted, one retagged
    GEN="$test_dir/gen_library.py" python3 -I - "$card" <<'EOF'
import os, sys, glob, importlib.util
card = sys.argv[1]
spec = importlib.util.spec_from_file_location('g', os.environ['GEN'])
g = importlib.util.module_from_spec(spec); spec.loader.exec_module(g)
d = os.path.join(card, 'Music', 'New Artist', 'New Album'); os.makedirs(d, exist_ok=True)
for k in range(100):
    g.song(os.path.join(d, '%03d New.mp3' % k), 'New %d' % k, 'New Artist', 'New Album', 'New Artist', k + 1, 1, 2026, 'Pop')
gone = sorted(glob.glob(os.path.join(card, 'Music', 'Artist 0000[1-9]', '*', '*.mp3')))[:100]
for p in gone: os.remove(p)
p = os.path.join(card, 'Music', 'Artist 00000', 'Album 000000', '01 Song 000000.mp3')
g.song(p, 'Retagged', 'Artist 00000', 'Album 000000', 'Artist 00000', 1, 1, 1960, 'Rock')
os.utime(p, (1, 2000000000))   # a newer file time, as a tag editor leaves
EOF
    # a commit that cannot open the master index (a card going away under it,
    # as at a USB hand-over) must fail and keep the database: it deleted every
    # database file, and Tai's R1 lost its library that way (3 Nov)
    # (on a copy: the failed commit leaves the database marked dirty, which
    # the player answers with a rescan, not the update this test goes on to)
    cut="$work/card-$n-cut"; rm -rf "$cut"; cp -Rc "$card" "$cut" 2>/dev/null || cp -R "$card" "$cut"
    db0=$(ls "$cut/.rockbox"/database_*.tcd | grep -vc database_tmp.tcd)
    touch "$cut/Music/Artist 00000/Album 000000/01 Song 000000.mp3"   # something to commit
    DYLD_INSERT_LIBRARIES="$B/fail_master_open.dylib" "$B/scan-test" "$cut" --update >/dev/null 2>&1 || true
    db1=$(ls "$cut/.rockbox"/database_*.tcd 2>/dev/null | grep -vc database_tmp.tcd || true)
    if [ "$db1" -ge "$db0" ]; then echo "a master index the commit cannot open: the database kept ($db1 files)"
    else echo "FAIL: a master index the commit cannot open deleted the database ($db0 files, $db1 left)"; status=1; fi
    rm -rf "$cut"
    "$B/scan-test" "$card" --update || status=1
    rm -rf "$card"
done
exit $status
