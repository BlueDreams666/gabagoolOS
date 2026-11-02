#!/usr/bin/env python3
"""Thai letters and their marks in context, for the th<px>.slf packs.

Writes thai-codepoints.txt for slfpack.swift: the Thai block, then each
mark as Core Text shapes it after the letters that move it (key=string@i).
sloppy_xfont.inc picks the same keys at run time (xf_thai_key): keep the
two in step.

  key = 0xe000 | context << 7 | (letter - 0x0e00)
  context 0   the letter itself (a mark: as shaped over ko kai)
          1   an upper mark or tone over a tall consonant (po pla, fo fa, ...)
          2+  a tone over an upper vowel: 2 + vowel * 2 + tall
          12  a tone before sara am: 12 + tall
          14  a lower vowel under do chada, to patak, yo ying, tho than: 14 + which
          18  those four letters themselves above a lower vowel: 18 + which
"""
import os
KO, PO = 0x0e01, 0x0e1b                      # ordinary and tall representatives
UPPER = [0x0e31, 0x0e34, 0x0e35, 0x0e36, 0x0e37, 0x0e47, 0x0e4d, 0x0e4e]
TONES = [0x0e48, 0x0e49, 0x0e4a, 0x0e4b, 0x0e4c]
VOWELS = [0x0e31, 0x0e34, 0x0e35, 0x0e36, 0x0e37]   # a tone can sit on these
LOWER = [0x0e38, 0x0e39, 0x0e3a]
DESC = [0x0e0e, 0x0e0f, 0x0e0d, 0x0e10]
AM = 0x0e33


def key(ctx, c):
    return 0xe000 | ctx << 7 | (c - 0x0e00)


def ctx(k, letters, at):
    return '%x=%s@%d' % (k, ','.join('%x' % c for c in letters), at)


lines = ['e01-e3a', 'e3f-e5b']
for m in UPPER + TONES + LOWER + [AM]:
    lines.append(ctx(m, [KO, m], 1))
for m in UPPER + TONES + [AM]:
    lines.append(ctx(key(1, m), [PO, m], 1))
for vi, v in enumerate(VOWELS):
    for tall, b in enumerate([KO, PO]):
        for t in TONES:
            lines.append(ctx(key(2 + vi * 2 + tall, t), [b, v, t], 2))
for tall, b in enumerate([KO, PO]):
    for t in TONES:
        lines.append(ctx(key(12 + tall, t), [b, t, AM], 1))
for di, d in enumerate(DESC):
    for l in LOWER:
        lines.append(ctx(key(14 + di, l), [d, l], 1))
    lines.append(ctx(key(18 + di, d), [d, LOWER[0]], 0))
out = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'thai-codepoints.txt')
with open(out, 'w') as f:
    f.write('\n'.join(lines) + '\n')
print(out, len(lines), 'lines')
