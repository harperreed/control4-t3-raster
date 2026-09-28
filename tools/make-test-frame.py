#!/usr/bin/env python3
# ABOUTME: Renders a 1280x800 orientation/colour test frame as PNG using only the Python stdlib.
# ABOUTME: Labelled corners, an UP arrow, colour bars and a timestamp, so a photo of the panel shows rotation and colours.
"""Usage: tools/make-test-frame.py [OUT.png] [--label TEXT] [--stamp TEXT] [--width W] [--height H]

Writes OUT.png (default test-frame.png) and prints its path. Push it with
tools/push-frame.sh. The corner blocks differ in colour AND size, so a photo
shows which logical corner landed where even if colours are wrong:

  TOP-LEFT     red,   largest       TOP-RIGHT     green
  BOTTOM-LEFT  blue                 BOTTOM-RIGHT  white, smallest

`png_bytes()` is also imported by tt7d/test_e2e.py, so the test and this tool
share one PNG writer.
"""
import argparse
import os
import re
import struct
import sys
import time
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT_H = os.path.join(ROOT, "third_party", "mmkeypad", "init", "font8x8_basic.h")


def png_bytes(width, height, pixels, channels=4):
    """Encode raw 8-bit pixels (RGBA if channels == 4, RGB if 3) as a PNG."""
    color_type = {3: 2, 4: 6}[channels]
    stride = width * channels
    if len(pixels) != stride * height:
        raise ValueError(f"expected {stride * height} bytes of pixels, got {len(pixels)}")
    raw = b"".join(b"\x00" + bytes(pixels[y * stride:(y + 1) * stride]) for y in range(height))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    ihdr = struct.pack(">IIBBBBB", width, height, 8, color_type, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(raw, 6))
            + chunk(b"IEND", b""))


def load_font():
    """The public-domain 8x8 font the probe uses: {char: [8 row bytes]}, bit n = column n."""
    glyphs = {}
    with open(FONT_H) as f:
        for m in re.finditer(r"\{((?:\s*0x[0-9A-Fa-f]{2},?){8})\s*\},\s*// U\+([0-9A-F]{4})", f.read()):
            glyphs[chr(int(m.group(2), 16))] = [int(b, 16) for b in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1))]
    return glyphs


class Canvas:
    def __init__(self, width, height):
        self.w, self.h = width, height
        self.px = bytearray(width * height * 3)
        self.font = load_font()

    def fill(self, x, y, w, h, rgb):
        x0, y0, x1, y1 = max(0, x), max(0, y), min(self.w, x + w), min(self.h, y + h)
        if x1 <= x0 or y1 <= y0:
            return
        row = bytes(rgb) * (x1 - x0)
        for yy in range(y0, y1):
            o = (yy * self.w + x0) * 3
            self.px[o:o + len(row)] = row

    def text_width(self, s, scale):
        return len(s) * 8 * scale

    def text(self, x, y, s, scale, rgb):
        for ch in s:
            rows = self.font.get(ch, self.font["?"])
            for r, bits in enumerate(rows):
                for c in range(8):
                    if bits & (1 << c):
                        self.fill(x + c * scale, y + r * scale, scale, scale, rgb)
            x += 8 * scale

    def text_centred(self, y, s, scale, rgb):
        self.text((self.w - self.text_width(s, scale)) // 2, y, s, scale, rgb)

    def arrow_up(self, cx, top, size, rgb):
        """A solid triangle pointing up plus a shaft."""
        for i in range(size):
            self.fill(cx - i, top + i, 2 * i + 1, 1, rgb)
        self.fill(cx - size // 4, top + size, size // 2, size, rgb)


def render(width, height, label, stamp=None):
    c = Canvas(width, height)
    white, black = (255, 255, 255), (0, 0, 0)
    c.fill(0, 0, width, height, (24, 24, 32))
    u = min(width, height) // 20  # 40 px at 1280x800

    corners = [  # name, colour, size in units, anchor
        ("TOP-LEFT", (255, 0, 0), 4, "tl"),
        ("TOP-RIGHT", (0, 255, 0), 3, "tr"),
        ("BOTTOM-LEFT", (0, 0, 255), 2, "bl"),
        ("BOTTOM-RIGHT", white, 1, "br"),
    ]
    ts = 3
    for name, rgb, units, anchor in corners:
        s = units * u
        x = 0 if anchor[1] == "l" else width - s
        y = 0 if anchor[0] == "t" else height - s
        c.fill(x, y, s, s, rgb)
        tw, th = c.text_width(name, ts), 8 * ts
        tx = x + s + u // 2 if anchor[1] == "l" else x - u // 2 - tw
        ty = y + u // 2 if anchor[0] == "t" else y + s - u // 2 - th
        c.text(tx, ty, name, ts, white)

    c.arrow_up(width // 2, u // 2, 2 * u, white)
    c.text_centred(u // 2 + 4 * u + u // 2, "UP: TOP EDGE OF THE IMAGE", ts, white)

    bars = [white, (255, 255, 0), (0, 255, 255), (0, 255, 0), (255, 0, 255), (255, 0, 0), (0, 0, 255),
            (128, 128, 128)]
    by, bh = height * 45 // 100, height // 8
    for i, rgb in enumerate(bars):
        x0, x1 = i * width // len(bars), (i + 1) * width // len(bars)
        c.fill(x0, by, x1 - x0, bh, rgb)
    ry = by + bh + u // 2
    for x in range(width):
        v = x * 255 // (width - 1)
        c.fill(x, ry, 1, u, (v, v, v))

    if stamp is None:
        stamp = time.strftime("%Y-%m-%d %H:%M:%S %Z")
    c.text_centred(by - 3 * u, f"{width}x{height}  {stamp}", 4, white)
    if label:
        c.text_centred(by - 3 * u + 6 * 8, label, 3, (255, 255, 0))
    c.text_centred(ry + u + u // 2, "BARS: W Y C G M R B GREY   RAMP: BLACK -> WHITE", 2, white)

    for x, y, w, h in ((0, 0, width, 1), (0, height - 1, width, 1), (0, 0, 1, height), (width - 1, 0, 1, height)):
        c.fill(x, y, w, h, white)
    c.fill(width // 2 - 1, height // 2 - 1, 3, 3, black)
    return png_bytes(width, height, c.px, channels=3)


def main():
    ap = argparse.ArgumentParser(description="Render a TT7 orientation/colour test frame (PNG, stdlib only).")
    ap.add_argument("out", nargs="?", default="test-frame.png", help="output path (default test-frame.png)")
    ap.add_argument("--label", default="", help="extra line of text, e.g. which rotation you are testing")
    ap.add_argument("--stamp", help="text in place of the current time (the build uses this for tt7d's built-in "
                    "pattern, so the same source gives the same PNG)")
    ap.add_argument("--width", type=int, default=1280)
    ap.add_argument("--height", type=int, default=800)
    args = ap.parse_args()
    data = render(args.width, args.height, args.label.upper(), args.stamp)
    with open(args.out, "wb") as f:
        f.write(data)
    print(args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
