#!/usr/bin/env python3
"""Draws the README's face pictures with the firmware's own drawing code (firmware/src/face.c).

Builds tests/face_preview.c with the host C compiler, runs it in its "readme" mode and writes one PNG per picture to
docs/images/faces/. Needs only Python and a C compiler (cc, gcc or clang). Run it again after changing a face.
"""

import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "docs", "images", "faces")
SCALE = 4  # screen pixels per OLED pixel
PAD = 16  # dark glass around the picture, in screen pixels
LIT, GLASS, RIM = (0xe6, 0xf3, 0xff), (0x0b, 0x0d, 0x10), (0x2a, 0x2e, 0x34)


def frames():
    cc = next((c for c in ("cc", "gcc", "clang") if shutil.which(c)), None)
    if cc is None:
        sys.exit("no C compiler found (cc, gcc or clang)")
    src = os.path.join(ROOT, "firmware", "src")
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "face_preview.exe" if sys.platform == "win32" else "face_preview")
        subprocess.run([cc, "-Wall", "-I", src] + [os.path.join(src, f) for f in ("face.c", "mood.c", "buddy_cfg.c", "battery.c")]
                       + [os.path.join(ROOT, "tests", "face_preview.c"), "-o", exe], check=True)
        out = subprocess.run([exe, "readme"], capture_output=True, text=True, check=True).stdout
    for block in out.split("--- ")[1:]:
        name, *rows = block.strip().split("\n")
        yield name.strip(), rows


def png(rows):
    w, h = len(rows[0]) * SCALE + 2 * PAD, len(rows) * SCALE + 2 * PAD
    lines = []
    for y in range(h):
        line = bytearray(b"\0")  # filter type 0
        oy = (y - PAD) // SCALE if PAD <= y < h - PAD else None
        for x in range(w):
            ox = (x - PAD) // SCALE if PAD <= x < w - PAD else None
            if x in (0, w - 1) or y in (0, h - 1):
                line += bytes(RIM)
            elif oy is not None and ox is not None and rows[oy][ox] == "#":
                line += bytes(LIT)
            else:
                line += bytes(GLASS)
        lines.append(bytes(line))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(b"".join(lines), 9)) + chunk(b"IEND", b""))


def main():
    os.makedirs(OUT, exist_ok=True)
    for name, rows in frames():
        path = os.path.join(OUT, name + ".png")
        with open(path, "wb") as f:
            f.write(png(rows))
        print(os.path.relpath(path, ROOT))


if __name__ == "__main__":
    main()
