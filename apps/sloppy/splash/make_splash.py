"""Compose the 480x800 start-up splash from the GabagoolOS logo and embed it.

The logo's paper is taken out and replaced by deep ink; the wordmark, the pool
photo in its frame and the "Made by" stamp keep their own colours (the stamp
turns sage so it reads on ink). Under them, a pool-blue halftone, like the
photo's print, rises from the bottom. Writes splash.png (also the picture for
the boot menu) and splash.inc (RGB565, drawn by the in-app splash).

    python3 make_splash.py ../../../../branding/gabagoolos-logo.webp
"""
from pathlib import Path
import math
import struct
import subprocess
import sys
import tempfile
import zlib

W, H = 480, 800
INK, DEEP = (0x10, 0x16, 0x1d), (0x0b, 0x26, 0x34)
POOL, SAGE = (0x4f, 0xb3, 0xd9), (0xd6, 0xe6, 0xcf)


def read_bmp(path):
    d = Path(path).read_bytes()
    off = struct.unpack_from("<I", d, 10)[0]
    w, h, _, bpp = struct.unpack_from("<iiHH", d, 18)
    st, b = ((w * bpp + 31) // 32) * 4, bpp // 8
    rows = []
    for y in range(abs(h)):
        i = off + (y if h < 0 else abs(h) - 1 - y) * st
        rows.append([(d[i + x * b + 2], d[i + x * b + 1], d[i + x * b]) for x in range(w)])
    return rows


def mix(a, b, t):
    return tuple(a[k] + (b[k] - a[k]) * t for k in range(3))


def letter_alpha(p):
    """How much of a pixel off the photo is lettering rather than paper."""
    r, g, b = p
    if r + g + b < 150:
        return 1.0                                      # the letters' dark outline
    sage = (g - r - 3) / 12                             # pale sage leans green; paper leans warm
    lemon = (r - b - 45) / 45                           # lemon is short of blue
    return max(0.0, min(1.0, max(sage, lemon)))


def background(x, y):
    t = y / (H - 1)
    c = mix(INK, DEEP, t * t)
    # halftone on a 45 degree grid, dots growing toward the bottom
    s = 10
    u, v = (x + y) / s, (x - y) / s
    du, dv = u - round(u), v - round(v)
    d = math.hypot(du, dv) * s / math.sqrt(2)
    rad = max(0.0, (t - 0.5) / 0.5) * 3.2
    cov = max(0.0, min(1.0, rad - d + 0.5))
    return mix(c, POOL, cov * 0.30)


def box_scale(src, x0, y0, x1, y1, w, h):
    """Area-average src[y0:y1][x0:x1] (RGBA) down to w x h."""
    out = []
    fx, fy = (x1 - x0) / w, (y1 - y0) / h
    for oy in range(h):
        ya, yb = y0 + int(oy * fy), max(y0 + int(oy * fy) + 1, y0 + int((oy + 1) * fy))
        row = []
        for ox in range(w):
            xa, xb = x0 + int(ox * fx), max(x0 + int(ox * fx) + 1, x0 + int((ox + 1) * fx))
            acc = [0.0, 0.0, 0.0, 0.0]
            n = 0
            for yy in range(ya, yb):
                line = src[yy]
                for xx in range(xa, xb):
                    r, g, b, a = line[xx]
                    acc[0] += r * a; acc[1] += g * a; acc[2] += b * a; acc[3] += a
                    n += 1
            a = acc[3] / n
            row.append(((acc[0] / acc[3], acc[1] / acc[3], acc[2] / acc[3]) if acc[3] else (0, 0, 0), a))
        out.append(row)
    return out


def main(logo):
    with tempfile.TemporaryDirectory() as tmp:
        bmp = Path(tmp) / "logo.bmp"
        subprocess.run(["sips", "-s", "format", "bmp", str(logo), "--out", str(bmp)],
                       check=True, stdout=subprocess.DEVNULL)
        src = read_bmp(bmp)
    n = len(src)
    # the photo's frame: the dark rule through the middle row and column
    mid = src[n // 2]
    fx0 = next(x for x in range(n) if sum(mid[x]) < 120)
    fx1 = max(x for x in range(n) if sum(mid[x]) < 120)
    fy1 = max(y for y in range(n) if sum(src[y][n // 2]) < 120)
    fy0 = next(y for y in range(n) if all(sum(src[y + k][fx0 + 2]) < 120 for k in range(20)))
    # the stamp sits below the frame on the left, the wordmark above it
    keyed = []
    for y in range(n):
        row = []
        for x in range(n):
            p = src[y][x]
            if fx0 <= x <= fx1 and fy0 <= y <= fy1:
                row.append((*p, 1.0))
            elif y > fy1:
                a = max(0.0, min(1.0, (300 - sum(p)) / 160))   # the stamp's dark ink, in sage
                row.append((*SAGE, a))
            else:
                row.append((*p, letter_alpha(p)))
        keyed.append(row)
    top = next(y for y in range(fy0) if sum(1 for x in range(fx0, fx1) if keyed[y][x][3] > 0.5) > 6)
    lw = 432
    lh = round((fy1 + 1 - top) * lw / (fx1 + 1 - fx0))
    pad = round(8 * (fx1 + 1 - fx0) / lw)               # the wordmark runs a little past the frame
    logo_px = box_scale(keyed, fx0 - pad, top, fx1 + 1 + pad, fy1 + 1, lw + 16, lh)
    sx = [x for x in range(fx0) if any(keyed[y][x][3] > 0.6 for y in range(fy1 + 4, n - 4))]
    sy = [y for y in range(fy1 + 4, n - 4) if any(keyed[y][x][3] > 0.6 for x in range(0, fx0 + 400))]
    st_x0, st_x1, st_y0, st_y1 = min(sx) - 4, max(x for y in sy for x in range(0, fx0 + 400) if keyed[y][x][3] > 0.6) + 5, min(sy) - 4, max(sy) + 5
    sw = 150
    shh = round((st_y1 - st_y0) * sw / (st_x1 - st_x0))
    stamp = box_scale(keyed, st_x0, st_y0, st_x1, st_y1, sw, shh)

    ly = 168
    sty = H - 70 - shh
    img = []
    for y in range(H):
        row = []
        for x in range(W):
            c = background(x, y)
            lx, lyy = x - (W - lw - 16) // 2, y - ly
            if 0 <= lx < lw + 16 and 0 <= lyy < lh:
                p, a = logo_px[lyy][lx]
                c = mix(c, p, a)
            gx, gy = x - (W - sw) // 2, y - sty
            if 0 <= gx < sw and 0 <= gy < shh:
                p, a = stamp[gy][gx]
                c = mix(c, p, a * 0.85)
            row.append(tuple(max(0, min(255, round(v))) for v in c))
        img.append(row)

    here = Path(__file__).parent
    raw = b"".join(b"\0" + bytes(v for p in r for v in p) for r in img)
    chunk = lambda t, b: struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b) & 0xffffffff)
    (here / "splash.png").write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
                                      + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))
    px = [((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3) for row in img for r, g, b in row]
    with (here / "splash.inc").open("w") as f:
        f.write("/* Generated by make_splash.py from the GabagoolOS logo; 480x800 RGB565. */\n")
        f.write("static const uint16_t splash_art[480 * 800] = {\n")
        for i in range(0, len(px), 16):
            f.write("    " + ", ".join(f"0x{p:04x}" for p in px[i:i + 16]) + ",\n")
        f.write("};\n")


if __name__ == "__main__":
    main(sys.argv[1])
