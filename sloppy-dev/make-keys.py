#!/usr/bin/env python3
"""Write the card's keys file (.rockbox/sloppy/keys.bin) for a release.

The Last.fm API key and secret stay out of the program and its source: a
release carries them in this file, scrambled the way lfm_keys_load() in
apps/sloppy/sloppy_net.c reads it. Without the file, Last.fm says "Not in
this build" and the rest of scrobbling works.

    python3 make-keys.py <sloppy_lastfm_key.h> <out/keys.bin>

The header is the local, untracked one with SL_LASTFM_API_KEY and
SL_LASTFM_API_SECRET. Nothing is printed of either.
"""
import re
import sys


def scramble(data):
    x = 0x9E3779B9
    out = bytearray(data)
    for i in range(len(out)):
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        out[i] ^= x & 0xFF
    return bytes(out)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    text = open(sys.argv[1], encoding='utf-8').read()
    found = dict(re.findall(r'#define\s+SL_LASTFM_API_(KEY|SECRET)\s+"([^"]*)"', text))
    key, secret = found.get('KEY', ''), found.get('SECRET', '')
    if not key or not secret or '\t' in key + secret or len(key) > 63 or len(secret) > 63:
        sys.exit('no usable SL_LASTFM_API_KEY and SL_LASTFM_API_SECRET in ' + sys.argv[1])
    body = f'lastfm\t{key}\t{secret}\n'.encode()
    with open(sys.argv[2], 'wb') as f:
        f.write(b'GBK1' + scramble(body))
    print('wrote', sys.argv[2], f'({4 + len(body)} bytes)')


if __name__ == '__main__':
    main()
