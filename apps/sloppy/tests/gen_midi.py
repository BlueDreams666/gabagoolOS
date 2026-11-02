#!/usr/bin/env python3
"""Make the MIDI codec's test files (midi_test.c), each from a few lines here.
usage: gen_midi.py <out-dir>"""
import os
import struct
import sys


def var(n):
    b = [n & 0x7f]
    n >>= 7
    while n:
        b.insert(0, 0x80 | (n & 0x7f))
        n >>= 7
    return bytes(b)


def track(events):
    """events: (delta, bytes) pairs; the end of track is added"""
    body = b''.join(var(d) + e for d, e in events) + var(0) + b'\xff\x2f\x00'
    return b'MTrk' + struct.pack('>I', len(body)) + body


def smf(fmt, div, tracks):
    return b'MThd' + struct.pack('>IHHH', 6, fmt, len(tracks), div) + b''.join(tracks)


def meta(t, data):
    return b'\xff' + bytes([t]) + var(len(data)) + data


def tempo(us):
    return meta(0x51, struct.pack('>I', us)[1:])


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    w = lambda name, data: open(os.path.join(out, name), 'wb').write(data)
    # one A (440 Hz) on a flute for a second at 120 bpm (480 ticks a quarter), named
    tone = smf(0, 480, [track([(0, meta(0x03, b'Test tone')), (0, tempo(500000)), (0, b'\xc0\x49'),
                               (0, b'\x90\x45\x64'), (960, b'\x80\x45\x40')])])
    w('tone440.mid', tone)
    # the same in a RIFF RMID wrapper
    w('tone440.rmi', b'RIFF' + struct.pack('<I', 4 + 8 + len(tone)) + b'RMID' + b'data' + struct.pack('<I', len(tone)) + tone)
    # bent a semitone up (range 2: +4096 is half of it), by running status and a note-on of velocity 0
    w('bend.mid', smf(0, 480, [track([(0, b'\xc0\x49'), (0, b'\xe0\x00\x60'), (0, b'\x90\x45\x64'),
                                      (960, b'\x45\x00')])]))
    # format 1: the tempo map in track 0 (120 bpm for a quarter, then 60), notes in track 1
    w('tempo.mid', smf(1, 96, [track([(0, tempo(500000)), (96, tempo(1000000))]),
                               track([(0, b'\x90\x3c\x50'), (96, b'\x80\x3c\x00'), (0, b'\x90\x40\x50'), (96, b'\x80\x40\x00')])]))
    # SMPTE time: 25 frames of 40 ticks, a thousand ticks a second; a sysex on the way
    w('smpte.mid', smf(0, (256 - 25) << 8 | 40, [track([(0, b'\xf0\x05\x7e\x7f\x09\x01\xf7'), (0, b'\x90\x48\x60'),
                                                         (500, b'\x80\x48\x00')])]))
    # drums for four seconds: kick, snare and hat in eighths at 120 bpm
    ev = []
    for i in range(16):
        notes = [42] + ([36] if i % 4 == 0 else []) + ([38] if i % 4 == 2 else [])
        for k, n in enumerate(notes):
            ev.append((240 if (i and k == 0) else 0, bytes([0x99, n, 100])))
    w('drums.mid', smf(0, 480, [track(ev + [(240, b'\xb9\x7b\x00')])]))
    # an arpeggio, a note every tenth of a second (480 ticks a quarter at 120 bpm: 96 ticks), for seeking
    ev = [(0, b'\xc0\x00')]
    for i in range(30):
        n = 60 + (i * 4) % 24
        ev += [(0 if i == 0 else 48, bytes([0x90, n, 90])), (48, bytes([0x80, n, 0]))]
    w('arp.mid', smf(0, 480, [track(ev)]))
    # dense: sixteen channels, two notes each, every family, held and struck for ten seconds
    tr = []
    for c in range(16):
        if c == 9:
            continue
        ev = [(0, bytes([0xc0 | c, (c * 8) % 128])), (0, bytes([0xb0 | c, 10, (c * 17) % 128]))]
        for b in range(20):
            for k in (0, 7):
                ev.append((0, bytes([0x90 | c, 36 + c * 3 + k, 70])))
            ev.append((240, bytes([0x80 | c, 36 + c * 3, 0])))
            ev.append((0, bytes([0x80 | c, 36 + c * 3 + 7, 0])))
        tr.append(track(ev))
    ev = []
    for i in range(80):
        ev.append((0 if i == 0 else 120, bytes([0x99, (35, 38, 42, 46, 49, 51)[i % 6], 100])))
    tr.append(track(ev))
    w('dense.mid', smf(1, 480, tr))


if __name__ == '__main__':
    main()
