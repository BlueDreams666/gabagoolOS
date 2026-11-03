#!/bin/bash
# tick_stall_test.c on Linux (it needs timer_create). On a Mac it runs in
# the R1's build VM (build-support/r1-local), which sees this folder.
set -eu
test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if [ "$(uname)" != Linux ]; then
    export PATH="$HOME/tools/lima/bin:$PATH"
    exec limactl shell r1build -- bash "$test_dir/$(basename "$0")"
fi
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
# kernel-unix.c's Rockbox headers: the test declares what it needs itself
for h in config system kernel button audio panic timer; do : > "$work/$h.h"; done
gcc -std=gnu11 -O1 -g -Wall -Wno-unused-function -fsanitize=address,undefined -I"$work" \
    -o "$work/tick_stall_test" "$test_dir/tick_stall_test.c" -lpthread -lrt
"$work/tick_stall_test"
