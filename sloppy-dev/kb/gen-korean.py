#!/usr/bin/env python3
"""Hangul for the kor<px>.slf and korx<px>.slf letter packs.

kor-codepoints.txt: the 2,350 syllables of KS X 1001 (every syllable in
everyday Korean; Python's euc_kr codec holds exactly these as two bytes)
and the compatibility jamo (ㅋㅋ, ㅠㅠ, a lone consonant typed alone).
korx-codepoints.txt: the other 8,822 modern syllables, a pack read only
when a title has one (똠, 햏, 뷁). Conjoining jamo from a Mac's decomposed
file names are put together into these syllables at run time.
"""
import os
HERE = os.path.dirname(os.path.abspath(__file__))
ks = [c for c in range(0xac00, 0xd7a4) if len(chr(c).encode('euc_kr', 'ignore')) == 2]
assert len(ks) == 2350
rest = sorted(set(range(0xac00, 0xd7a4)) - set(ks))
with open(os.path.join(HERE, 'kor-codepoints.txt'), 'w') as f:
    f.write('\n'.join('%x' % c for c in ks) + '\n3131-318e\n')
with open(os.path.join(HERE, 'korx-codepoints.txt'), 'w') as f:
    f.write('\n'.join('%x' % c for c in rest) + '\n')
print(len(ks), 'common syllables and 94 jamo;', len(rest), 'more')
