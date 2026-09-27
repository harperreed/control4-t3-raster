#!/usr/bin/env python3
# ABOUTME: Verify an AOSP boot image for the TT7: magic, size, and the embedded Rockchip SHA1 id.
# ABOUTME: Import it for load()/verify(), or run `bootimg.py verify <img>` (exit 0 = good) from shell scripts.
"""Boot image checks shared by `make check` and scripts/flash-boot.sh.

The id recipe is MMKeypad's compute_id() (third_party/mmkeypad/tools/mkbootimg.py),
imported rather than copied so there is one definition of it.
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "third_party", "mmkeypad", "tools"))
import mkbootimg  # noqa: E402  (path set up above)

BOOT_PARTITION_BYTES = 0x6000 * 512  # mtdparts: 0x00006000@0x0000a000(boot) = 12582912
ID_OFFSET = 576                      # 8 magic + 10*4 fields + 16 name + 512 cmdline
ID_FIELD_LEN = 32                    # id[8] uint32; the SHA1 uses the first 20 bytes


def pages(n, page_size):
    return (n + page_size - 1) // page_size


def load(path):
    """Parse an image into its header fields and sections."""
    with open(path, "rb") as f:
        data = f.read()
    if data[0:8] != mkbootimg.MAGIC:
        raise ValueError(f"{path}: no ANDROID! magic (got {data[0:8]!r})")
    h = mkbootimg.parse_header(data)
    ps = h["page_size"]
    if ps == 0 or ps & (ps - 1):
        raise ValueError(f"{path}: bad page size {ps}")
    k_off = ps
    r_off = k_off + pages(h["kernel_size"], ps) * ps
    s_off = r_off + pages(h["ramdisk_size"], ps) * ps
    end = s_off + pages(h["second_size"], ps) * ps
    if end > len(data):
        raise ValueError(f"{path}: sections end at {end}, file is {len(data)} bytes")
    return {
        "path": path,
        "size": len(data),
        "header": h,
        "kernel": data[k_off:k_off + h["kernel_size"]],
        "ramdisk": data[r_off:r_off + h["ramdisk_size"]],
        "second": data[s_off:s_off + h["second_size"]],
        "id_field": data[ID_OFFSET:ID_OFFSET + ID_FIELD_LEN],
    }


def expected_id(img):
    h = img["header"]
    return mkbootimg.compute_id(img["kernel"], img["ramdisk"], img["second"], h["second_size"],
                                h["tags_addr"], h["page_size"], h["name"], h["cmdline"])


def verify(path, max_size=BOOT_PARTITION_BYTES):
    """Return (ok, list of human-readable findings)."""
    try:
        img = load(path)
    except (OSError, ValueError, SystemExit) as e:
        return False, [f"FAIL {e}"]
    notes, ok = [], True
    if img["size"] > max_size:
        ok = False
        notes.append(f"FAIL size {img['size']} > boot partition {max_size}")
    else:
        notes.append(f"ok   size {img['size']} <= {max_size}")
    want = expected_id(img)
    got = img["id_field"]
    if got[:20] != want or any(got[20:]):
        ok = False
        notes.append(f"FAIL id {got.hex()} != compute_id {want.hex()} + 12 zero bytes")
    else:
        notes.append(f"ok   id {want.hex()} matches compute_id")
    h = img["header"]
    notes.append(f"     kernel {h['kernel_size']} B, ramdisk {h['ramdisk_size']} B, "
                 f"second {h['second_size']} B, page {h['page_size']}")
    return ok, notes


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    v = sub.add_parser("verify", help="check magic, size and embedded id; exit 1 on any failure")
    v.add_argument("image")
    v.add_argument("--max-size", type=int, default=BOOT_PARTITION_BYTES,
                   help=f"largest allowed file size in bytes (default {BOOT_PARTITION_BYTES})")
    args = ap.parse_args()
    ok, notes = verify(args.image, args.max_size)
    print(f"{args.image}:")
    for n in notes:
        print(f"  {n}")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
