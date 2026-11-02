#!/usr/bin/env python3
"""Generate the DSD reference files for the WavPack DSD decode test.

Every file is made here from a formula (sine tones through a second-order
sigma-delta modulator, or a fixed pseudo-random bit pattern), so the test
vectors carry no third-party audio. The committed .wv files in
tests/wvdsd/ were made from these with WavPack 5.6.0:

  wavpack -f  tone64.dsf  -o tone64-fast.wv     (DSD mode 1, fast)
  wavpack -h  tone64.dsf  -o tone64-high.wv     (DSD mode 3, high)
  wavpack     noise64.dsf -o noise64-raw.wv     (stored raw, mode 0)
  wavpack -h  same128.dsf -o same128-high.wv    (DSD128, L = R: false stereo)
  wavpack -f  mono64.dsf  -o mono64-fast.wv     (one channel)

usage: gen_dsd.py <out-dir>
"""
import math
import struct
import sys
import os

DSF_BLOCK = 4096


def sdm(freq, rate, nbits, amp=0.5):
    """Second-order sigma-delta: one bit per sample, MSB first per byte."""
    out = bytearray((nbits + 7) // 8)
    i1 = i2 = 0.0
    y = 1.0
    w = 2.0 * math.pi * freq / rate
    for n in range(nbits):
        x = amp * math.sin(w * n)
        i1 += x - y
        i2 += i1 - y
        y = 1.0 if i2 >= 0.0 else -1.0
        if y > 0:
            out[n >> 3] |= 0x80 >> (n & 7)
    return bytes(out)


def noise(nbytes, seed):
    out = bytearray(nbytes)
    s = seed
    for i in range(nbytes):
        s = (s * 1103515245 + 12345) & 0x7FFFFFFF
        out[i] = (s >> 16) & 0xFF
    return bytes(out)


REV = bytes(int('{:08b}'.format(b)[::-1], 2) for b in range(256))


def write_dsf(path, rate, chans):
    """chans: one bytes object per channel, MSB first; DSF stores LSB first."""
    nbits = len(chans[0]) * 8
    blocks = (len(chans[0]) + DSF_BLOCK - 1) // DSF_BLOCK
    data = bytearray()
    for b in range(blocks):
        for c in chans:
            part = c[b * DSF_BLOCK:(b + 1) * DSF_BLOCK].translate(REV)
            data += part + b'\0' * (DSF_BLOCK - len(part))
    fmt = struct.pack('<4sQIIIIIIQII', b'fmt ', 52, 1, 0,
                      2 if len(chans) == 2 else 1, len(chans), rate, 1,
                      nbits, DSF_BLOCK, 0)
    total = 28 + len(fmt) + 12 + len(data)
    head = struct.pack('<4sQQQ', b'DSD ', 28, total, 0)
    with open(path, 'wb') as f:
        f.write(head + fmt + struct.pack('<4sQ', b'data', 12 + len(data)) + data)


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    r64, r128 = 2822400, 5644800
    n = int(r64 * 0.15) // 8 * 8 + 4 * 8   # not a whole number of blocks
    write_dsf(os.path.join(out, 'tone64.dsf'), r64,
              [sdm(1000, r64, n), sdm(1500, r64, n, 0.4)])
    write_dsf(os.path.join(out, 'noise64.dsf'), r64,
              [noise(8000, 1), noise(8000, 2)])
    t = sdm(3000, r128, int(r128 * 0.06) // 8 * 8)
    write_dsf(os.path.join(out, 'same128.dsf'), r128, [t, t])
    write_dsf(os.path.join(out, 'mono64.dsf'), r64,
              [sdm(440, r64, int(r64 * 0.08) // 8 * 8)])


if __name__ == '__main__':
    main()
