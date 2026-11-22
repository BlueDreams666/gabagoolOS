#!/usr/bin/env python3
"""Name the addresses in a player's .rockbox/crash.log, or the slowest
timer in sloppy.log's lag and stall lines.

    tools/crashlog-symbols.py crash.log rockbox.elf
    tools/crashlog-symbols.py sloppy.log rockbox.elf

rockbox.elf (or, less precisely, rockbox.map) comes from the same build as
the rockbox.r1 that crashed: CI's "-symbols" artifact, or
rockbox-hibyr1-symbols.zip on a release. The player runs a stripped binary,
so its crash log holds bare addresses. The ELF names static functions too;
the map only knows global ones. For file and line, run
mipsel-rockbox-linux-gnu-addr2line -f -e rockbox.elf on an address.
"""
import bisect
import re
import struct
import sys


def load_elf(path):
    """Sorted (start, end, name) for every function in a 32-bit ELF."""
    data = open(path, "rb").read()
    if data[:4] != b"\x7fELF" or data[4] != 1:
        sys.exit("%s: not a 32-bit ELF" % path)
    e = "<" if data[5] == 1 else ">"
    shoff, = struct.unpack_from(e + "I", data, 32)
    shentsize, shnum = struct.unpack_from(e + "HH", data, 46)
    sections = [struct.unpack_from(e + "IIIIIIIIII", data, shoff + i * shentsize)
                for i in range(shnum)]
    spans = []
    for sh in sections:
        if sh[1] != 2:                      # SHT_SYMTAB
            continue
        strtab = sections[sh[6]]
        for off in range(sh[4], sh[4] + sh[5], 16):
            st_name, value, size, info = struct.unpack_from(e + "IIIB", data, off)
            if info & 0xf != 2 or not size:  # STT_FUNC
                continue
            s = strtab[4] + st_name
            spans.append((value, value + size, data[s:data.index(b"\0", s)].decode(errors="replace")))
    if not spans:
        sys.exit("%s: no symbols (stripped?)" % path)
    spans.sort(key=lambda s: s[0])
    return spans


def load_map(path):
    """Sorted (start, end, name) for every code section and symbol."""
    spans = []
    pending = None          # a section name too long to share its line
    in_text = False
    for line in open(path, errors="replace"):
        if line.startswith(".text"):
            in_text = True
        elif line.startswith(".") and not line.startswith(".text"):
            in_text = False
        if not in_text:
            continue
        m = re.match(r"^ (\.text\.?\S*)\s*$", line)
        if m:
            pending = m.group(1)
            continue
        m = re.match(r"^ (\.text\.?\S*)?\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+)", line)
        if m:
            section = m.group(1) or pending
            pending = None
            start, size = int(m.group(2), 16), int(m.group(3), 16)
            name = section[6:] if section and section.startswith(".text.") else ""
            if size:
                spans.append((start, start + size, name or m.group(4).rsplit("/", 1)[-1]))
            continue
        pending = None
        m = re.match(r"^\s+0x([0-9a-f]+)\s+([A-Za-z_]\w*)\s*$", line)
        if m:
            spans.append((int(m.group(1), 16), None, m.group(2)))
    spans.sort(key=lambda s: s[0])
    return spans


def name(spans, starts, addr):
    i = bisect.bisect_right(starts, addr) - 1
    # the nearest symbol at or below, inside a section that holds it
    while i >= 0:
        start, end, sym = spans[i]
        if end is None or addr < end:
            return "%s+0x%x" % (sym, addr - start)
        i -= 1
    return "?"


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__.strip())
    spans = (load_map if sys.argv[2].endswith(".map") else load_elf)(sys.argv[2])
    starts = [s[0] for s in spans]
    for line in open(sys.argv[1], errors="replace"):
        line = line.rstrip("\n")
        if line.startswith("=== crash"):
            print(line)
            for key in ("pc", "ra"):
                m = re.search(r"\b%s (0x[0-9a-f]+)" % key, line)
                if m:
                    print("  %-3s %s  %s" % (key, m.group(1), name(spans, starts, int(m.group(1), 16))))
        elif line.startswith("version "):
            print("  " + line)
        elif re.search(r"timer 0x[0-9a-f]+", line):   # a lag or stall line in sloppy.log
            print(re.sub(r"timer (0x[0-9a-f]+)", lambda m: "timer %s" % name(spans, starts, int(m.group(1), 16)), line))
        elif line.startswith("stack:"):
            print("  called from (newest first; a few may be stale):")
            for a in line.split()[1:]:
                print("      %s  %s" % (a, name(spans, starts, int(a, 16))))


if __name__ == "__main__":
    main()
