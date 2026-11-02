#!/usr/bin/env python3
"""mkpage.py: remote.html -> apps/sloppy/sloppy_remote_page.h (the phone
remote's page as one C string). Edit the HTML, run this, commit both."""
from pathlib import Path
here = Path(__file__).resolve().parent
out = here.parents[1] / 'apps/sloppy/sloppy_remote_page.h'
lines = [l.strip() for l in (here / 'remote.html').read_text(encoding='utf-8').splitlines()]
lines = [l for l in lines if l]
def c(s):
    b = s.encode('utf-8')
    r = ''
    for ch in b:
        if ch == 0x22: r += '\\"'
        elif ch == 0x5c: r += '\\\\'
        elif 32 <= ch < 127: r += chr(ch)
        else: r += '\\x%02x""' % ch
    return '"' + r + '\\n"'
body = '\n'.join('    ' + c(l) for l in lines)
out.write_text('''/* GabagoolOS internals. GPLv2+. */
/* The phone remote's page (sloppy_net.c serves it at /), made by
 * sloppy-dev/remote/mkpage.py from remote.html: edit that, not this. One
 * file, no fonts or scripts from anywhere else; it asks the player for the
 * rest, its manifest and icons too. */
#ifndef SLOPPY_REMOTE_PAGE_H
#define SLOPPY_REMOTE_PAGE_H
static const char sl_remote_html[] =
%s;
#endif
''' % body, encoding='utf-8')
n = sum(len(l.encode()) + 1 for l in lines)
print(out, n, 'bytes')
