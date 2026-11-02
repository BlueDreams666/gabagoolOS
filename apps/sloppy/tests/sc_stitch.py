#!/usr/bin/env python3
"""Joins the frames SLOPPY_SC_SHOTS writes into one tall PNG a page.
Plain Python (no PIL on this Mac): PPM in, PNG out.

  sc_stitch.py stitch <frames dir> <out dir>       every <page>-<look>-<tag>.txt
  sc_stitch.py compare <out dir>                   <page>-<look>-compare.png, before on the left
  sc_stitch.py sheet <out dir> <look> <tag> <png> <page,page,...>
                                                   every page in one look, side by side, half size

Each .txt line is "<frame.ppm> <scroll y> <body top>": the first frame
is kept whole, each later one adds the body under the header at its scroll."""
import sys, os, glob, zlib, struct

# An image is (w, h, rows): rows a list of bytes, 3 per pixel.
def read_ppm(path):
    d = open(path, 'rb').read()
    parts, i = [], 0
    while len(parts) < 4:
        while d[i:i + 1].isspace(): i += 1
        j = i
        while not d[j:j + 1].isspace(): j += 1
        parts.append(d[i:j]); i = j
    i += 1
    w, h = int(parts[1]), int(parts[2])
    return w, h, [d[i + y * w * 3:i + (y + 1) * w * 3] for y in range(h)]

def read_png(path):
    d = open(path, 'rb').read()
    i, idat, w, h = 8, b'', 0, 0
    while i < len(d):
        n, t = struct.unpack('>I4s', d[i:i + 8])
        c = d[i + 8:i + 8 + n]
        if t == b'IHDR': w, h = struct.unpack('>II', c[:8])
        elif t == b'IDAT': idat += c
        i += 12 + n
    raw, rows, s = zlib.decompress(idat), [], w * 3 + 1
    for y in range(h):
        assert raw[y * s] == 0        # written by write_png: no filter
        rows.append(raw[y * s + 1:(y + 1) * s])
    return w, h, rows

def write_png(path, img):
    w, h, rows = img
    def chunk(t, c): return struct.pack('>I', len(c)) + t + c + struct.pack('>I', zlib.crc32(t + c) & 0xffffffff)
    raw = b''.join(b'\0' + r for r in rows)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))

def stitch(src, out):
    os.makedirs(out, exist_ok=True)
    for txt in sorted(glob.glob(os.path.join(src, '*.txt'))):
        lines = [l.split() for l in open(txt) if l.strip()]
        if not lines: continue
        w, h, rows = read_ppm(lines[0][0])
        top = int(lines[0][2])
        rows = list(rows)
        for f, y, _ in lines[1:]:
            _, _, r = read_ppm(f)
            y = int(y)
            need = top + y + (h - top)
            if len(rows) < need: rows += [b'\0' * (w * 3)] * (need - len(rows))
            rows[top + y:top + y + h - top] = r[top:]
        write_png(os.path.join(out, os.path.basename(txt)[:-4] + '.png'), (w, len(rows), rows))

def side(imgs, gap, fill):
    H = max(i[1] for i in imgs)
    W = sum(i[0] for i in imgs) + gap * (len(imgs) - 1)
    g = bytes(fill) * gap
    rows = []
    for y in range(H):
        parts = []
        for w, h, r in imgs:
            parts.append(r[y] if y < h else bytes(fill) * w)
        rows.append(g.join(parts))
    return W, H, rows

def compare(out):
    for b in sorted(glob.glob(os.path.join(out, '*-before.png'))):
        a = b[:-len('-before.png')] + '-after.png'
        if not os.path.exists(a): continue
        write_png(b[:-len('-before.png')] + '-compare.png', side([read_png(b), read_png(a)], 24, (128, 128, 128)))

def half(img):
    w, h, rows = img
    out = []
    for y in range(0, h - 1, 2):
        r = rows[y]
        out.append(b''.join(r[x:x + 3] for x in range(0, w * 3, 6)))
    return w // 2, len(out), out

def sheet(out, look, tag, dest, pages):
    ims = [(p, half(read_png(os.path.join(out, f'{p}-{look}-{tag}.png')))) for p in pages
           if os.path.exists(os.path.join(out, f'{p}-{look}-{tag}.png'))]
    if not ims: return
    cols, gap = 8, 16
    bg = (210, 210, 214) if 'light' in look else (40, 40, 46)
    band = []
    for i in range(0, len(ims), cols):
        row = [im for _, im in ims[i:i + cols]]
        while len(row) < cols: row.append((240, 1, [bytes(bg) * 240]))
        w, h, r = side(row, gap, bg)
        band += r + [bytes(bg) * w] * gap
    W = len(band[0]) // 3
    edge = bytes(bg) * gap
    rows = [edge + bytes(bg) * W + edge] * gap + [edge + r + edge for r in band]
    write_png(dest, (W + 2 * gap, len(rows), rows))
    with open(dest[:-4] + '.txt', 'w') as k:
        for n, (p, _) in enumerate(ims):
            k.write(f'row {n // cols + 1}, column {n % cols + 1}: {p}\n')

if __name__ == '__main__':
    cmd = sys.argv[1]
    if cmd == 'stitch': stitch(sys.argv[2], sys.argv[3])
    elif cmd == 'compare': compare(sys.argv[2])
    elif cmd == 'sheet': sheet(sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5], sys.argv[6].split(','))
