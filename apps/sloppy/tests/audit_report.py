#!/usr/bin/env python3
"""Triage for the screen audit (SLOPPY_AUDIT=<dir> ./ui-test).

    python3 audit_report.py <dir>            counts, then findings deduplicated
                                             across looks, most visible first
    python3 audit_report.py <dir> --png      also turn <dir>/shots/*.ppm into PNG

A finding is the same one in every look when its screen, rule and object
path match; the text is not part of the key, since it changes with the
language. Visibility = how bad the rule is x how often the screen is seen x
in how many looks it shows.
"""
import collections, os, re, struct, sys, zlib

RULES = {'a': 'overlap', 'b': 'clipped', 'c': 'chrome', 'd': 'tap target', 'e': 'contrast'}
SEEN = [  # how often a user is on the screen: first match wins
    (r'^(home|music|songs|albums?|now-playing|queue|mini|control-centre|lyrics|toast|banner|song-sheet)', 3),
    (r'^(artist|genre|year|playlist|search|library|loved|for-you|most|wrapped|files|audiobook|lock|volume|empty)', 2),
    (r'^(settings|wifi|bluetooth|internet-radio|coverflow)', 1.5),
]


def seen(screen):
    for pat, w in SEEN:
        if re.match(pat, screen):
            return w
    return 1  # the Playroom


def severity(rule, detail):
    if rule == 'c':
        return 3
    if rule == 'a':
        return 2.5
    if rule == 'b':
        m = re.search(r'(\d+) px', detail)
        return 2.5 if 'no ellipsis' in detail or 'runs off' in detail or (m and int(m.group(1)) >= 3) else 1.5
    if rule == 'e':
        m = re.search(r'contrast ([\d.]+)', detail)
        r = float(m.group(1)) if m else 3
        return 3 if r < 2 else 2 if r < 3 else 1
    return 1.5  # d


def load(d):
    rows = []
    with open(os.path.join(d, 'audit.tsv'), encoding='utf-8', errors='replace') as f:
        next(f)
        for line in f:
            p = line.rstrip('\n').split('\t')
            if len(p) >= 8:
                rows.append(dict(screen=p[0], look=p[1], rule=p[2], path=p[3], text=p[4], rect=p[5], other=p[6], detail=p[7]))
    return rows


def key(r):
    detail = re.sub(r'[\d.]+', '#', r['detail'].split('"')[0])
    return (r['screen'].replace('@end', ''), r['rule'], r['path'], detail)


def ppm_to_png(src, dst):
    data = open(src, 'rb').read()
    head = data.split(b'\n', 3)
    w, h = map(int, head[1].split())
    px = head[3]
    raw = b''.join(b'\0' + px[y * w * 3:(y + 1) * w * 3] for y in range(h))
    chunk = lambda t, b: struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
    open(dst, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                          chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def main():
    d = sys.argv[1]
    rows = load(d)
    by_rule = collections.Counter(r['rule'] for r in rows)
    print(f'{len(rows)} findings: ' + ', '.join(f'{RULES[k]} {by_rule.get(k, 0)}' for k in 'abcde'))
    groups = collections.OrderedDict()
    for r in rows:
        groups.setdefault(key(r), []).append(r)
    uniq = collections.Counter(k[1] for k in groups)
    print(f'{len(groups)} distinct: ' + ', '.join(f'{RULES[k]} {uniq.get(k, 0)}' for k in 'abcde'))
    scored = []
    for k, g in groups.items():
        looks = sorted({r['look'] for r in g})
        score = severity(k[1], g[0]['detail']) * seen(k[0]) * (1 + len(looks) ** 0.5)
        scored.append((score, k, g, looks))
    scored.sort(key=lambda s: -s[0])
    print()
    for score, k, g, looks in scored:
        ex = g[0]
        print(f'{score:6.1f}  {k[0]:<26} {k[1]} {RULES[k[1]]:<10} {ex["text"][:48]:<50} {ex["detail"][:70]}  '
              f'[{len(looks)} looks, e.g. {looks[0]}]  {k[2]}')
    if '--png' in sys.argv:
        sd = os.path.join(d, 'shots')
        for n in sorted(os.listdir(sd)):
            if n.endswith('.ppm'):
                ppm_to_png(os.path.join(sd, n), os.path.join(sd, n[:-4] + '.png'))
                os.remove(os.path.join(sd, n))


if __name__ == '__main__':
    main()
