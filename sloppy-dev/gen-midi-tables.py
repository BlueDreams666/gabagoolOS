#!/usr/bin/env python3
"""Write lib/rbcodec/codecs/libmidi/midi_tables.h, the MIDI codec's numbers,
so the codec itself needs no maths library: the sine, the phase steps, the
pan law, and the built-in instruments (GabagoolOS's own, drawn from sines:
a General MIDI family each, and the drum kit), their envelopes as per-block
multipliers. The codec mixes in blocks of BLOCK samples at 44100 Hz.

  sine_q[65]     sin(2 pi k / 256), k = 0..64, Q15
  step_top[768]  32-bit phase step a sample for pitches 120 .. 132 in
                 1/64 semitones; each octave lower is one shift right
  pan_q[128]     the left side's gain at each pan (cos law), Q15
  fam[16]        each family's harmonics (Q8 weights), attack in blocks,
                 decay and release multipliers (Q16 a block), sustain (Q15)
  drum[128]      each GM drum key's tone (start and end step, sweep), its
                 tone and noise levels and decays; zero for unused keys
"""
import math, os

RATE, BLOCK = 44100, 32
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
out = os.path.join(root, 'lib/rbcodec/codecs/libmidi/midi_tables.h')

def mul(t):          # level kept after one block, decaying to 1/e in t seconds
    return 0 if t <= 0 else min(65535, round(65536 * math.exp(-BLOCK / RATE / t)))
def blocks(ms):
    return max(1, round(ms / 1000 * RATE / BLOCK))
def step(hz):
    return round(hz * 2 ** 32 / RATE)

sine = [round(32767 * math.sin(2 * math.pi * k / 256)) for k in range(65)]
steps = [step(440 * 2 ** ((120 + i / 64 - 69) / 12)) for i in range(768)]
pan = [round(32767 * math.cos(p / 127 * math.pi / 2)) for p in range(128)]

# family: harmonics 1..8 (relative), attack ms, decay s (to sustain), sustain 0..1, release s
saw = [1 / h for h in range(1, 9)]
FAM = [
    ('piano',      [1, .55, .28, .18, .1, .06, .03, .02], 3, 1.6, 0.0, .35),
    ('chromatic',  [1, 0, .3, .5, 0, 0, .2, 0],           2, 0.6, 0.0, .5),
    ('organ',      [1, .8, .6, .5, 0, .3, 0, .3],         6, 0.1, 1.0, .06),
    ('guitar',     [1, .6, .45, .3, .22, .15, .1, .07],   3, 1.0, 0.0, .25),
    ('bass',       [1, .5, .2, .1, .05, 0, 0, 0],         4, 1.2, 0.3, .12),
    ('strings',    [s * .9 for s in saw],                 70, 1.0, 0.85, .3),
    ('ensemble',   [1, .45, .3, .2, .14, .1, .07, .05],   110, 1.0, 0.85, .35),
    ('brass',      saw,                                   25, 0.6, 0.8, .12),
    ('reed',       [1, .05, .5, .05, .3, .03, .2, .02],   20, 0.5, 0.9, .1),
    ('pipe',       [1, .15, .06, .02, 0, 0, 0, 0],        35, 0.5, 0.9, .12),
    ('synth lead', [1, .5, .33, .25, .2, .17, .14, .12],  4, 0.3, 0.9, .08),
    ('synth pad',  [1, .4, .2, .12, .06, .03, 0, 0],      280, 1.5, 0.9, .6),
    ('synth fx',   [1, 0, .11, 0, .04, 0, .02, 0],        90, 1.5, 0.7, .5),
    ('ethnic',     [1, .7, .5, .35, .25, .15, .1, .05],   2, 0.9, 0.0, .3),
    ('percussive', [1, .2, .1, 0, 0, 0, 0, 0],            1, 0.35, 0.0, .15),
    ('effects',    [1, .5, .5, .5, .3, .3, .2, .2],       30, 1.0, 0.6, .4),
]

# GM drum keys: tone Hz from, to, sweep s, tone level, tone decay s, noise level, noise decay s, bright (high-passed)
def d(f0, f1, sw, tl, td, nl, nd, br=1):
    return (step(f0), step(f1), mul(sw), round(tl * 32767), mul(td), round(nl * 32767), mul(nd), br)
