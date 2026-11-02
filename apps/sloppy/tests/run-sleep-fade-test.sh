#!/bin/sh
# The sleep timer's fade through the real PCM mixer (sleep_fade_test.c).
# Usage: run-sleep-fade-test.sh [envelope.csv]
set -eu
test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo=$(CDPATH= cd -- "$test_dir/../../.." && pwd)
build=${SLOPPY_BUILD_SIM:-$repo/../build-support/build-sim}
work=$(mktemp -d "${TMPDIR:-/tmp}/sloppy-fade.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
${CC:-cc} -std=gnu99 -O2 -Wall -Wno-unused-function \
 -I"$build" -I"$repo/firmware/include" -I"$repo/firmware/export" -I"$repo/firmware/kernel/include" \
 -I"$repo/firmware/target/hosted/sdl" -I"$repo/firmware/target/hosted" -I"$repo/firmware/target/hosted/hiby/r1" \
 -I"$repo/firmware" -I"$repo/lib/rbcodec" -I"$repo/lib/rbcodec/dsp" -I"$repo/apps" -I"$repo/uisimulator/common" \
 -DHIBY_R1 "$test_dir/sleep_fade_test.c" -lm -o "$work/sleep-fade-test"
"$work/sleep-fade-test" "$@"
