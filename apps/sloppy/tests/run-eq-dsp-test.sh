#!/bin/sh
# The EQ's DSP against the eq.c and dsp_filter.c from before typed bands
# (EQ_OLD_REV: its own 32-bit filters) and a double RBJ reference. Usage: run-eq-dsp-test.sh [legacy|typed|sweep|low|overload|switch|cost]
set -eu
test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo=$(CDPATH= cd -- "$test_dir/../../.." && pwd)
build=${SLOPPY_BUILD_SIM:-$repo/../build-support/build-sim}
old_rev=${EQ_OLD_REV:-6e7387fb29}
work=$(mktemp -d "${TMPDIR:-/tmp}/sloppy-eq.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir "$work/old"
git -C "$repo" show "$old_rev:lib/rbcodec/dsp/eq.c" > "$work/old/eq.c"
git -C "$repo" show "$old_rev:lib/rbcodec/dsp/eq.h" > "$work/old/eq.h"
git -C "$repo" show "$old_rev:lib/rbcodec/dsp/dsp_filter.c" > "$work/old/dsp_filter.c"
git -C "$repo" show "$old_rev:lib/rbcodec/dsp/dsp_filter.h" > "$work/old/dsp_filter.h"
# the EQ's config.cfg reading and writing, as settings_list.c has them, today and before
sed -n '/^static void eq_load_from_cfg/,/^#ifdef HAVE_PLAY_FREQ/p' "$repo/apps/settings_list.c" | sed '$d' > "$work/eq_cfg_new.inc"
git -C "$repo" show "$old_rev:apps/settings_list.c" | sed -n '/^static void eq_load_from_cfg/,/^#ifdef HAVE_PLAY_FREQ/p' | sed '$d' \
    | sed 's/struct eq_band_setting/struct old_eq_band_setting/g; s/eq_load_from_cfg/old_eq_load_from_cfg/; s/eq_write_to_cfg/old_eq_write_to_cfg/; s/eq_is_changed/old_eq_is_changed/; s/eq_set_default/old_eq_set_default/' > "$work/eq_cfg_old.inc"
inc="-I$work -I$build -I$repo/firmware/include -I$repo/firmware/export -I$repo/firmware/kernel/include \
 -I$repo/firmware/target/hosted/sdl -I$repo/firmware/target/hosted -I$repo/firmware/target/hosted/hiby/r1 \
 -I$repo/firmware -I$repo/lib/rbcodec -I$repo/lib/rbcodec/dsp -I$repo/lib/rbcodec/metadata -I$repo/lib/fixedpoint \
 -I$repo/apps -I$repo/uisimulator/common -DHIBY_R1 -DDEBUG"
flags="-std=gnu99 -O2 -g -Wall -Wno-unused-function ${EQ_SAN:+-fsanitize=$EQ_SAN} ${EQ_CFLAGS:-}"
# the old filters under their own names, so the old eq.c runs on them
old_names=""
for f in filter_shelf_coefs filter_bishelf_coefs filter_pk_coefs filter_ls_coefs filter_hs_coefs \
         filter_copy filter_flush filter_process enqueue dequeue; do
    old_names="$old_names -D$f=old_$f"
done
${CC:-cc} $flags -I"$work/old" $inc $old_names -c "$work/old/dsp_filter.c" -o "$work/old_filter.o"
${CC:-cc} $flags -I"$work/old" $inc $old_names -c "$work/old/eq.c" -o "$work/old_eq.o" \
    -Ddsp_set_eq_precut=old_dsp_set_eq_precut -Ddsp_set_eq_coefs=old_dsp_set_eq_coefs \
    -Ddsp_eq_enable=old_dsp_eq_enable -DEQUALIZER_proc_db_entry=old_EQUALIZER_proc_db_entry
${CC:-cc} $flags $inc -DEQ_CFG_TEST "$test_dir/eq_dsp_test.c" "$repo/lib/rbcodec/dsp/eq.c" \
    "$repo/lib/rbcodec/dsp/dsp_filter.c" "$repo/lib/fixedpoint/fixedpoint.c" "$work/old_eq.o" "$work/old_filter.o" -lm -o "$work/eq-dsp-test"
"$work/eq-dsp-test" "$@"
