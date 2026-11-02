#!/usr/bin/env python3
"""Make tone-opus.ogg, the radio's Ogg Opus test vector, on a Mac.

A tone made here (left 440 Hz, right 660 Hz, 1.5 s at 48 kHz: e_opus_l and
e_opus_r in batch_e.inc) is encoded by macOS's own Opus encoder
(afconvert, which writes Opus only in CAF), and its packets are put in Ogg
pages here: OpusHead with the encoder's priming as the pre-skip, OpusTags,
then the audio, the end trimmed by the last page's granule position.

usage: make_opus_vector.py <out.ogg>
"""
import math, os, struct, subprocess, sys, tempfile, wave

RATE, SECS = 48000, 1.5


def tone():
    fr = bytearray()
    for n in range(int(RATE * SECS)):
        fr += struct.pack('<hh', int(12000 * math.sin(2 * math.pi * 440 * n / RATE)),
                          int(9000 * math.sin(2 * math.pi * 660 * n / RATE + 0.3)))
    return bytes(fr)


def crc32(data):
    c = 0
    for b in data:
        c ^= b << 24
        for _ in range(8):
            c = (c << 1) ^ 0x04c11db7 if c & 0x80000000 else c << 1
            c &= 0xffffffff
    return c


def page(serial, seq, granule, flags, packets):
    lace = bytearray()
    for p in packets:
        lace += b'\xff' * (len(p) // 255) + bytes([len(p) % 255])
    assert len(lace) <= 255
    head = struct.pack('<4sBBqIII', b'OggS', 0, flags, granule, serial, seq, 0) + bytes([len(lace)]) + lace
    body = b''.join(packets)
    c = crc32(head + body)
    return head[:22] + struct.pack('<I', c) + head[26:] + body


def caf_opus(path):
    d = open(path, 'rb').read()
    p, chunks = 8, {}
    while p < len(d):
        t, n = d[p:p + 4], struct.unpack('>q', d[p + 4:p + 12])[0]
        chunks[t] = d[p + 12:p + 12 + n]
        p += 12 + n
    desc = chunks[b'desc']
    assert desc[8:12] == b'opus'
    chans = struct.unpack('>I', desc[24:28])[0]
    pakt = chunks[b'pakt']
    npk, valid, priming, _ = struct.unpack('>qqii', pakt[:24])
    sizes, v, q = [], 0, 24
    while len(sizes) < npk:
        b = pakt[q]; q += 1
        v = (v << 7) | (b & 0x7f)
        if not b & 0x80:
            sizes.append(v); v = 0
    data, at, pk = chunks[b'data'][4:], 0, []
    for s in sizes:
        pk.append(data[at:at + s]); at += s
    return chans, priming, valid, pk


def main():
    out = sys.argv[1]
    with tempfile.TemporaryDirectory() as t:
        w = wave.open(os.path.join(t, 'tone.wav'), 'wb')
        w.setnchannels(2); w.setsampwidth(2); w.setframerate(RATE)
        w.writeframes(tone()); w.close()
        subprocess.check_call(['afconvert', '-f', 'caff', '-d', 'opus', '-b', '96000',
                               os.path.join(t, 'tone.wav'), os.path.join(t, 'tone.caf')])
        chans, skip, valid, packets = caf_opus(os.path.join(t, 'tone.caf'))
    serial = 0x67616261                   # "gaba"
    head = b'OpusHead' + struct.pack('<BBHIhB', 1, chans, skip, RATE, 0, 0)
    tags = b'OpusTags' + struct.pack('<I', 10) + b'gabagoolOS' + struct.pack('<I', 0)
    pages = [page(serial, 0, 0, 2, [head]), page(serial, 1, 0, 0, [tags])]
    granule, seq = 0, 2
    for i in range(0, len(packets), 10):
        group = packets[i:i + 10]
        granule += 960 * len(group)
        last = i + 10 >= len(packets)
        pages.append(page(serial, seq, skip + valid if last else granule, 4 if last else 0, group))
        seq += 1
    open(out, 'wb').write(b''.join(pages))
    print('%s: %d packets, pre-skip %d, %d samples' % (out, len(packets), skip, valid))


if __name__ == '__main__':
    main()
