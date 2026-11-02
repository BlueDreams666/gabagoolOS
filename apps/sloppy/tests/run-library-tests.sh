#!/bin/sh
set -eu
test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
test_work=$(mktemp -d "${TMPDIR:-/tmp}/sloppy-library.XXXXXX")
trap 'rm -rf "$test_work"' EXIT HUP INT TERM
${CC:-cc} -std=c99 -Wall -Wextra -Werror -g -fsanitize=${SANITIZERS:-address,undefined} \
    -I"$test_dir/stubs" "$test_dir/library_test.c" -o "$test_work/library-test"
cd "$test_work"
./library-test
