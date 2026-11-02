#!/bin/sh
# The MIDI codec and its metadata parser on the files gen_midi.py makes.
# MIDI_SAN=address,undefined for the sanitizers.
set -eu
test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo=$(CDPATH= cd -- "$test_dir/../../.." && pwd)
build=${SLOPPY_BUILD_SIM:-$repo/../build-support/build-sim}
work=$(mktemp -d "${TMPDIR:-/tmp}/sloppy-midi.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
python3 "$test_dir/gen_midi.py" "$work"
inc="-I$build -I$repo/firmware/include -I$repo/firmware/export -I$repo/firmware/kernel/include \
 -I$repo/firmware/target/hosted/sdl -I$repo/firmware/target/hosted -I$repo/firmware/target/hosted/hiby/r1 \
 -I$repo/firmware -I$repo/lib/rbcodec -I$repo/lib/rbcodec/codecs -I$repo/lib/rbcodec/codecs/lib \
 -I$repo/lib/rbcodec/dsp -I$repo/lib/rbcodec/metadata -I$repo/lib/fixedpoint -I$repo/apps -I$repo/uisimulator/common"
flags="-std=gnu99 -O2 -g -Wall -Wno-unused-function -DHIBY_R1 -DDEBUG ${MIDI_SAN:+-fsanitize=$MIDI_SAN -fno-sanitize-recover=all}"
${CC:-cc} $flags $inc -DCODEC_HEADER= "$test_dir/midi_test.c" "$repo/lib/rbcodec/codecs/midi.c" \
    "$repo/lib/rbcodec/metadata/midi.c" -lm -o "$work/midi-test"
"$work/midi-test" "$work"
