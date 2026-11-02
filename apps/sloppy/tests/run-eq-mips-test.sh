#!/bin/sh
# dsp_mips.S's EQ filter loop against dsp_filter.c's C, bit for bit (eq_mips_test.py).
# Needs the R1 cross toolchain, so on the Mac it runs itself in the r1build VM,
# which mounts this folder at the same path. Usage: run-eq-mips-test.sh [cases]
set -eu
test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo=$(CDPATH= cd -- "$test_dir/../../.." && pwd)
G=${R1_CROSS:-$HOME/rb/rbdev/bin/mipsel-rockbox-linux-gnu-}
if [ ! -x "${G}gcc" ]; then
    lima=${LIMACTL:-$HOME/tools/lima/bin/limactl}
    exec "$lima" shell r1build -- sh "$test_dir/run-eq-mips-test.sh" "$@"
fi
build=${SLOPPY_BUILD_SIM:-$repo/../build-support/build-sim}
work=$(mktemp -d "${TMPDIR:-/tmp}/sloppy-eq-mips.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
inc="-I$build -I$repo/firmware/include -I$repo/firmware/export -I$repo/firmware/kernel/include \
 -I$repo/firmware/target/hosted/sdl -I$repo/firmware/target/hosted -I$repo/firmware/target/hosted/hiby/r1 \
 -I$repo/firmware -I$repo/lib/rbcodec -I$repo/lib/rbcodec/dsp -I$repo/lib/rbcodec/metadata -I$repo/lib/fixedpoint -I$repo/apps -DHIBY_R1"
# the asm as the R1 build assembles it
"${G}gcc" -march=mips32r2 -mhard-float -mno-mips16 -fPIC -Umips -DAPPLICATION $inc \
    -c "$repo/lib/rbcodec/dsp/dsp_mips.S" -o "$work/dsp_mips.o"
if "${G}readelf" -rW "$work/dsp_mips.o" | grep -q "Relocation section '.rel.text'"; then
    echo 'dsp_mips.o has relocations; the interpreter runs .text as is' >&2
    exit 1
fi
"${G}objcopy" -O binary -j .text "$work/dsp_mips.o" "$work/dsp_mips.bin"
# the C, as the host builds it (the older filters' replaygain table left out)
printf '#include <stdlib.h>\nlong get_replaygain_int(long g) { (void)g; abort(); }\n' > "$work/rg.c"
${CC:-cc} -std=gnu99 -O2 -fPIC -shared -fno-strict-overflow $inc \
    "$repo/lib/rbcodec/dsp/dsp_filter.c" "$repo/lib/fixedpoint/fixedpoint.c" "$work/rg.c" -o "$work/libref.so"
python3 "$test_dir/eq_mips_test.py" "$work/dsp_mips.bin" "$work/libref.so" "$@"
