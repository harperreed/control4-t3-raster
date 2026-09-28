#!/usr/bin/env python3
# ABOUTME: End-to-end host test of `tt7cam convert`: synthetic NV12 file in, JPEG decoded by Pillow out.
# ABOUTME: Run via `uv run --with pillow==12.3.0 python3 cam/test_convert.py --tool build/host/tt7cam` (make check).
import argparse
import io
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image

# BT.601 limited-range (Y, Cb, Cr) and the RGB each should decode to.
BANDS = [
    ((81, 90, 240), (254, 0, 0)),     # red
    ((145, 54, 34), (0, 255, 1)),     # green
    ((41, 240, 110), (0, 0, 255)),    # blue
    ((128, 128, 128), (130, 130, 130)),  # mid-grey
]
W, H = 256, 128
TOLERANCE = 6  # JPEG at q95 with 4:2:0 chroma; measured away from band edges

failures = 0


def check(cond, msg):
    global failures
    if not cond:
        failures += 1
        print(f"FAIL {msg}", file=sys.stderr)


def synthetic_nv12():
    band_of = [x * len(BANDS) // W for x in range(W)]
    y_plane = bytes(BANDS[band_of[x]][0][0] for _ in range(H) for x in range(W))
    uv_row = bytearray()
    for x in range(0, W, 2):
        _, cb, cr = BANDS[band_of[x]][0]
        uv_row += bytes((cb, cr))
    return y_plane + bytes(uv_row) * (H // 2)


def run(tool, *args):
    return subprocess.run([tool, *args], capture_output=True, text=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tool", required=True)
    tool = ap.parse_args().tool

    with tempfile.TemporaryDirectory() as d:
        d = Path(d)
        raw, jpg = d / "in.nv12", d / "out.jpg"
        raw.write_bytes(synthetic_nv12())

        r = run(tool, "convert", str(raw), str(jpg), "--width", str(W), "--height", str(H), "--quality", "95")
        check(r.returncode == 0, f"convert exit {r.returncode}: {r.stderr.strip()}")
        check(not (d / "out.jpg.tmp").exists(), "temp file left behind")
        if jpg.exists():
            img = Image.open(io.BytesIO(jpg.read_bytes()))
            check(img.format == "JPEG", f"format {img.format}")
            check(img.size == (W, H), f"size {img.size}")
            img = img.convert("RGB")
            band_w = W // len(BANDS)
            for i, (_, want) in enumerate(BANDS):
                x0, x1 = i * band_w + 12, (i + 1) * band_w - 12
                px = [img.getpixel((x, y)) for y in range(8, H - 8, 4) for x in range(x0, x1, 4)]
                mean = [sum(p[c] for p in px) / len(px) for c in range(3)]
                ok = all(abs(mean[c] - want[c]) <= TOLERANCE for c in range(3))
                check(ok, f"band {i}: mean RGB {[round(m, 1) for m in mean]}, want {want} +-{TOLERANCE}")

        # A file of the wrong size is refused, and no JPEG appears.
        short = d / "short.nv12"
        short.write_bytes(synthetic_nv12()[:-1])
        bad = d / "bad.jpg"
        r = run(tool, "convert", str(short), str(bad), "--width", str(W), "--height", str(H))
        check(r.returncode == 1 and not bad.exists(), f"short input: exit {r.returncode}, jpg exists {bad.exists()}")

        r = run(tool, "convert", str(raw), str(bad))
        check(r.returncode == 2, f"convert without --width/--height: exit {r.returncode}")
        r = run(tool, "snap")
        check(r.returncode == 2, f"snap without OUT: exit {r.returncode}")
        r = run(tool, "snap", "x.jpg", "--width", "1279")
        check(r.returncode == 2, f"odd width: exit {r.returncode}")
        r = run(tool, "bogus")
        check(r.returncode == 2, f"unknown command: exit {r.returncode}")

    if failures:
        print(f"cam convert e2e: {failures} check(s) failed", file=sys.stderr)
        return 1
    print("  ok   cam convert e2e: NV12 file -> tt7cam convert -> JPEG decoded by Pillow")
    return 0


if __name__ == "__main__":
    sys.exit(main())