DR = {}
for k in (35, 36): DR[k] = d(160, 48, .035, 1.0, .28, .05, .02)
DR[37] = d(900, 900, 0, .35, .02, .5, .025)
for k in (38, 40): DR[k] = d(190, 160, .05, .45, .09, .75, .16)
DR[39] = d(0, 0, 0, 0, 0, .9, .12)
for k, f in ((41, 82), (43, 98), (45, 117), (47, 139), (48, 165), (50, 196)): DR[k] = d(f, f * .62, .12, .9, .3, .08, .05)
for k in (42, 44): DR[k] = d(0, 0, 0, 0, 0, .55, .035)
DR[46] = d(0, 0, 0, 0, 0, .55, .3)
for k in (49, 57): DR[k] = d(0, 0, 0, 0, 0, .6, 1.1)
for k in (51, 59): DR[k] = d(520, 520, 0, .12, .5, .4, .7)
DR[52] = d(0, 0, 0, 0, 0, .6, .8)
DR[53] = d(760, 760, 0, .35, .6, .2, .4)
DR[54] = d(0, 0, 0, 0, 0, .5, .16)
DR[55] = d(0, 0, 0, 0, 0, .55, .5)
DR[56] = d(560, 560, 0, .5, .25, 0, 0)
for k, f in ((60, 420), (61, 300), (62, 360), (63, 330), (64, 240)): DR[k] = d(f, f * .9, .05, .6, .12, .1, .03)
for k in (65, 66): DR[k] = d(260, 230, .08, .6, .25, .1, .05)
for k in (67, 68): DR[k] = d(900, 900, 0, .4, .15, .05, .02)
for k in (69, 70): DR[k] = d(0, 0, 0, 0, 0, .35, .08)
for k in (71, 72): DR[k] = d(2400, 2000, .1, .2, .15, 0, 0)
for k in (73, 74): DR[k] = d(0, 0, 0, 0, 0, .3, .1)
for k in (75, 76, 77): DR[k] = d(1200 if k == 76 else 1000, 1000, 0, .45, .06, .05, .02)
for k in (78, 79): DR[k] = d(350, 300, .05, .4, .15, .05, .03)
for k in (80, 81): DR[k] = d(3000, 3000, 0, .25, .12 if k == 80 else .6, 0, 0)

with open(out, 'w') as f:
    w = f.write
    w('/* Made by sloppy-dev/gen-midi-tables.py. Do not edit. */\n')
    w('#define MIDI_RATE %d\n#define MIDI_BLOCK %d\n' % (RATE, BLOCK))
    w('static const int16_t sine_q[65] = {\n')
    for i in range(0, 65, 13): w('    ' + ', '.join('%d' % v for v in sine[i:i + 13]) + ',\n')
    w('};\nstatic const uint32_t step_top[768] = {\n')
    for i in range(0, 768, 8): w('    ' + ', '.join('%uu' % v for v in steps[i:i + 8]) + ',\n')
    w('};\nstatic const int16_t pan_q[128] = {\n')
    for i in range(0, 128, 16): w('    ' + ', '.join('%d' % v for v in pan[i:i + 16]) + ',\n')
    w('};\nstruct midi_family { uint8_t harm[8]; uint16_t attack; uint16_t decay, sustain, release; };\n')
    w('static const struct midi_family fam[16] = {\n')
    for name, h, a, dc, s, r in FAM:
        hq = ', '.join('%d' % round(x * 255) for x in h)
        w('    { { %s }, %d, %d, %d, %d },   /* %s */\n' % (hq, blocks(a), mul(dc), round(s * 32767), mul(r), name))
    w('};\nstruct midi_drum { uint32_t f0, f1; uint16_t sweep; int16_t tone; uint16_t tone_decay; int16_t noise; uint16_t noise_decay; uint8_t bright; };\n')
    w('static const struct midi_drum drum[128] = {\n')
    for k in sorted(DR):
        w('    [%d] = { %uu, %uu, %d, %d, %d, %d, %d, %d },\n' % ((k,) + DR[k]))
    w('};\n')
print(out)
