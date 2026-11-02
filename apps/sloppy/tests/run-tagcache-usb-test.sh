#!/bin/sh
# The database scan stops for USB mode wherever its event is in the queue
# (tagcache_usb_test.c). Usage: run-tagcache-usb-test.sh
set -eu
test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo=$(CDPATH= cd -- "$test_dir/../../.." && pwd)
build=${SLOPPY_BUILD_SIM:-$repo/../build-support/build-sim}
work=$(mktemp -d "${TMPDIR:-/tmp}/sloppy-tcusb.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
sed -n '/^static bool check_event_queue(void)/,/^}/p' "$repo/apps/tagcache.c" > "$work/tagcache_check_event_queue.inc"
[ -s "$work/tagcache_check_event_queue.inc" ] || { echo "check_event_queue not found in tagcache.c"; exit 1; }
${CC:-cc} -std=gnu99 -O1 -w \
 -I"$work" -I"$build" -I"$repo/firmware/include" -I"$repo/firmware/export" -I"$repo/firmware/kernel/include" \
 -I"$repo/firmware/kernel" -I"$repo/firmware/target/hosted/sdl" -I"$repo/firmware/target/hosted" \
 -I"$repo/firmware/target/hosted/hiby/r1" -I"$repo/firmware" -I"$repo/apps" -I"$repo/uisimulator/common" \
 -DROCKBOX -DMEMORYSIZE=16 -DHIBY_R1 -DTARGET_ID=124 -DSIMULATOR \
 "$test_dir/tagcache_usb_test.c" -o "$work/tagcache-usb-test"
"$work/tagcache-usb-test"
