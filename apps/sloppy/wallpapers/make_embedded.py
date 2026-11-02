"""Crop source images and embed 480x800 RGB565 wallpaper and preview arrays."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


def read_bmp(path: Path):
    data = path.read_bytes()
    if data[:2] != b"BM":
        raise ValueError(f"not a BMP: {path}")
    offset = struct.unpack_from("<I", data, 10)[0]
    width, height, planes, depth = struct.unpack_from("<iiHH", data, 18)
    if planes != 1 or depth not in (24, 32) or width != 480 or abs(height) != 800:
        raise ValueError(f"expected 480x800 24/32-bit BMP: {path}")
    stride = ((width * depth + 31) // 32) * 4
    pixels = []
    for y in range(800):
        sy = y if height < 0 else 799 - y
        row = offset + sy * stride
        for x in range(480):
            b, g, r = data[row + x * (depth // 8):row + x * (depth // 8) + 3]
            pixels.append(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))
    return pixels


def emit(name: str, pixels):
    out = Path(__file__).parent / f"{name}.inc"
    with out.open("w") as f:
        f.write(f"/* Generated from the {name} source image; 480x800 RGB565. */\n")
        f.write(f"static const uint16_t wallpaper_{name}[480 * 800] = {{\n")
        for i in range(0, len(pixels), 16):
            f.write("    " + ", ".join(f"0x{p:04x}" for p in pixels[i:i + 16]) + ",\n")
        f.write("};\n")
        f.write(f"static const uint16_t wallpaper_{name}_preview[60 * 100] = {{\n")
        for y in range(100):
            row = [pixels[(y * 8 + 4) * 480 + x * 8 + 4] for x in range(60)]
            for i in range(0, 60, 15):
                f.write("    " + ", ".join(f"0x{p:04x}" for p in row[i:i + 15]) + ",\n")
        f.write("};\n")


if __name__ == "__main__":
    sources = {
        "orion": ("orion.jpg", (1067, 640)),
        "pillars": ("pillars.png", (1925, 1155)),
        "red_roots": ("red_roots.png", None),
        "nexus_background": ("nexus_background.png", (800, 480)),
    }
    for name in sys.argv[1:]:
        filename, crop = sources[name]
        with tempfile.TemporaryDirectory() as directory:
            source = Path(__file__).parent / filename
            if crop:
                cropped = Path(directory) / ("crop" + source.suffix)
                subprocess.run(["sips", "-c", str(crop[0]), str(crop[1]),
                                str(source), "--out", str(cropped)], check=True,
                               stdout=subprocess.DEVNULL)
                source = cropped
            bmp = Path(directory) / "wallpaper.bmp"
            subprocess.run(["sips", "-z", "800", "480", "-s", "format", "bmp",
                            str(source), "--out", str(bmp)], check=True,
                           stdout=subprocess.DEVNULL)
            emit(name, read_bmp(bmp))
