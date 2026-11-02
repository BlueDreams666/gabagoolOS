#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""append.py rows.json: append new phrases to every table, then run gen-zh.py
and gen-langs.py.

The five-column table is append-only (other rows live only in the .h), so
nothing is regenerated from translations.py: each phrase goes to the end of
translations.py's T, arabic.py's AR, zh.py's ZH, it.py's IT ... ja.py's JA
and sloppy_lang_table.h. rows.json is a list of
[en, es, fr, de, he, ar, zh, it, nl, pt, pl, cs, ru, ja]. A phrase already in
the .h stops it (edit that row in place instead); so does a printf mismatch.
Rows of seven (no it ... ja) still go in; gen-langs.py then names them as
missing until their translations are added to it.py ... ja.py."""
import ast, json, re, subprocess, sys
EXTRA = ('it', 'nl', 'pt', 'pl', 'cs', 'ru', 'ja')   # gen-langs.py's, in sloppy_language order
from pathlib import Path

HERE = Path(__file__).resolve().parent
TABLE = HERE.parents[1] / 'apps/sloppy/sloppy_lang_table.h'


def c_str(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n') + '"'


def insert_before_last(path, closer, text):
    s = path.read_text(encoding='utf-8')
    i = s.rfind('\n' + closer + '\n') + 1   # the line that is the closer alone
    assert i > 0, (path, closer)
    path.write_text(s[:i] + text + s[i:], encoding='utf-8')


rows = json.load(open(sys.argv[1], encoding='utf-8'))
have = set()
for line in TABLE.read_text(encoding='utf-8').splitlines():
    m = re.match(r'\{ ("(?:[^"\\]|\\.)*")', line)
    if m: have.add(ast.literal_eval(m.group(1)))
for r in rows:
    assert len(r) in (7, 7 + len(EXTRA)), r
    assert len(r) == len(rows[0]), 'every row the same length'
    if r[0] in have: raise SystemExit(f'already in the table: {r[0]!r}')
    fmt = re.findall(r'%[-+0#]*[0-9]*l?[sdu]', r[0])
    for x in r[1:]:
        if re.findall(r'%[-+0#]*[0-9]*l?[sdu]', x) != fmt: raise SystemExit(f'printf arguments differ: {r[0]!r} / {x!r}')
insert_before_last(HERE / 'translations.py', ']', ''.join('(%s),\n' % ', '.join(repr(x) for x in r[:5]) for r in rows))
insert_before_last(HERE / 'arabic.py', '}', ''.join('%r: %r,\n' % (r[0], r[5]) for r in rows))
insert_before_last(HERE / 'zh.py', '}', ''.join('%r: %r,\n' % (r[0], r[6]) for r in rows))
with open(TABLE, 'a', encoding='utf-8') as f:
    for r in rows:
        f.write('{ %s, { %s } },\n' % (c_str(r[0]), ', '.join(c_str(x) for x in r[1:6])))
if len(rows[0]) > 7:
    for i, code in enumerate(EXTRA):
        insert_before_last(HERE / f'{code}.py', '}', ''.join('%r: %r,\n' % (r[0], r[7 + i]) for r in rows))
subprocess.run([sys.executable, str(HERE / 'gen-zh.py')], check=True, cwd=HERE)
if len(rows[0]) > 7: subprocess.run([sys.executable, str(HERE / 'gen-langs.py')], check=True, cwd=HERE)
print(f'{len(rows)} phrases appended')
