#!/usr/bin/env python3
"""Hebrew drawn to match the Latin beside it.

Noto Sans Hebrew's letters sit about as high as Source Sans's lower case and
read lighter at the same weight, so a Hebrew title looked smaller and thinner
than an English one in the same row. Each UI font's Hebrew now comes from
its own font, ss_heb_<style>_<size>.c, drawn by ttf2lvgl (Core Text, no
other tools) 10 % bigger and a step heavier than the Latin's style:

    regular 400 -> 500, semibold 600 -> 700, bold 700 -> 800

The UI font loses its own Hebrew glyphs and falls back to that font, which
falls back to what the UI font fell back to before (Arabic, or the semibold
of the same size). Run from anywhere; it compiles ttf2lvgl.swift into a temp
folder. Run it again after generate-ui-fonts.py, which puts Hebrew back.
"""

import os
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FONTS = ROOT / "apps" / "sloppy" / "fonts"
HEBREW_TTF = ROOT / "sloppy-dev" / "fonts-src" / "NotoSansHebrew.ttf"
HEBREW = (0x590, 0x5FF)
WGHT = {"regular": 500, "semibold": 700, "bold": 800}
SCALE = 1.1
UI = [("regular", 16), ("regular", 18), ("regular", 21), ("regular", 23), ("regular", 26),
      ("semibold", 18), ("semibold", 21), ("semibold", 23), ("semibold", 26), ("semibold", 30),
      ("semibold", 34), ("semibold", 38), ("bold", 28)]


def strip_hebrew(src):
    """The font's text without its Hebrew glyphs, its cmaps rebuilt as runs."""
    bm0, bm1 = src.index("glyph_bitmap[] = {"), None
    bm1 = src.index("};", bm0)
    parts = re.split(r"(    /\* U\+([0-9A-F]+)[^\n]*\*/\n)", src[bm0 + len("glyph_bitmap[] = {"):bm1])
    blocks = []                          # (codepoint, comment line, bytes)
    for i in range(1, len(parts), 3):
        data = re.findall(r"0x[0-9a-f]{1,2}", parts[i + 2])
        blocks.append((int(parts[i + 1], 16), parts[i], data))
    d0 = src.index("glyph_dsc[] = {")
    d1 = src.index("};", d0)
    dsc = re.findall(r"\{\.bitmap_index = (\d+), (\.adv_w = [^}]*)\}", src[d0:d1])
    assert len(dsc) == len(blocks) + 1, (len(dsc), len(blocks))   # id 0 is reserved
    kept, out_bm, out_dsc, at = [], [], [dsc[0]], 0
    for (cp, comment, data), (_, rest) in zip(blocks, dsc[1:]):
        if HEBREW[0] <= cp <= HEBREW[1]:
            continue
        kept.append(cp)
        out_bm.append(comment + "".join(   # as lv_font_conv lays it out: 8 a line, a blank line after
            "    " + " ".join(x + "," for x in data[k:k + 8]) + "\n" for k in range(0, len(data), 8)) + "\n")
        out_dsc.append((str(at), rest))
        at += len(data)
    runs, gid = [], 1
    for cp in kept:
        if runs and cp == runs[-1][0] + runs[-1][1]:
            runs[-1][1] += 1
        else:
            runs.append([cp, 1, gid])
        gid += 1
    cmaps = ",\n".join(
        "    {\n        .range_start = %d, .range_length = %d, .glyph_id_start = %d,\n"
        "        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, "
        ".type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY\n    }" % tuple(r) for r in runs)
    c0 = src.index("/*Collect the unicode lists and glyph_id offsets*/")
    c1 = src.index("};", src.index("cmaps[] =", c0)) + 2
    u0 = src.find("static const uint16_t unicode_list_", d1)
    out = (src[:bm0 + len("glyph_bitmap[] = {")] + "\n" + "".join(out_bm) + src[bm1:d0]
           + "glyph_dsc[] = {\n" + ",\n".join(
               "    {.bitmap_index = %s, %s}%s" % (b, r, " /* id = 0 reserved */" if i == 0 else "")
               for i, (b, r) in enumerate(out_dsc)) + "\n"
           + src[d1:u0 if 0 <= u0 < c0 else c0]
           + "/*Collect the unicode lists and glyph_id offsets*/\nstatic const lv_font_fmt_txt_cmap_t cmaps[] =\n{\n"
           + cmaps + "\n};" + src[c1:])
    return re.sub(r"\.cmap_num = \d+", ".cmap_num = %d" % len(runs), out)


def main():
    with tempfile.TemporaryDirectory(prefix="sloppy-heb-") as temp:
        tool = Path(temp) / "ttf2lvgl"
        subprocess.run(["swiftc", "-O", str(ROOT / "sloppy-dev" / "ttf2lvgl.swift"), "-o", str(tool)], check=True)
        for style, size in UI:
            name, heb = f"ss_{style}_{size}", f"ss_heb_{style}_{size}"
            path = FONTS / f"{name}.c"
            src = path.read_text(encoding="utf-8")
            fb = re.search(r"\.fallback = &(\w+)", src).group(1)
            if fb == heb:                # done before: keep the chain it made
                fb = re.search(r"\.fallback = &(\w+)", (FONTS / f"{heb}.c").read_text()).group(1)
            if fb.startswith("ss_hebrew_"):   # the old big Hebrew, at the default weight
                fb = re.search(r"\.fallback = &(\w+)", (FONTS / f"{fb}.c").read_text()).group(1)
            env = dict(os.environ, TTF2LVGL_WGHT=str(WGHT[style]), TTF2LVGL_SCALE=str(SCALE))
            subprocess.run([str(tool), str(HEBREW_TTF), str(size), heb, "0x590-0x5FF",
                            str(FONTS / f"{heb}.c"), fb], check=True, env=env)
            if "/* U+05" in src:
                src = strip_hebrew(src)
            src = re.sub(r"(extern const lv_font_t )\w+;", r"\g<1>%s;" % heb, src, count=1)
            src = re.sub(r"\.fallback = &\w+", ".fallback = &%s" % heb, src, count=1)
            src = re.sub(r"\(Hebrew wght=\d+\)", "(Hebrew: %s, sloppy-dev/hebrew-fonts.py)" % heb, src, count=1)
            path.write_text(src, encoding="utf-8")
            print(f"{name}: Hebrew from {heb} ({WGHT[style]}, x{SCALE}), then {fb}")


if __name__ == "__main__":
    main()
