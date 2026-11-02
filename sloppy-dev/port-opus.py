#!/usr/bin/env python3
"""Copy Rockbox's Opus decoder (lib/rbcodec/codecs/libopus) into
apps/sloppy/opus for the internet radio, as tremor/ and flac/ were.

The decoder's sources from libopus/SOURCES (CELT, SILK and the Opus layer;
not the Ogg framing or opus_header.c: the radio reads Ogg pages and
OpusHead itself), and every header they include, keep their celt/ and silk/
folders. Each #include "x.h" that the library found through -I paths is
rewritten to a path relative to the file, since the interface builds every
file with the same flags. config.h is replaced by opus/config.h (the shim
the copy carries); the Rockbox-only parts (logf, the decoder in a static
buffer) are taken out. Run from the repository root; the copy is replaced.
"""
import os, re, shutil, sys

SRC = 'lib/rbcodec/codecs/libopus'
DST = 'apps/sloppy/opus'
SKIP = {'ogg/framing.c', 'opus_header.c'}
DIRS = ['', 'celt', 'silk']           # libopus.make's -I order
PLATFORM = {'arm', 'cf', 'x86', 'mips', 'fixed', 'float'}   # optimisations the R1 build never turns on


def sources():
    out = []
    for line in open(os.path.join(SRC, 'SOURCES')):
        line = line.strip()
        if line and not line.startswith('/*') and not line.startswith('#') and line not in SKIP:
            out.append(line)
    return out


INC = re.compile(r'^(\s*#\s*include\s*)"([^"]+)"', re.M)


def find(name, here):
    for d in [here] + DIRS:
        p = os.path.normpath(os.path.join(d, name))
        if os.path.isfile(os.path.join(SRC, p)):
            return p
    return None


def main():
    if not os.path.isdir(SRC):
        sys.exit('run from the repository root')
    keep = {}                          # path -> text
    todo = sources()
    for s in todo:
        if not os.path.isfile(os.path.join(SRC, s)):
            sys.exit('missing ' + s)
    while todo:
        p = todo.pop()
        if p in keep:
            continue
        text = open(os.path.join(SRC, p), encoding='latin-1').read()
        here = os.path.dirname(p)

        def fix(m):
            name = m.group(2)
            if name == 'config.h':
                return m.group(1) + '"' + os.path.relpath('config.h', here or '.') + '"'
            if name == 'logf.h':
                return '/* logf.h: not in the interface */'
            f = find(name, here)
            if f is None or f.split('/')[1:2] and f.split('/')[1] in PLATFORM or 'static_modes_float' in f:
                return m.group(0)      # a platform header, or float tables, behind an #if: left out
            if f not in keep:
                todo.append(f)
            return m.group(1) + '"' + os.path.relpath(f, here or '.') + '"'
        text = INC.sub(fix, text)
        # the interface defines no HAVE_CONFIG_H: config.h always
        text = re.sub(r'#ifdef HAVE_CONFIG_H[ \t]*\n([ \t]*#[ \t]*include "[./]*config\.h")[ \t]*\n#endif[^\n]*',
                      r'\1', text)
        text = re.sub(r'ifdef HAVE_CONFIG_H\b', 'if 1 /* config.h */', text)
        text = text.replace('defined(HAVE_CONFIG_H)', '1 /* config.h */')
        keep[p] = text

    d = keep['opus_decoder.c']
    d = d.replace('#define DEBUG\n#define LOGF_ENABLE\n', '')
    a = d.index('/* Rockbox optimization */')
    b = d.index('static void smooth_fade(')
    d = d[:a] + d[b:]                  # the static decoder: the radio allocates its own
    keep['opus_decoder.c'] = d

    if os.path.isdir(DST):
        for f in os.listdir(DST):
            if f not in ('config.h', 'README.gabagool'):
                q = os.path.join(DST, f)
                shutil.rmtree(q) if os.path.isdir(q) else os.remove(q)
    for p, text in sorted(keep.items()):
        q = os.path.join(DST, p)
        os.makedirs(os.path.dirname(q) or DST, exist_ok=True)
        open(q, 'w', encoding='latin-1').write(text)
    print('%d files, %d sources' % (len(keep), sum(p.endswith('.c') for p in keep)))
    for p in sorted(keep):
        if p.endswith('.c'):
            print('sloppy/opus/' + p)


if __name__ == '__main__':
    main()
