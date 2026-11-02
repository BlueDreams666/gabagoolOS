#!/usr/bin/env python3
"""apps/sloppy/sloppy_unshape.h: LVGL shapes Arabic as it stores a label's
text (LV_USE_ARABIC_PERSIAN_CHARS), so a text field hands back presentation
forms. This table turns each back into the letters it stands for, from
Unicode's own compatibility decompositions."""
import unicodedata as u
from pathlib import Path

rows = []
for cp in list(range(0xFB50, 0xFC00)) + list(range(0xFE70, 0xFF00)):
    d = u.decomposition(chr(cp))
    if not d or not d.startswith('<'):
        continue
    parts = [int(x, 16) for x in d.split()[1:]]
    if not all(0x600 <= p <= 0x6ff for p in parts) or len(parts) > 2:
        continue
    rows.append((cp, parts[0], parts[1] if len(parts) > 1 else 0))
out = ["/* Generated from Unicode's compatibility decompositions (sloppy-dev/kb/gen-unshape.py):",
       " * an Arabic presentation form, as LVGL's shaping stores it, and the letters it stands for */",
       "static const uint16_t sl_unshape_map[][3] = {"]
line = "   "
for r in rows:
    item = " { 0x%04x, 0x%04x, 0x%04x }," % r
    if len(line) + len(item) > 110:
        out.append(line)
        line = "   "
    line += item
out += [line, "};"]
Path(__file__).resolve().parents[2].joinpath("apps/sloppy/sloppy_unshape.h").write_text("\n".join(out) + "\n")
print(len(rows), "forms")
