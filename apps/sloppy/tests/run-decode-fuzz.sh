#!/bin/bash
# The playback decoders (lib/rbcodec/codecs) against damaged files, under
# ASan and UBSan: Rockbox's own warble player is built for the R1's config
# with a stand-in SDL, then plays the files in decode/ that once crashed or
# hung a codec, and each file given, damaged the same way each run.
# A hang is a play that has not finished in 10 s.
#
#   run-decode-fuzz.sh [-n mutants a file] [file ...]      default 1000
set -eu
n=1000
[ "${1:-}" = -n ] && { n=$2; shift 2; }
test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo=$(CDPATH= cd -- "$test_dir/../../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/sloppy-decode.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work/bin" "$work/build" "$work/stub" "$work/runs"
# Xcode's ASan runtime: the Command Line Tools' hangs before main on macOS 27
[ -d /Applications/Xcode.app ] && export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
unset SDKROOT || true
sdk=$(xcrun --show-sdk-path)
cat > "$work/bin/gcc" <<EOF
#!/bin/bash
[ "\$1" = --version ] && { echo "gcc (clang) 15.0"; exit 0; }
a=(); for x in "\$@"; do [ "\$x" = -lasan ] || a+=("\$x"); done
exec clang -isysroot "$sdk" -w "\${a[@]}"
EOF
printf '#!/bin/sh\nexec "%s/bin/gcc" -E "$@"\n' "$work" > "$work/bin/cpp"
printf '#!/bin/sh\neval in=\\${$(($#-1))}; eval out=\\${$#}\n[ "$in" = "$out" ] || cp "$in" "$out"\n' > "$work/bin/objcopy"
printf '#!/bin/sh\ncase "$1" in --cflags) echo "-I%s/stub";; --version) echo 2.0.99;; esac\n' "$work" > "$work/bin/sdl-config"
ln -s sdl-config "$work/bin/sdl2-config"
ln -s gcc "$work/bin/gcc-15"; ln -s cpp "$work/bin/cpp-15"
for t in gcc-ar gcc-ar-15; do ln -s "$(xcrun -f ar)" "$work/bin/$t"; done
chmod +x "$work/bin/"*
# warble writes to a file here: SDL's audio is never opened
cat > "$work/stub/SDL.h" <<'EOF'
#include <stdlib.h>
#include <stdint.h>
typedef struct SDL_sem SDL_sem;
typedef uint16_t Uint16; typedef uint8_t Uint8; typedef uint32_t Uint32;
typedef struct { int freq; Uint16 format; Uint8 channels, silence; Uint16 samples, padding; Uint32 size;
                 void (*callback)(void *, Uint8 *, int); void *userdata; } SDL_AudioSpec;
#define SDL_INIT_AUDIO 0x10
#define AUDIO_S16SYS 0x8010
static inline int SDL_Init(int f) { (void)f; return -1; }
static inline const char *SDL_GetError(void) { return "no SDL"; }
static inline SDL_sem *SDL_CreateSemaphore(int v) { (void)v; abort(); }
static inline int SDL_SemPost(SDL_sem *s) { (void)s; abort(); }
static inline int SDL_SemWait(SDL_sem *s) { (void)s; abort(); }
static inline void SDL_DestroySemaphore(SDL_sem *s) { (void)s; }
static inline int SDL_OpenAudio(SDL_AudioSpec *a, SDL_AudioSpec *b) { (void)a; (void)b; return -1; }
static inline void SDL_PauseAudio(int p) { (void)p; }
static inline void SDL_CloseAudio(void) {}
static inline void SDL_Quit(void) {}
static inline void SDL_LockAudio(void) {}
static inline void SDL_UnlockAudio(void) {}
static inline void SDL_Delay(Uint32 ms) { (void)ms; }
EOF
cat > "$work/stub/endian.h" <<'EOF'
#include <machine/endian.h>
#include <libkern/OSByteOrder.h>
#define htobe16(x) OSSwapHostToBigInt16(x)
#define htole16(x) OSSwapHostToLittleInt16(x)
#define be16toh(x) OSSwapBigToHostInt16(x)
#define le16toh(x) OSSwapLittleToHostInt16(x)
#define htobe32(x) OSSwapHostToBigInt32(x)
#define htole32(x) OSSwapHostToLittleInt32(x)
#define be32toh(x) OSSwapBigToHostInt32(x)
#define le32toh(x) OSSwapLittleToHostInt32(x)
#define htobe64(x) OSSwapHostToBigInt64(x)
#define htole64(x) OSSwapHostToLittleInt64(x)
#define be64toh(x) OSSwapBigToHostInt64(x)
#define le64toh(x) OSSwapLittleToHostInt64(x)
EOF
printf '#define bswap_16(x) __builtin_bswap16(x)\n#define bswap_32(x) __builtin_bswap32(x)\n#define bswap_64(x) __builtin_bswap64(x)\n' > "$work/stub/byteswap.h"
printf '#include <stdio.h>\n#include <stdlib.h>\nvoid __assert(const char *f, int l, const char *e) { fprintf(stderr, "ROCKBOX ASSERT: %%s:%%d: %%s\\n", f, l, e); abort(); }\n' > "$work/stub/assert.c"
clang -c "$work/stub/assert.c" -o "$work/stub/assert.o" && ar rc "$work/stub/libassertshim.a" "$work/stub/assert.o"
export PATH="$work/bin:$PATH"
B="$work/build"
san="-fsanitize=address,undefined -fno-sanitize-recover=address"
(cd "$B" && CC=gcc "$repo/tools/configure" --target=hibyr1 --type=w --with-address-sanitizer --with-ubsan >configure.log 2>&1)
sed -i '' -e "s|^export GCCOPTS=.*|export GCCOPTS= -Wno-pointer-sign -fno-builtin -g -O1 -fno-omit-frame-pointer $san -fPIC -I\$(ROOTDIR)/lib/skin_parser|" \
    -e "s|^export LDOPTS=.*|export LDOPTS=-lpthread -lm -ldl $san -L$work/stub -lassertshim|" \
    -e 's|^export GLOBAL_LDOPTS=.*|export GLOBAL_LDOPTS=|' \
    -e "s|^export SHARED_LDFLAGS=.*|export SHARED_LDFLAGS=-dynamiclib $san -Wl,-no_warn_duplicate_libraries|" "$B/Makefile"
# the SID codec (gcc's nested functions) does not build with clang: not needed here
(cd "$B" && make -k -j8 >make.log 2>&1) || true
[ -x "$B/warble.hibyr1" ] || { echo "FAIL: warble did not build (see make.log)"; cp "$B/make.log" /tmp/sloppy-decode-make.log; exit 1; }
cat > "$work/mut.c" <<'EOF'
/* mut <file> <seed> <out>: one damaged copy, the same for the same seed */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
static unsigned rs;
static unsigned rnd(void) { rs = rs * 1103515245u + 12345u; return rs >> 8; }
int main(int argc, char **argv)
{
    static const uint32_t words[] = { 0xffffffffu, 0x7fffffffu, 0x80000000u, 0, 0xfffffff0u, 0x10000 };
    static const unsigned char bytes[] = { 0, 0xff, 0x7f, 0x80, 0xfe, 1 };
    FILE *f = argc == 4 ? fopen(argv[1], "rb") : NULL;
    if (!f) return 2;
    fseek(f, 0, SEEK_END); size_t len = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *m = malloc(len + 4);
    if (!len || fread(m, 1, len, f) != len) return 2;
    fclose(f);
    rs = (unsigned)atoi(argv[2]);
    for (int j = 0, k = 1 + rnd() % 8; j < k; j++) {
        size_t at = rnd() % len;
        if (rnd() % 2) at = rnd() % (len < 4096 ? len : 4096);   /* half in the headers */
        uint32_t v;
        switch (rnd() % 6) {
        case 0: m[at] ^= 1u << (rnd() % 8); break;
        case 1: m[at] = bytes[rnd() % 6]; break;
        case 2: v = words[rnd() % 6]; if (at + 4 <= len) memcpy(m + at, &v, 4); break;
        case 3: v = __builtin_bswap32(words[rnd() % 6]); if (at + 4 <= len) memcpy(m + at, &v, 4); break;
        case 4: if (rnd() % 3 == 0) len = at + 1; else m[at] ^= 0x80; break;
        default: m[at] = (unsigned char)rnd(); break;
        }
    }
    f = fopen(argv[3], "wb"); fwrite(m, 1, len, f); fclose(f);
    return 0;
}
EOF
clang -O1 -w -o "$work/mut" "$work/mut.c"

export ASAN_OPTIONS=detect_leaks=0:allocator_may_return_null=1 UBSAN_OPTIONS=print_stacktrace=1
# play one file; prints why it failed, if it did
play() {
    local rc=0
    timeout -s KILL 10 "$B/warble.hibyr1" -c wait=13230000:halt=1 "$1" /dev/null >/dev/null 2>"$work/err" || rc=$?
    if [ $rc -eq 137 ]; then echo "hang"
    elif grep -q "ERROR: AddressSanitizer\|ROCKBOX ASSERT" "$work/err"; then
        grep -m1 -E "ERROR: AddressSanitizer|ROCKBOX ASSERT" "$work/err"; grep -m3 -E "^ +#[0-9]+ " "$work/err"
    elif [ $rc -ge 128 ]; then echo "signal $((rc - 128))"
    fi
}
status=0
for f in "$test_dir"/decode/*; do
    why=$(play "$f")
    if [ -n "$why" ]; then echo "FAIL decode/$(basename "$f"): $why"; status=1; fi
done
[ $status = 0 ] && echo "ok  decode/: $(ls "$test_dir/decode" | wc -l | tr -d ' ') files that once crashed or hung"
for s in "$@"; do
    b=$(basename "$s"); bad=0
    for i in $(seq 1 "$n"); do
        m="$work/runs/$i.${b##*.}"
        "$work/mut" "$s" "$i" "$m"
        why=$(play "$m")
        if [ -n "$why" ]; then
            cp "$m" "/tmp/sloppy-decode-$b-$i.${b##*.}"
            echo "FAIL $b, damaged copy $i (kept in /tmp/sloppy-decode-$b-$i.${b##*.}): $why"; bad=1; status=1
        fi
        rm -f "$m"
    done
    [ $bad = 0 ] && echo "ok  $b: $n damaged copies played"
done
exit $status
